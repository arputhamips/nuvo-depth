#include "depthd/image_encoder.hpp"
#include "depthd/configuration_revision.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include <yaml-cpp/yaml.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace depthd::encoding {
namespace {

[[nodiscard]] std::string av_error(const int code) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
  av_strerror(code, text.data(), text.size());
  return text.data();
}

[[nodiscard]] std::string uuid(const std::array<std::uint8_t, 16>& value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(36U);
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 4U || index == 6U || index == 8U || index == 10U) {
      result.push_back('-');
    }
    result.push_back(digits[value[index] >> 4U]);
    result.push_back(digits[value[index] & 0x0fU]);
  }
  return result;
}

[[nodiscard]] StreamMode stream_mode(const std::string_view value) {
  if (value == "raw") return StreamMode::kRawOnly;
  if (value == "encoded") return StreamMode::kEncodedOnly;
  if (value == "both") return StreamMode::kBoth;
  throw std::invalid_argument("encoding.mode must be raw, encoded, or both");
}

[[nodiscard]] Codec codec(const std::string_view value) {
  if (value == "jpeg") return Codec::kJpeg;
  if (value == "h264") return Codec::kH264;
  throw std::invalid_argument("encoding.codec must be jpeg or h264");
}

[[nodiscard]] Source source(const std::string_view value) {
  if (value == "left_rectified") return Source::kLeftRectified;
  if (value == "right_rectified") return Source::kRightRectified;
  if (value == "left_color") return Source::kLeftColor;
  if (value == "right_color") return Source::kRightColor;
  if (value == "disparity") return Source::kDisparity;
  if (value == "depth") return Source::kDepth;
  if (value == "left_mono") return Source::kLeftMono;
  if (value == "right_mono") return Source::kRightMono;
  if (value == "left_rgb") return Source::kLeftRgb;
  if (value == "right_rgb") return Source::kRightRgb;
  if (value == "left_rectified_rgb") return Source::kLeftRectifiedRgb;
  if (value == "right_rectified_rgb") return Source::kRightRectifiedRgb;
  if (value == "rectified_stereo") return Source::kRectifiedStereo;
  if (value == "rectified_stereo_rgb") return Source::kRectifiedStereoRgb;
  throw std::invalid_argument(
      "encoding.source must name a supported image product");
}

template <class Value>
[[nodiscard]] Value required(const YAML::Node& root, const char* key) {
  const auto node = root[key];
  if (!node || !node.IsScalar()) {
    throw std::invalid_argument(std::string("encoding.") + key +
                                " is required");
  }
  try {
    return node.as<Value>();
  } catch (const YAML::Exception&) {
    throw std::invalid_argument(std::string("encoding.") + key +
                                " has the wrong type");
  }
}

[[nodiscard]] std::string frame_metadata_json(const EncodedFrame& frame) {
  std::ostringstream output;
  output << "{\"schema_version\":1,\"capture_epoch\":"
         << frame.identity.capture_epoch << ",\"capture_sequence\":"
         << frame.identity.capture_sequence << ",\"capture_time_ns\":"
         << frame.identity.capture_time_ns << ",\"capture_clock\":"
         << static_cast<unsigned>(frame.identity.capture_clock)
         << ",\"configuration_epoch\":"
         << frame.identity.configuration_epoch << ",\"calibration_sha256\":\""
         << calibration_identity_to_sha256(frame.calibration)
         << "\",\"calibration_authority\":"
         << static_cast<unsigned>(frame.calibration_authority)
         << ",\"encoding_configuration_generation\":" << frame.encoding_configuration_generation
         << ",\"encoding_applied_revision\":\"" << frame.encoding_applied_revision
         << "\",\"device_uuid\":\""
         << uuid(frame.identity.device_uuid) << "\",\"run_uuid\":\""
         << uuid(frame.identity.run_uuid) << "\",\"source\":\""
         << name(frame.source) << "\",\"codec\":\"" << name(frame.codec)
         << "\",\"mime_type\":\"" << frame.mime_type
         << "\",\"codec_string\":\"" << frame.codec_string
         << "\",\"width\":" << frame.width << ",\"height\":"
         << frame.height << ",\"keyframe\":"
         << (frame.keyframe ? "true" : "false") << ",\"encode_ms\":"
         << frame.encode_ms << '}';
  return output.str();
}

void put_u16(std::byte* target, const std::uint16_t value) {
  target[0] = static_cast<std::byte>((value >> 8U) & 0xffU);
  target[1] = static_cast<std::byte>(value & 0xffU);
}

void put_u32(std::byte* target, const std::uint32_t value) {
  target[0] = static_cast<std::byte>((value >> 24U) & 0xffU);
  target[1] = static_cast<std::byte>((value >> 16U) & 0xffU);
  target[2] = static_cast<std::byte>((value >> 8U) & 0xffU);
  target[3] = static_cast<std::byte>(value & 0xffU);
}

void put_u64(std::byte* target, const std::uint64_t value) {
  for (std::size_t index = 0; index < 8U; ++index) {
    target[index] =
        static_cast<std::byte>((value >> ((7U - index) * 8U)) & 0xffU);
  }
}

struct SourcePlane {
  std::array<const std::uint8_t*, 4> data{};
  std::array<int, 4> linesize{};
  AVPixelFormat pixel_format{AV_PIX_FMT_GRAY8};
  std::uint32_t width{};
  std::uint32_t height{};
  std::vector<std::uint8_t> storage;
};

[[nodiscard]] SourcePlane source_plane(const FrameSetStorage& frame,
                                       const Source selected) {
  SourcePlane result;
  std::size_t available{};
  const auto rectified_format = [&](const ImageProductFormat format) {
    switch (format) {
      case ImageProductFormat::kGray8:
        result.pixel_format = AV_PIX_FMT_GRAY8;
        break;
      case ImageProductFormat::kBgr8:
        result.pixel_format = AV_PIX_FMT_BGR24;
        result.linesize[0] *= 3;
        break;
      case ImageProductFormat::kRgb8:
        result.pixel_format = AV_PIX_FMT_RGB24;
        result.linesize[0] *= 3;
        break;
      default:
        throw std::invalid_argument("selected rectified source format is invalid");
    }
  };
  switch (selected) {
    case Source::kLeftRectified:
      result.data[0] = frame.left_rectified.data();
      result.width = frame.metadata.left_rectified.grid.width;
      result.height = frame.metadata.left_rectified.grid.height;
      result.linesize[0] =
          static_cast<int>(frame.metadata.left_rectified.grid.width);
      rectified_format(frame.metadata.left_rectified.format);
      available = frame.left_rectified.size();
      break;
    case Source::kRightRectified:
      result.data[0] = frame.right_rectified.data();
      result.width = frame.metadata.right_rectified.grid.width;
      result.height = frame.metadata.right_rectified.grid.height;
      result.linesize[0] =
          static_cast<int>(frame.metadata.right_rectified.grid.width);
      rectified_format(frame.metadata.right_rectified.format);
      available = frame.right_rectified.size();
      break;
    case Source::kLeftColor:
    case Source::kRightColor: {
      const auto& color = selected == Source::kLeftColor
                              ? frame.left_color_yuv420
                              : frame.right_color_yuv420;
      const auto& grid = selected == Source::kLeftColor
                             ? frame.metadata.left_rectified.grid
                             : frame.metadata.right_rectified.grid;
      result.width = grid.width;
      result.height = grid.height;
      if (grid.width == 0U || grid.height == 0U || grid.width % 2U != 0U ||
          grid.height % 2U != 0U ||
          grid.height > std::numeric_limits<std::size_t>::max() / grid.width) {
        throw std::invalid_argument("selected color source grid is invalid");
      }
      const auto pixels = static_cast<std::size_t>(grid.width) * grid.height;
      if (color.size() != pixels * 3U / 2U) {
        throw std::invalid_argument(
            "selected color source is unavailable for this frame");
      }
      result.pixel_format = AV_PIX_FMT_YUVJ420P;
      result.data[0] = color.data();
      result.data[1] = color.data() + pixels;
      result.data[2] = color.data() + pixels + pixels / 4U;
      result.linesize[0] = static_cast<int>(grid.width);
      result.linesize[1] = static_cast<int>(grid.width / 2U);
      result.linesize[2] = static_cast<int>(grid.width / 2U);
      available = color.size();
      break;
    }
    case Source::kLeftMono:
    case Source::kRightMono:
    case Source::kLeftRgb:
    case Source::kRightRgb:
    case Source::kLeftRectifiedRgb:
    case Source::kRightRectifiedRgb:
    case Source::kRectifiedStereo:
    case Source::kRectifiedStereoRgb: {
      const std::vector<std::uint8_t>* pixels{};
      switch (selected) {
        case Source::kLeftMono: pixels = &frame.left_mono; break;
        case Source::kRightMono: pixels = &frame.right_mono; break;
        case Source::kLeftRgb: pixels = &frame.left_rgb; break;
        case Source::kRightRgb: pixels = &frame.right_rgb; break;
        case Source::kLeftRectifiedRgb: pixels = &frame.left_rectified_rgb; break;
        case Source::kRightRectifiedRgb: pixels = &frame.right_rectified_rgb; break;
        case Source::kRectifiedStereo: pixels = &frame.rectified_stereo; break;
        case Source::kRectifiedStereoRgb: pixels = &frame.rectified_stereo_rgb; break;
        default: break;
      }
      const bool stereo = selected == Source::kRectifiedStereo ||
                          selected == Source::kRectifiedStereoRgb;
      const bool color = selected != Source::kLeftMono && selected != Source::kRightMono &&
                         selected != Source::kRectifiedStereo;
      result.width = frame.metadata.left_rectified.grid.width * (stereo ? 2U : 1U);
      result.height = frame.metadata.left_rectified.grid.height;
      result.linesize[0] = static_cast<int>(result.width * (color ? 3U : 1U));
      result.pixel_format = color ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_GRAY8;
      result.data[0] = pixels->data();
      available = pixels->size();
      break;
    }
    case Source::kDisparity:
    case Source::kDepth: {
      const auto disparity_scale =
          frame.metadata.disparity.accepted_max_disparity_px;
      if (selected == Source::kDisparity &&
          (!std::isfinite(disparity_scale) || disparity_scale <= 0.0F)) {
        throw std::invalid_argument(
            "selected disparity encoder source has an invalid range");
      }
      const auto& grid = selected == Source::kDisparity
                             ? frame.metadata.disparity.grid
                             : frame.metadata.depth.grid;
      const auto& values = selected == Source::kDisparity ? frame.disparity
                                                          : frame.depth_m;
      result.width = grid.width;
      result.height = grid.height;
      result.linesize[0] = static_cast<int>(grid.width);
      if (grid.width == 0U ||
          grid.height > std::numeric_limits<std::size_t>::max() / grid.width) {
        throw std::invalid_argument("selected encoder source grid is invalid");
      }
      const auto pixels = static_cast<std::size_t>(grid.width) * grid.height;
      if (values.size() != pixels) {
        throw std::invalid_argument(
            "selected encoder source payload does not match its grid");
      }
      result.storage.resize(pixels);
      for (std::size_t index = 0; index < pixels; ++index) {
        const auto value = values[index];
        if (!std::isfinite(value)) {
          result.storage[index] = 0U;
          continue;
        }
        const auto normalized = selected == Source::kDisparity
                                    ? std::clamp(value / disparity_scale, 0.0F,
                                                 1.0F)
                                    : 1.0F - std::clamp((value - 0.2F) / 12.0F,
                                                        0.0F, 1.0F);
        result.storage[index] = static_cast<std::uint8_t>(
            std::lround(normalized * 255.0F));
      }
      result.data[0] = result.storage.data();
      available = result.storage.size();
      break;
    }
  }
  if (result.data[0] == nullptr || result.width == 0U ||
      result.height == 0U || result.linesize[0] < 0 ||
      static_cast<std::uint32_t>(result.linesize[0]) < result.width ||
      result.height > std::numeric_limits<std::size_t>::max() /
                          static_cast<std::size_t>(result.linesize[0]) ||
      available < static_cast<std::size_t>(result.linesize[0]) *
                      result.height) {
    throw std::invalid_argument("selected encoder source is invalid");
  }
  return result;
}

struct CodecContextDeleter {
  void operator()(AVCodecContext* value) const noexcept {
    avcodec_free_context(&value);
  }
};
struct FrameDeleter {
  void operator()(AVFrame* value) const noexcept { av_frame_free(&value); }
};
struct PacketDeleter {
  void operator()(AVPacket* value) const noexcept { av_packet_free(&value); }
};
struct SwsDeleter {
  void operator()(SwsContext* value) const noexcept { sws_freeContext(value); }
};

class FfmpegFrameEncoder final : public FrameEncoder {
 public:
  explicit FfmpegFrameEncoder(Configuration config) : config_(std::move(config)) {
    validate(config_);
  }

  EncodedFrame encode(const FrameSetStorage& frame,
                      const std::uint64_t stream_sequence) override {
    return encode_plane(source_plane(frame, config_.source), frame.metadata,
                        stream_sequence);
  }

  EncodedFrame encode_image(const ImageView& image,
                            const ProductSetMetadata& metadata,
                            const std::uint64_t stream_sequence) override {
    SourcePlane input;
    input.width = image.width;
    input.height = image.height;
    input.data[0] = image.data;
    input.linesize[0] = static_cast<int>(image.stride_bytes);
    switch (image.encoding) {
      case ImageEncoding::kGray8: input.pixel_format = AV_PIX_FMT_GRAY8; break;
      case ImageEncoding::kRgb8: input.pixel_format = AV_PIX_FMT_RGB24; break;
      case ImageEncoding::kBgr8: input.pixel_format = AV_PIX_FMT_BGR24; break;
    }
    if (!image.data || image.width == 0U || image.height == 0U) {
      throw std::invalid_argument("preview image is incomplete");
    }
    return encode_plane(input, metadata, stream_sequence);
  }

  EncodedFrame encode_plane(const SourcePlane& input,
                            const ProductSetMetadata& metadata,
                            const std::uint64_t stream_sequence) {
    const auto started = std::chrono::steady_clock::now();
    // The disparity/depth matcher grid can be odd-sized (the Pi 5 profile is
    // 273x204), while the browser-compatible YUV420 encoders require even
    // dimensions.  Native sizing therefore trims at most one edge pixel;
    // explicitly requested dimensions remain subject to configuration
    // validation and are never changed behind the caller's back.
    const auto width =
        config_.width == 0U ? input.width & ~std::uint32_t{1U} : config_.width;
    const auto height = config_.height == 0U
                            ? input.height & ~std::uint32_t{1U}
                            : config_.height;
    if (!context_ || width != width_ || height != height_ ||
        input.width != input_width_ || input.height != input_height_ ||
        input.pixel_format != input_pixel_format_) {
      initialize(width, height, input.width, input.height,
                 input.pixel_format);
    }

    const int writable = av_frame_make_writable(output_.get());
    if (writable < 0) {
      throw std::runtime_error("FFmpeg frame is not writable: " +
                               av_error(writable));
    }
    const auto scaled = sws_scale(scaler_.get(), input.data.data(),
                                  input.linesize.data(), 0,
                                  static_cast<int>(input.height), output_->data,
                                  output_->linesize);
    if (scaled != static_cast<int>(height_)) {
      throw std::runtime_error("FFmpeg did not convert the complete image");
    }
    output_->pts = next_pts_++;
    const bool force_key = stream_sequence == 1U ||
                           stream_sequence % config_.keyframe_interval == 0U;
    output_->pict_type = force_key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;

    const int sent = avcodec_send_frame(context_.get(), output_.get());
    if (sent < 0) {
      throw std::runtime_error("FFmpeg encoder rejected a frame: " +
                               av_error(sent));
    }
    EncodedFrame result;
    result.identity = metadata.frame;
    result.calibration = metadata.calibration;
    result.calibration_authority = metadata.calibration_authority;
    result.encoding_configuration_generation = 1U;
    result.encoding_applied_revision = revision::sha256_hex(configuration_json(config_));
    result.source = config_.source;
    result.codec = config_.codec;
    result.width = width_;
    result.height = height_;
    result.stream_sequence = stream_sequence;
    result.mime_type = config_.codec == Codec::kH264 ? "video/h264"
                                                     : "image/jpeg";
    result.codec_string = config_.codec == Codec::kH264
                              ? "avc1.42E02A"
                              : "jpeg";
    for (;;) {
      const int received = avcodec_receive_packet(context_.get(), packet_.get());
      if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) break;
      if (received < 0) {
        throw std::runtime_error("FFmpeg packet receive failed: " +
                                 av_error(received));
      }
      result.keyframe = result.keyframe ||
                        (packet_->flags & AV_PKT_FLAG_KEY) != 0;
      const auto* begin = reinterpret_cast<const std::byte*>(packet_->data);
      result.bytes.insert(result.bytes.end(), begin, begin + packet_->size);
      av_packet_unref(packet_.get());
    }
    if (result.bytes.empty()) {
      throw std::runtime_error(
          "low-latency encoder produced no packet for the submitted frame");
    }
    const auto finished = std::chrono::steady_clock::now();
    result.encode_ms =
        std::chrono::duration<double, std::milli>(finished - started).count();
    return result;
  }

 private:
  void initialize(const std::uint32_t width, const std::uint32_t height,
                  const std::uint32_t input_width,
                  const std::uint32_t input_height,
                  const AVPixelFormat input_pixel_format) {
    if (width == 0U || height == 0U || width > 1920U || height > 1080U ||
        width % 2U != 0U || height % 2U != 0U) {
      throw std::invalid_argument(
          "resolved encoder dimensions must be even and no larger than 1920x1080");
    }
    const AVCodec* codec_impl = nullptr;
    if (config_.codec == Codec::kH264) {
      codec_impl = avcodec_find_encoder_by_name("libx264");
    } else {
      codec_impl = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    }
    if (codec_impl == nullptr) {
      throw std::runtime_error(config_.codec == Codec::kH264
                                   ? "FFmpeg libx264 encoder is unavailable"
                                   : "FFmpeg MJPEG encoder is unavailable");
    }
    std::unique_ptr<AVCodecContext, CodecContextDeleter> context(
        avcodec_alloc_context3(codec_impl));
    if (!context) throw std::bad_alloc();
    context->width = static_cast<int>(width);
    context->height = static_cast<int>(height);
    context->time_base = {1, static_cast<int>(config_.cadence_hz)};
    context->framerate = {static_cast<int>(config_.cadence_hz), 1};
    context->gop_size = static_cast<int>(config_.keyframe_interval);
    context->max_b_frames = 0;
    context->thread_count = static_cast<int>(config_.threads);
    context->thread_type = FF_THREAD_SLICE;
    context->bit_rate = static_cast<std::int64_t>(config_.bitrate_bps);
    context->pix_fmt = config_.codec == Codec::kH264 ? AV_PIX_FMT_YUV420P
                                                     : AV_PIX_FMT_YUVJ420P;
    if (config_.codec == Codec::kH264) {
      context->flags |= AV_CODEC_FLAG_LOW_DELAY;
      av_opt_set(context->priv_data, "preset", config_.preset.c_str(), 0);
      av_opt_set(context->priv_data, "profile", "baseline", 0);
      if (config_.low_latency) {
        av_opt_set(context->priv_data, "tune", "zerolatency", 0);
      }
      const auto parameters = "repeat-headers=1:annexb=1:scenecut=0:keyint=" +
                              std::to_string(config_.keyframe_interval) +
                              ":min-keyint=" +
                              std::to_string(config_.keyframe_interval);
      av_opt_set(context->priv_data, "x264-params", parameters.c_str(), 0);
    } else {
      context->flags |= AV_CODEC_FLAG_QSCALE;
      const auto qscale = std::clamp(
          31 - static_cast<int>((config_.jpeg_quality - 1U) * 29U / 99U),
          2, 31);
      context->global_quality = qscale * FF_QP2LAMBDA;
    }
    context->color_range = config_.codec == Codec::kH264
        ? AVCOL_RANGE_MPEG : AVCOL_RANGE_JPEG;
    context->colorspace = AVCOL_SPC_BT470BG;
    const int opened = avcodec_open2(context.get(), codec_impl, nullptr);
    if (opened < 0) {
      throw std::runtime_error("FFmpeg encoder open failed: " +
                               av_error(opened));
    }
    std::unique_ptr<AVFrame, FrameDeleter> output(av_frame_alloc());
    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    if (!output || !packet) throw std::bad_alloc();
    output->format = context->pix_fmt;
    output->width = context->width;
    output->height = context->height;
    output->quality = context->global_quality;
    output->color_range = context->color_range;
    output->colorspace = context->colorspace;
    const int allocated = av_frame_get_buffer(output.get(), 32);
    if (allocated < 0) {
      throw std::runtime_error("FFmpeg frame allocation failed: " +
                               av_error(allocated));
    }
    std::unique_ptr<SwsContext, SwsDeleter> scaler(sws_getContext(
        static_cast<int>(input_width), static_cast<int>(input_height),
        input_pixel_format, static_cast<int>(width), static_cast<int>(height),
        context->pix_fmt, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr));
    if (!scaler) {
      throw std::runtime_error("FFmpeg scaler creation failed");
    }
    const auto* coefficients = sws_getCoefficients(SWS_CS_ITU601);
    // Camera sYCC/I420 and packed gray/RGB inputs carry full-range samples.
    // JPEG retains that range; H.264 uses explicitly signalled video range.
    if (sws_setColorspaceDetails(scaler.get(), coefficients, 1, coefficients,
          config_.codec == Codec::kJpeg ? 1 : 0, 0, 1 << 16, 1 << 16) < 0) {
      throw std::runtime_error("FFmpeg encoder color range setup failed");
    }
    context_ = std::move(context);
    output_ = std::move(output);
    packet_ = std::move(packet);
    scaler_ = std::move(scaler);
    width_ = width;
    height_ = height;
    input_width_ = input_width;
    input_height_ = input_height;
    input_pixel_format_ = input_pixel_format;
    next_pts_ = 0;
  }

  Configuration config_;
  std::unique_ptr<AVCodecContext, CodecContextDeleter> context_;
  std::unique_ptr<AVFrame, FrameDeleter> output_;
  std::unique_ptr<AVPacket, PacketDeleter> packet_;
  std::unique_ptr<SwsContext, SwsDeleter> scaler_;
  std::uint32_t width_{};
  std::uint32_t height_{};
  std::uint32_t input_width_{};
  std::uint32_t input_height_{};
  AVPixelFormat input_pixel_format_{AV_PIX_FMT_NONE};
  std::int64_t next_pts_{};
};

void write_atomic(const std::filesystem::path& destination,
                  const std::span<const std::byte> bytes) {
  std::filesystem::create_directories(destination.parent_path());
  auto temporary = destination;
  temporary += ".tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot create encoded output");
  }
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  output.close();
  if (!output) {
    throw std::runtime_error("cannot write encoded output");
  }
  std::filesystem::rename(temporary, destination);
}

std::string read_configuration_document(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input || input.tellg() < 0 || input.tellg() > 64 * 1024) {
    throw std::runtime_error("cannot read bounded encoding configuration");
  }
  const auto size = static_cast<std::size_t>(input.tellg());
  std::string document(size, '\0');
  input.seekg(0);
  input.read(document.data(), static_cast<std::streamsize>(size));
  if (!input) throw std::runtime_error("cannot read encoding configuration");
  return document;
}

}  // namespace

bool raw_enabled(const Configuration& config) noexcept {
  return config.mode != StreamMode::kEncodedOnly;
}

bool encoded_enabled(const Configuration& config) noexcept {
  return config.mode != StreamMode::kRawOnly;
}

bool requires_depth(const Source source) noexcept {
  return source == Source::kDisparity || source == Source::kDepth;
}

std::string_view name(const StreamMode value) noexcept {
  switch (value) {
    case StreamMode::kRawOnly: return "raw";
    case StreamMode::kEncodedOnly: return "encoded";
    case StreamMode::kBoth: return "both";
  }
  return "unknown";
}

std::string_view name(const Codec value) noexcept {
  switch (value) {
    case Codec::kJpeg: return "jpeg";
    case Codec::kH264: return "h264";
  }
  return "unknown";
}

std::string_view name(const Source value) noexcept {
  switch (value) {
    case Source::kLeftRectified: return "left_rectified";
    case Source::kRightRectified: return "right_rectified";
    case Source::kLeftColor: return "left_color";
    case Source::kRightColor: return "right_color";
    case Source::kDisparity: return "disparity";
    case Source::kDepth: return "depth";
    case Source::kLeftMono: return "left_mono";
    case Source::kRightMono: return "right_mono";
    case Source::kLeftRgb: return "left_rgb";
    case Source::kRightRgb: return "right_rgb";
    case Source::kLeftRectifiedRgb: return "left_rectified_rgb";
    case Source::kRightRectifiedRgb: return "right_rectified_rgb";
    case Source::kRectifiedStereo: return "rectified_stereo";
    case Source::kRectifiedStereoRgb: return "rectified_stereo_rgb";
  }
  return "unknown";
}

void validate(const Configuration& config) {
  if (config.schema_version != 1U) {
    throw std::invalid_argument("encoding.schema_version must be 1");
  }
  if ((config.width == 0U) != (config.height == 0U) ||
      config.width > 1920U || config.height > 1080U ||
      (config.width != 0U &&
       (config.width % 2U != 0U || config.height % 2U != 0U))) {
    throw std::invalid_argument(
        "encoding dimensions must be native (0x0) or even and at most 1920x1080");
  }
  if (config.cadence_hz == 0U || config.cadence_hz > 60U) {
    throw std::invalid_argument("encoding.cadence_hz must be in 1..60");
  }
  if (config.bitrate_bps < 100'000U || config.bitrate_bps > 50'000'000U) {
    throw std::invalid_argument("encoding.bitrate_bps must be in 100000..50000000");
  }
  if (config.jpeg_quality == 0U || config.jpeg_quality > 100U) {
    throw std::invalid_argument("encoding.jpeg_quality must be in 1..100");
  }
  if (config.keyframe_interval == 0U || config.keyframe_interval > 600U) {
    throw std::invalid_argument("encoding.keyframe_interval must be in 1..600");
  }
  if (config.threads == 0U || config.threads > 4U) {
    throw std::invalid_argument("encoding.threads must be in 1..4");
  }
  static constexpr std::array<std::string_view, 5> presets{
      "ultrafast", "superfast", "veryfast", "faster", "fast"};
  if (std::find(presets.begin(), presets.end(), config.preset) ==
      presets.end()) {
    throw std::invalid_argument("encoding.preset is not allowed");
  }
}

void validate_latest_configuration(const Configuration& config) {
  validate(config);
  if (encoded_enabled(config) && config.codec == Codec::kH264 &&
      config.keyframe_interval != 1U) {
    throw std::invalid_argument(
        "latest-frame H.264 delivery requires keyframe_interval=1");
  }
}

Configuration parse_configuration(const std::string_view document) {
  YAML::Node root;
  try {
    root = YAML::Load(std::string(document));
  } catch (const YAML::Exception&) {
    throw std::invalid_argument("encoding configuration is not valid JSON");
  }
  if (!root.IsMap()) {
    throw std::invalid_argument("encoding configuration must be an object");
  }
  static constexpr std::array<std::string_view, 13> fields{
      "schema_version", "mode", "codec", "source", "width", "height",
      "cadence_hz", "bitrate_bps", "jpeg_quality", "keyframe_interval",
      "threads", "low_latency", "preset"};
  for (const auto& item : root) {
    const auto key = item.first.as<std::string>();
    if (std::find(fields.begin(), fields.end(), key) == fields.end()) {
      throw std::invalid_argument("unknown encoding field: " + key);
    }
  }
  Configuration result;
  result.schema_version = required<std::uint32_t>(root, "schema_version");
  result.mode = stream_mode(required<std::string>(root, "mode"));
  result.codec = codec(required<std::string>(root, "codec"));
  result.source = source(required<std::string>(root, "source"));
  result.width = required<std::uint32_t>(root, "width");
  result.height = required<std::uint32_t>(root, "height");
  result.cadence_hz = required<std::uint32_t>(root, "cadence_hz");
  result.bitrate_bps = required<std::uint32_t>(root, "bitrate_bps");
  result.jpeg_quality = required<std::uint32_t>(root, "jpeg_quality");
  result.keyframe_interval =
      required<std::uint32_t>(root, "keyframe_interval");
  result.threads = required<std::uint32_t>(root, "threads");
  result.low_latency = required<bool>(root, "low_latency");
  result.preset = required<std::string>(root, "preset");
  validate(result);
  return result;
}

std::string configuration_json(const Configuration& config) {
  validate(config);
  std::ostringstream output;
  output << "{\"schema_version\":1,\"mode\":\"" << name(config.mode)
         << "\",\"codec\":\"" << name(config.codec)
         << "\",\"source\":\"" << name(config.source)
         << "\",\"width\":" << config.width << ",\"height\":"
         << config.height << ",\"cadence_hz\":" << config.cadence_hz
         << ",\"bitrate_bps\":" << config.bitrate_bps
         << ",\"jpeg_quality\":" << config.jpeg_quality
         << ",\"keyframe_interval\":" << config.keyframe_interval
         << ",\"threads\":" << config.threads << ",\"low_latency\":"
         << (config.low_latency ? "true" : "false")
         << ",\"preset\":\"" << config.preset << "\"}";
  return output.str();
}

Configuration load_configuration(const std::filesystem::path& path,
                                 const Configuration& fallback) {
  validate(fallback);
  if (path.empty() || !std::filesystem::exists(path)) return fallback;
  const auto document = read_configuration_document(path);
  return parse_configuration(document);
}

std::vector<std::byte> serialize(const EncodedFrame& frame) {
  if (frame.stream_sequence == 0U || frame.width == 0U || frame.height == 0U ||
      frame.mime_type.empty() || frame.codec_string.empty() ||
      frame.bytes.empty() || frame.bytes.size() > kMaximumEncodedPayloadBytes) {
    throw std::invalid_argument("encoded frame is incomplete or oversized");
  }
  const auto metadata = frame_metadata_json(frame);
  if (metadata.size() > kMaximumEncodedMetadataBytes) {
    throw std::invalid_argument("encoded metadata exceeds its bound");
  }
  std::vector<std::byte> result(kEncodedHeaderBytes + metadata.size() +
                                frame.bytes.size());
  result[0] = std::byte{'N'};
  result[1] = std::byte{'V'};
  result[2] = std::byte{'E'};
  result[3] = std::byte{'1'};
  put_u16(result.data() + 4U, 1U);
  put_u16(result.data() + 6U, frame.keyframe ? 1U : 0U);
  put_u32(result.data() + 8U, static_cast<std::uint32_t>(metadata.size()));
  put_u64(result.data() + 12U, frame.bytes.size());
  put_u64(result.data() + 20U, frame.stream_sequence);
  put_u32(result.data() + 28U, 0U);
  std::memcpy(result.data() + kEncodedHeaderBytes, metadata.data(),
              metadata.size());
  std::memcpy(result.data() + kEncodedHeaderBytes + metadata.size(),
              frame.bytes.data(), frame.bytes.size());
  return result;
}

EncodedFrame FrameEncoder::encode_image(const ImageView&,
                                       const ProductSetMetadata&, std::uint64_t) {
  throw std::runtime_error("encoder does not support direct image input");
}

std::unique_ptr<FrameEncoder> make_ffmpeg_encoder(
    const Configuration& config) {
  return std::make_unique<FfmpegFrameEncoder>(config);
}

namespace {

bool same_capture_scope(const FrameSetIdentity& left,
                        const FrameSetIdentity& right) noexcept {
  return left.device_uuid == right.device_uuid && left.run_uuid == right.run_uuid &&
         left.capture_epoch == right.capture_epoch;
}

bool same_capture(const FrameSetIdentity& left,
                  const FrameSetIdentity& right) noexcept {
  // Processing can apply a newer matcher revision after capture preflight.
  // Its configuration_epoch does not change ownership of the captured image.
  return same_capture_scope(left, right) &&
         left.capture_sequence == right.capture_sequence &&
         left.capture_time_ns == right.capture_time_ns &&
         left.capture_clock == right.capture_clock;
}

}  // namespace

struct AsyncFrameEncoder::Impl {
  struct Preview {
    ProductSetMetadata metadata;
    std::vector<std::uint8_t> left;
    std::vector<std::uint8_t> right;
    std::vector<std::uint8_t> left_rgb;
    std::vector<std::uint8_t> right_rgb;
    std::vector<std::uint8_t> left_rectified;
    std::vector<std::uint8_t> right_rectified;
    std::vector<std::uint8_t> side_by_side;
  };
  struct Work {
    std::shared_ptr<const FrameSetStorage> frame;
    std::shared_ptr<Preview> preview;
    Configuration config;
    std::uint64_t generation{};
    std::uint64_t configuration_generation{};
    std::string applied_revision;
  };

  Impl(std::filesystem::path control, std::filesystem::path output,
       Configuration initial, std::filesystem::path trace,
       EncoderFactory factory, PreviewRectifierFactory rectifier_factory,
       OutputPublisher publisher)
      : control_file(std::move(control)), output_file(std::move(output)),
        trace_directory(std::move(trace)), fallback(std::move(initial)),
        config(fallback), encoder_factory(std::move(factory)),
        preview_rectifier_factory(std::move(rectifier_factory)), output_publisher(std::move(publisher)) {
    validate_latest_configuration(fallback);
    if (!encoder_factory) encoder_factory = make_ffmpeg_encoder;
    for (auto& slot : preview_pool) slot = std::make_shared<Preview>();
    stats_value.demand_cadence_hz = demand_cadence_hz;
    active_revision = revision::sha256_hex(configuration_json(config));
    stats_value.requested_revision = active_revision;
    remove_latest();
    refresh();
    stats_value.running = true;
    worker = std::thread(&Impl::run, this);
  }

  void remove_latest() {
    if (output_publisher) output_publisher({}, 0U, {});
    if (output_file.empty()) return;
    std::error_code error;
    std::filesystem::remove(output_file, error);
    if (error) throw std::runtime_error("cannot invalidate encoded output: " +
                                        error.message());
  }

  void refresh() {
    // Capture preflight and completed-frame publication run on different
    // threads. Serialize file observations as well as configuration updates.
    std::lock_guard refresh_lock(refresh_mutex);
    std::error_code error;
    const bool exists = !control_file.empty() &&
                        std::filesystem::exists(control_file, error) && !error;
    std::optional<std::filesystem::file_time_type> modified;
    if (exists) {
      modified = std::filesystem::last_write_time(control_file, error);
      if (error) modified.reset();
    }
    if (observed_file == exists && observed_modified == modified) return;
    std::string requested_revision;
    try {
      const auto document = exists ? read_configuration_document(control_file)
                                   : configuration_json(fallback);
      requested_revision = revision::sha256_hex(document);
      auto updated = parse_configuration(document);
      validate_latest_configuration(updated);
      std::lock_guard lock(mutex);
      // Removal and the worker's final rename share this short lock. Encoding
      // and writing a potentially large temporary payload happen outside it.
      remove_latest();
      config = std::move(updated);
      ++generation;
      pending.reset();
      last_submitted.reset();
      last_numeric_admitted.reset();
      ++stats_value.configuration_reloads;
      ++stats_value.configuration_generation;
      stats_value.requested_revision = requested_revision;
      active_revision = requested_revision;
      stats_value.applied_revision.clear();
      stats_value.configuration_error.clear();
      observed_file = exists;
      observed_modified = modified;
      last_error.clear();
    } catch (const std::exception& exception) {
      std::lock_guard lock(mutex);
      ++stats_value.configuration_errors;
      stats_value.requested_revision = requested_revision;
      stats_value.configuration_error = exception.what();
      last_error = exception.what();
      observed_file = exists;
      observed_modified = modified;
    }
  }

  void align_scope(const FrameSetIdentity& identity) {
    if (capture_scope && !same_capture_scope(*capture_scope, identity)) {
      remove_latest();
      ++generation;
      pending.reset();
      last_submitted.reset();
      last_numeric_admitted.reset();
    }
    capture_scope = identity;
  }

  bool cadence_limited(const FrameSetIdentity& identity) const noexcept {
    return cadence_limited_against(identity, last_submitted);
  }

  bool cadence_limited_against(const FrameSetIdentity& identity,
      const std::optional<FrameSetIdentity>& previous) const noexcept {
    if (!previous || !same_capture_scope(*previous, identity)) {
      return false;
    }
    if (identity.capture_sequence <= previous->capture_sequence ||
        identity.capture_time_ns <= previous->capture_time_ns) return true;
    const auto period_ns = static_cast<std::uint64_t>(1'000'000'000.0 /
        std::min(static_cast<double>(config.cadence_hz), demand_cadence_hz));
    // Absorb camera timestamp jitter without raising output cadence materially.
    const auto tolerance_ns = std::min<std::uint64_t>(period_ns / 10U, 2'000'000ULL);
    const auto elapsed = static_cast<std::uint64_t>(
        identity.capture_time_ns - previous->capture_time_ns);
    return elapsed + tolerance_ns < period_ns;
  }

  CapturePlan prepare_capture(const FrameSetIdentity& identity) {
    if (identity.capture_time_ns <= 0) {
      throw std::invalid_argument("encoder capture timestamp must be positive");
    }
    refresh();
    std::lock_guard lock(mutex);
    CapturePlan plan;
    plan.identity = identity;
    ++stats_value.capture_preflights;
    if (stop_requested) {
      plan.result = SubmitResult::kStopped;
      return plan;
    }
    align_scope(identity);
    plan.generation = generation;
    if (!encoded_enabled(config) || demand_cadence_hz == 0.0) return plan;
    if (cadence_limited(identity) ||
        (requires_depth(config.source) && cadence_limited_against(identity, last_numeric_admitted))) {
      ++stats_value.capture_cadence_limited;
      plan.result = SubmitResult::kCadenceLimited;
      return plan;
    }
    plan.result = SubmitResult::kAccepted;
    if (config.source == Source::kLeftColor) {
      plan.color_preview = ColorPreviewRequest::kLeft;
    } else if (config.source == Source::kRightColor) {
      plan.color_preview = ColorPreviewRequest::kRight;
    }
    if (plan.color_preview != ColorPreviewRequest::kNone) {
      ++stats_value.color_preview_requests;
    }
    // Do not reserve/advance cadence here. A slow processor replaces pending
    // captures; every due replacement must still carry its own color plane.
    return plan;
  }

  bool commit_native_capture(const CapturePlan& plan) {
    std::lock_guard lock(mutex);
    if (stop_requested || plan.result != SubmitResult::kAccepted ||
        plan.generation != generation || !requires_depth(config.source) ||
        !encoded_enabled(config) || demand_cadence_hz == 0.0 ||
        (last_numeric_admitted &&
         cadence_limited_against(plan.identity, last_numeric_admitted))) return false;
    last_numeric_admitted = plan.identity;
    ++stats_value.numeric_admitted;
    return true;
  }

  SubmitResult submit(std::shared_ptr<const FrameSetStorage> frame,
                      const CapturePlan* plan, const bool native_only = false) {
    if (!frame) throw std::invalid_argument("encoder cannot submit a null frame");
    refresh();
    std::lock_guard lock(mutex);
    if (stop_requested) return SubmitResult::kStopped;
    if (native_only) {
      if (!plan || plan->result != SubmitResult::kAccepted) {
        return plan ? plan->result : SubmitResult::kDisabled;
      }
      if (plan->generation != generation) {
        ++stats_value.stale_discarded;
        return SubmitResult::kConfigurationChanged;
      }
      if (!requires_depth(config.source)) return SubmitResult::kDisabled;
    }
    if (plan) {
      if (!same_capture(plan->identity, frame->metadata.frame)) {
        throw std::invalid_argument("encoder capture plan belongs to another frame");
      }
      if (plan->generation != generation) {
        ++stats_value.stale_discarded;
        return SubmitResult::kConfigurationChanged;
      }
    } else {
      align_scope(frame->metadata.frame);
    }
    if (!encoded_enabled(config) || demand_cadence_hz == 0.0) return SubmitResult::kDisabled;
    if ((plan && plan->result == SubmitResult::kCadenceLimited) ||
        cadence_limited(frame->metadata.frame)) {
      ++stats_value.cadence_limited;
      return SubmitResult::kCadenceLimited;
    }
    if (plan && plan->result != SubmitResult::kAccepted) {
      return plan->result;
    }
    if ((config.source == Source::kLeftColor && frame->left_color_yuv420.empty()) ||
        (config.source == Source::kRightColor && frame->right_color_yuv420.empty())) {
      ++stats_value.preview_unavailable;
      return SubmitResult::kPreviewUnavailable;
    }
    if (requires_depth(config.source)) {
      const auto kind = config.source == Source::kDepth
          ? ipc_v2::ProductKind::kDepth : ipc_v2::ProductKind::kDisparity;
      if ((frame->available_products & ipc_v2::product_bit(kind)) == 0U) {
        ++stats_value.preview_unavailable;
        return SubmitResult::kPreviewUnavailable;
      }
    }
    last_submitted = frame->metadata.frame;
    ++stats_value.submitted;
    const bool replaced = pending.has_value();
    if (replaced) ++stats_value.pending_replaced;
    pending = Work{std::move(frame), {}, config, generation,
                   stats_value.configuration_generation, active_revision};
    condition.notify_one();
    return replaced ? SubmitResult::kReplacedPending : SubmitResult::kAccepted;
  }


  static bool color_source(const Source source) noexcept {
    return source == Source::kLeftColor || source == Source::kRightColor ||
           source == Source::kLeftRgb || source == Source::kRightRgb ||
           source == Source::kLeftRectifiedRgb || source == Source::kRightRectifiedRgb ||
           source == Source::kRectifiedStereoRgb;
  }
  static bool stereo_source(const Source source) noexcept {
    return source == Source::kRectifiedStereo || source == Source::kRectifiedStereoRgb;
  }
  static bool right_source(const Source source) noexcept {
    return source == Source::kRightColor || source == Source::kRightRectified ||
           source == Source::kRightMono || source == Source::kRightRgb ||
           source == Source::kRightRectifiedRgb;
  }
  static bool rectified_source(const Source source) noexcept {
    return source == Source::kLeftRectified || source == Source::kRightRectified ||
           source == Source::kLeftRectifiedRgb || source == Source::kRightRectifiedRgb ||
           stereo_source(source);
  }

  void set_demand(const double cadence) {
    if (!std::isfinite(cadence) || cadence < 0.0 || cadence > 60.0) {
      throw std::invalid_argument("preview demand cadence must be in 0..60");
    }
    std::lock_guard lock(mutex);
    if (cadence == demand_cadence_hz) return;
    remove_latest();
    demand_cadence_hz = cadence;
    stats_value.demand_cadence_hz = cadence;
    ++generation;
    pending.reset();
    last_submitted.reset();
    last_numeric_admitted.reset();
  }

  SubmitResult submit_capture(const ProductSetMetadata& metadata,
                             const CaptureImageReader& reader) {
    if (!reader) throw std::invalid_argument("preview requires a capture reader");
    depthd::validate(metadata.frame);
    const auto& grid = metadata.left_rectified.grid;
    const auto pixels = static_cast<std::size_t>(grid.width) * grid.height;
    // Includes both input planes, aligned RGB rows, rectified outputs, stereo
    // output and FFmpeg's SIMD padding for every retained pool slot.
    const auto bounded_slot_bytes = pixels * 21U +
        static_cast<std::size_t>(grid.height) * 62U +
        7U * AV_INPUT_BUFFER_PADDING_SIZE;
    if (grid.width == 0U || grid.height == 0U || grid.width % 2U != 0U ||
        grid.height % 2U != 0U || grid.width > 1920U || grid.height > 1080U ||
        bounded_slot_bytes * preview_pool.size() > 256U * 1024U * 1024U) {
      throw std::invalid_argument("preview input grid exceeds its memory bound");
    }
    refresh();
    Configuration selected;
    std::shared_ptr<Preview> slot;
    std::uint64_t selected_generation{};
    {
      std::lock_guard lock(mutex);
      ++stats_value.capture_preflights;
      if (stop_requested) return SubmitResult::kStopped;
      align_scope(metadata.frame);
      if (!encoded_enabled(config) || demand_cadence_hz == 0.0 ||
          requires_depth(config.source)) return SubmitResult::kDisabled;
      if (cadence_limited(metadata.frame)) {
        ++stats_value.capture_cadence_limited;
        ++stats_value.cadence_limited;
        return SubmitResult::kCadenceLimited;
      }
      for (const auto& candidate : preview_pool) {
        if (candidate.use_count() == 1) { slot = candidate; break; }
      }
      if (!slot) {
        ++stats_value.image_pool_dropped;
        return SubmitResult::kPreviewUnavailable;
      }
      selected = config;
      selected_generation = generation;
      ++stats_value.image_admitted;
    }
    slot->metadata = metadata;
    const auto color = color_source(selected.source);
    const auto input_bytes = color ? pixels * 3U / 2U : pixels;
    const auto copy = [&](const CameraRole role, std::vector<std::uint8_t>& output) {
      // libswscale's SIMD readers need an addressable tail beyond the last
      // packed plane. The capture reader still receives its exact pixel span.
      output.resize(input_bytes + AV_INPUT_BUFFER_PADDING_SIZE, 0U);
      reader(role, color, std::span<std::uint8_t>(output.data(), input_bytes));
      std::lock_guard lock(mutex);
      ++stats_value.image_copies;
      if (color) ++stats_value.color_preview_requests;
    };
    if (!right_source(selected.source) || stereo_source(selected.source)) {
      copy(CameraRole::kLogicalLeft, slot->left);
    }
    if (right_source(selected.source) || stereo_source(selected.source)) {
      copy(CameraRole::kLogicalRight, slot->right);
    }
    std::lock_guard lock(mutex);
    if (stop_requested) return SubmitResult::kStopped;
    if (selected_generation != generation || demand_cadence_hz == 0.0) {
      ++stats_value.stale_discarded;
      return SubmitResult::kConfigurationChanged;
    }
    last_submitted = metadata.frame;
    const bool replaced = pending.has_value();
    if (replaced) ++stats_value.pending_replaced;
    ++stats_value.submitted;
    pending = Work{{}, std::move(slot), selected, selected_generation,
                   stats_value.configuration_generation, active_revision};
    condition.notify_one();
    return replaced ? SubmitResult::kReplacedPending : SubmitResult::kAccepted;
  }

  ImageView render_preview(Preview& preview, const Source source) {
    const auto& grid = preview.metadata.left_rectified.grid;
    const auto color = color_source(source);
    const auto channels = color ? 3U : 1U;
    const auto bytes_per_row = static_cast<std::size_t>(grid.width) * channels;
    const auto render = [&](const bool right) -> ImageView {
      auto& raw = right ? preview.right : preview.left;
      auto& rgb = right ? preview.right_rgb : preview.left_rgb;
      ImageView image{raw.data(), grid.width, grid.height, grid.width,
                      ImageEncoding::kGray8};
      if (color) {
        const auto pixels = static_cast<std::size_t>(grid.width) * grid.height;
        const auto rgb_stride = (bytes_per_row + 31U) & ~std::size_t{31U};
        // RGB24 SIMD conversion may write a complete aligned vector at a row
        // edge. Give every row padding, including the final row of small images.
        rgb.resize(rgb_stride * grid.height + AV_INPUT_BUFFER_PADDING_SIZE, 0U);
        preview_scaler.reset(sws_getCachedContext(preview_scaler.release(),
            static_cast<int>(grid.width), static_cast<int>(grid.height), AV_PIX_FMT_YUV420P,
            static_cast<int>(grid.width), static_cast<int>(grid.height), AV_PIX_FMT_RGB24,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr));
        if (!preview_scaler) throw std::runtime_error("preview color converter unavailable");
        const auto* coefficients = sws_getCoefficients(SWS_CS_ITU601);
        // Capture requests libcamera sYCC: Rec.601 chroma with full-range Y.
        if (sws_setColorspaceDetails(preview_scaler.get(), coefficients, 1,
              coefficients, 1, 0, 1 << 16, 1 << 16) < 0) {
          throw std::runtime_error("preview sYCC color range setup failed");
        }
        const std::array<const std::uint8_t*, 4> input{
            raw.data(), raw.data() + pixels, raw.data() + pixels * 5U / 4U, nullptr};
        const std::array<int, 4> input_stride{static_cast<int>(grid.width),
            static_cast<int>(grid.width / 2U), static_cast<int>(grid.width / 2U), 0};
        const std::array<std::uint8_t*, 4> output{rgb.data(), nullptr, nullptr, nullptr};
        const std::array<int, 4> output_stride{static_cast<int>(rgb_stride), 0, 0, 0};
        if (sws_scale(preview_scaler.get(), input.data(), input_stride.data(), 0,
                      static_cast<int>(grid.height), output.data(), output_stride.data()) !=
            static_cast<int>(grid.height)) throw std::runtime_error("preview color conversion incomplete");
        image = {rgb.data(), grid.width, grid.height, rgb_stride, ImageEncoding::kRgb8};
      }
      if (rectified_source(source)) {
        if (!preview_rectifier) {
          if (!preview_rectifier_factory) {
            throw std::runtime_error("rectified preview requires its own rectifier");
          }
          preview_rectifier = preview_rectifier_factory();
          if (!preview_rectifier) throw std::runtime_error("preview rectifier factory returned null");
        }
        auto& rectified = right ? preview.right_rectified : preview.left_rectified;
        rectified.resize(bytes_per_row * grid.height + AV_INPUT_BUFFER_PADDING_SIZE, 0U);
        RectificationStats timing;
        preview_rectifier->rectify(right ? RectificationCamera::kRight : RectificationCamera::kLeft,
            image, {rectified.data(), grid.width, grid.height, bytes_per_row, image.encoding}, timing);
        image.data = rectified.data();
        image.stride_bytes = bytes_per_row;
        std::lock_guard lock(mutex);
        ++stats_value.image_rectified;
      }
      return image;
    };
    if (!stereo_source(source)) return render(right_source(source));
    const auto left = render(false);
    const auto right = render(true);
    preview.side_by_side.resize(bytes_per_row * 2U * grid.height + AV_INPUT_BUFFER_PADDING_SIZE, 0U);
    for (std::uint32_t row = 0; row < grid.height; ++row) {
      auto* output = preview.side_by_side.data() + row * bytes_per_row * 2U;
      std::memcpy(output, left.data + row * left.stride_bytes, bytes_per_row);
      std::memcpy(output + bytes_per_row, right.data + row * right.stride_bytes, bytes_per_row);
    }
    return {preview.side_by_side.data(), grid.width * 2U, grid.height,
            bytes_per_row * 2U, left.encoding};
  }

  void run() noexcept {
    std::unique_ptr<FrameEncoder> encoder;
    std::uint64_t encoder_generation = std::numeric_limits<std::uint64_t>::max();
    for (;;) {
      Work work;
      std::uint64_t sequence{};
      {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return stop_requested || pending; });
        if (stop_requested) break;
        work = std::move(*pending);
        pending.reset();
        sequence = ++stream_sequence;
      }
      auto temporary = output_file;
      temporary += ".tmp";
      try {
        if (!encoder || encoder_generation != work.generation) {
          encoder = encoder_factory(work.config);
          if (!encoder) throw std::runtime_error("encoder factory returned null");
          encoder_generation = work.generation;
        }
        auto encoded = work.preview
            ? encoder->encode_image(render_preview(*work.preview, work.config.source),
                                    work.preview->metadata, sequence)
            : encoder->encode(*work.frame, sequence);
        encoded.encoding_configuration_generation = work.configuration_generation;
        encoded.encoding_applied_revision = work.applied_revision;
        if (encoded.codec == Codec::kH264 && !encoded.keyframe) {
          throw std::runtime_error("latest-frame encoder emitted predictive H.264");
        }
        const auto bytes = serialize(encoded);
        // write_atomic() commits a separate temporary path, not the publicly
        // readable snapshot. The final public rename is generation-checked.
        if (!output_file.empty()) write_atomic(temporary, bytes);
        // Detect a control change that happened during a long encode even if
        // capture is currently idle and has not refreshed it yet.
        refresh();
        {
          std::lock_guard lock(mutex);
          if (stop_requested || generation != work.generation ||
              !encoded_enabled(config)) {
            if (!output_file.empty()) std::filesystem::remove(temporary);
            ++stats_value.stale_discarded;
            continue;
          }
          if (!output_file.empty()) std::filesystem::rename(temporary, output_file);
          if (output_publisher) output_publisher(encoded.identity, encoded.stream_sequence, bytes);
          ++stats_value.encoded;
          stats_value.encoded_bytes += encoded.bytes.size();
          stats_value.last_frame_bytes = encoded.bytes.size();
          stats_value.last_encode_ms = encoded.encode_ms;
          stats_value.applied_revision = work.applied_revision;
          if (stats_value.requested_revision == work.applied_revision) {
            stats_value.configuration_error.clear();
          }
          last_error.clear();
        }
        // Trace files retain historical frames; unlike latest.nve they are
        // never used to select the currently configured preview.
        if (!trace_directory.empty()) {
          write_atomic(trace_directory /
                           ("post_encoding_" +
                            std::to_string(encoded.identity.capture_sequence) +
                            ".nve"), bytes);
        }
      } catch (const std::exception& exception) {
        std::error_code ignored;
        if (!output_file.empty()) std::filesystem::remove(temporary, ignored);
        std::lock_guard lock(mutex);
        if (generation == work.generation && !stop_requested) {
          ++stats_value.failures;
          stats_value.configuration_error = exception.what();
          last_error = exception.what();
        } else {
          ++stats_value.stale_discarded;
        }
        encoder.reset();
      }
    }
    std::lock_guard lock(mutex);
    stats_value.running = false;
  }

  void stop() noexcept {
    {
      std::lock_guard lock(mutex);
      if (stop_requested) return;
      stop_requested = true;
      pending.reset();
      condition.notify_all();
    }
    if (worker.joinable()) worker.join();
  }

  std::filesystem::path control_file;
  std::filesystem::path output_file;
  std::filesystem::path trace_directory;
  Configuration fallback;
  mutable std::mutex mutex;
  std::mutex refresh_mutex;
  std::condition_variable condition;
  Configuration config;
  EncoderFactory encoder_factory;
  PreviewRectifierFactory preview_rectifier_factory;
  OutputPublisher output_publisher;
  std::unique_ptr<RectificationBackend> preview_rectifier;
  std::unique_ptr<SwsContext, SwsDeleter> preview_scaler;
  std::array<std::shared_ptr<Preview>, 3> preview_pool;
  double demand_cadence_hz{60.0};
  Stats stats_value{};
  std::optional<Work> pending;
  std::optional<FrameSetIdentity> capture_scope;
  std::optional<FrameSetIdentity> last_submitted;
  std::optional<FrameSetIdentity> last_numeric_admitted;
  std::string last_error;
  std::string active_revision;
  std::optional<std::filesystem::file_time_type> observed_modified;
  bool observed_file{};
  bool stop_requested{};
  std::uint64_t generation{};
  std::uint64_t stream_sequence{};
  std::thread worker;
};

AsyncFrameEncoder::AsyncFrameEncoder(std::filesystem::path control_file,
                                     std::filesystem::path output_file,
                                     Configuration fallback,
                                     std::filesystem::path trace_directory,
                                     EncoderFactory encoder_factory,
                                     PreviewRectifierFactory preview_rectifier_factory,
                                     OutputPublisher output_publisher)
    : impl_(std::make_unique<Impl>(std::move(control_file),
                                   std::move(output_file),
                                   std::move(fallback),
                                   std::move(trace_directory),
                                   std::move(encoder_factory),
                                   std::move(preview_rectifier_factory),
                                   std::move(output_publisher))) {}

AsyncFrameEncoder::~AsyncFrameEncoder() noexcept { stop(); }

SubmitResult AsyncFrameEncoder::submit(
    std::shared_ptr<const FrameSetStorage> frame) {
  return impl_->submit(std::move(frame), nullptr);
}

SubmitResult AsyncFrameEncoder::submit(
    std::shared_ptr<const FrameSetStorage> frame, const CapturePlan& plan) {
  return impl_->submit(std::move(frame), &plan);
}

SubmitResult AsyncFrameEncoder::submit_native(
    std::shared_ptr<const FrameSetStorage> frame, const CapturePlan& plan) {
  return impl_->submit(std::move(frame), &plan, true);
}

CapturePlan AsyncFrameEncoder::prepare_capture(const FrameSetIdentity& identity) {
  return impl_->prepare_capture(identity);
}

bool AsyncFrameEncoder::commit_native_capture(const CapturePlan& plan) {
  return impl_->commit_native_capture(plan);
}

SubmitResult AsyncFrameEncoder::submit_capture(
    const ProductSetMetadata& metadata, const CaptureImageReader& reader) {
  return impl_->submit_capture(metadata, reader);
}

void AsyncFrameEncoder::set_demand(const double cadence_hz) {
  impl_->set_demand(cadence_hz);
}

Configuration AsyncFrameEncoder::configuration() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->config;
}

void AsyncFrameEncoder::refresh_configuration() { impl_->refresh(); }

Stats AsyncFrameEncoder::stats() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->stats_value;
}

std::string AsyncFrameEncoder::take_last_error() {
  std::lock_guard lock(impl_->mutex);
  return std::exchange(impl_->last_error, {});
}

void AsyncFrameEncoder::stop() noexcept {
  if (impl_) impl_->stop();
}

}  // namespace depthd::encoding
