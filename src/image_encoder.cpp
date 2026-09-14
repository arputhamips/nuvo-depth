#include "depthd/image_encoder.hpp"

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
  if (value == "disparity") return Source::kDisparity;
  if (value == "depth") return Source::kDepth;
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
         << frame.identity.capture_time_ns << ",\"configuration_epoch\":"
         << frame.identity.configuration_epoch << ",\"device_uuid\":\""
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
  const std::uint8_t* data{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::size_t stride{};
  std::vector<std::uint8_t> storage;
};

[[nodiscard]] SourcePlane source_plane(const FrameSetStorage& frame,
                                       const Source selected) {
  SourcePlane result;
  std::size_t available{};
  switch (selected) {
    case Source::kLeftRectified:
      result.data = frame.left_rectified.data();
      result.width = frame.metadata.left_rectified.grid.width;
      result.height = frame.metadata.left_rectified.grid.height;
      result.stride = frame.metadata.left_rectified.grid.width;
      available = frame.left_rectified.size();
      break;
    case Source::kRightRectified:
      result.data = frame.right_rectified.data();
      result.width = frame.metadata.right_rectified.grid.width;
      result.height = frame.metadata.right_rectified.grid.height;
      result.stride = frame.metadata.right_rectified.grid.width;
      available = frame.right_rectified.size();
      break;
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
      result.stride = grid.width;
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
      result.data = result.storage.data();
      available = result.storage.size();
      break;
    }
  }
  if (result.data == nullptr || result.width == 0U || result.height == 0U ||
      result.stride < result.width ||
      result.height > std::numeric_limits<std::size_t>::max() / result.stride ||
      available < result.stride * result.height) {
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
    const auto started = std::chrono::steady_clock::now();
    const auto input = source_plane(frame, config_.source);
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
        input.width != input_width_ || input.height != input_height_) {
      initialize(width, height, input.width, input.height);
    }

    const int writable = av_frame_make_writable(output_.get());
    if (writable < 0) {
      throw std::runtime_error("FFmpeg frame is not writable: " +
                               av_error(writable));
    }
    const std::uint8_t* input_data[4]{input.data, nullptr, nullptr, nullptr};
    const int input_lines[4]{static_cast<int>(input.stride), 0, 0, 0};
    const auto scaled = sws_scale(scaler_.get(), input_data, input_lines, 0,
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
    result.identity = frame.metadata.frame;
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
                  const std::uint32_t input_height) {
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
      context->color_range = AVCOL_RANGE_JPEG;
    }
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
    const int allocated = av_frame_get_buffer(output.get(), 32);
    if (allocated < 0) {
      throw std::runtime_error("FFmpeg frame allocation failed: " +
                               av_error(allocated));
    }
    std::unique_ptr<SwsContext, SwsDeleter> scaler(sws_getContext(
        static_cast<int>(input_width), static_cast<int>(input_height),
        AV_PIX_FMT_GRAY8, static_cast<int>(width), static_cast<int>(height),
        context->pix_fmt, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr));
    if (!scaler) {
      throw std::runtime_error("FFmpeg scaler creation failed");
    }
    context_ = std::move(context);
    output_ = std::move(output);
    packet_ = std::move(packet);
    scaler_ = std::move(scaler);
    width_ = width;
    height_ = height;
    input_width_ = input_width;
    input_height_ = input_height;
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

}  // namespace

bool raw_enabled(const Configuration& config) noexcept {
  return config.mode != StreamMode::kEncodedOnly;
}

bool encoded_enabled(const Configuration& config) noexcept {
  return config.mode != StreamMode::kRawOnly;
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
    case Source::kDisparity: return "disparity";
    case Source::kDepth: return "depth";
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
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input || input.tellg() < 0 || input.tellg() > 64 * 1024) {
    throw std::runtime_error("cannot read bounded encoding configuration");
  }
  const auto size = static_cast<std::size_t>(input.tellg());
  std::string document(size, '\0');
  input.seekg(0);
  input.read(document.data(), static_cast<std::streamsize>(size));
  if (!input) throw std::runtime_error("cannot read encoding configuration");
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

std::unique_ptr<FrameEncoder> make_ffmpeg_encoder(
    const Configuration& config) {
  return std::make_unique<FfmpegFrameEncoder>(config);
}

struct AsyncFrameEncoder::Impl {
  Impl(std::filesystem::path control, std::filesystem::path output,
       Configuration initial, std::filesystem::path trace)
      : control_file(std::move(control)), output_file(std::move(output)),
        trace_directory(std::move(trace)), fallback(std::move(initial)),
        config(fallback) {
    validate(fallback);
    stats_value.running = true;
    worker = std::thread(&Impl::run, this);
  }

  void refresh() {
    std::error_code error;
    const bool exists = !control_file.empty() &&
                        std::filesystem::exists(control_file, error) && !error;
    std::optional<std::filesystem::file_time_type> modified;
    if (exists) {
      modified = std::filesystem::last_write_time(control_file, error);
      if (error) modified.reset();
    }
    if (observed_file == exists && observed_modified == modified) return;
    try {
      auto updated = exists ? load_configuration(control_file, fallback)
                            : fallback;
      std::lock_guard lock(mutex);
      config = std::move(updated);
      ++generation;
      ++stats_value.configuration_reloads;
      observed_file = exists;
      observed_modified = modified;
      last_error.clear();
    } catch (const std::exception& exception) {
      std::lock_guard lock(mutex);
      ++stats_value.configuration_errors;
      last_error = exception.what();
      observed_file = exists;
      observed_modified = modified;
    }
  }

  SubmitResult submit(std::shared_ptr<const FrameSetStorage> frame) {
    if (!frame) throw std::invalid_argument("encoder cannot submit a null frame");
    refresh();
    std::lock_guard lock(mutex);
    if (stop_requested) return SubmitResult::kStopped;
    if (!encoded_enabled(config)) {
      pending.reset();
      return SubmitResult::kDisabled;
    }
    const auto capture_ns = static_cast<std::uint64_t>(
        frame->metadata.frame.capture_time_ns);
    const auto period_ns = 1'000'000'000ULL / config.cadence_hz;
    // Camera timestamps wander slightly around their nominal cadence.  Without a
    // small tolerance, a 10 Hz source arriving a few microseconds early is
    // incorrectly reduced to 5 Hz.  Ten percent (capped at 2 ms) absorbs normal
    // clock jitter without permitting a materially higher output rate.
    const auto cadence_tolerance_ns =
        std::min(period_ns / 10U, 2'000'000ULL);
    if (last_submitted_capture_ns != 0U &&
        capture_ns > last_submitted_capture_ns &&
        capture_ns - last_submitted_capture_ns + cadence_tolerance_ns <
            period_ns) {
      ++stats_value.cadence_limited;
      return SubmitResult::kCadenceLimited;
    }
    last_submitted_capture_ns = capture_ns;
    ++stats_value.submitted;
    const bool replaced = static_cast<bool>(pending);
    if (replaced) ++stats_value.pending_replaced;
    pending = std::move(frame);
    condition.notify_one();
    return replaced ? SubmitResult::kReplacedPending : SubmitResult::kAccepted;
  }

  void run() noexcept {
    std::unique_ptr<FrameEncoder> encoder;
    std::uint64_t encoder_generation = std::numeric_limits<std::uint64_t>::max();
    for (;;) {
      std::shared_ptr<const FrameSetStorage> frame;
      Configuration work_config;
      std::uint64_t work_generation{};
      std::uint64_t sequence{};
      {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return stop_requested || pending; });
        if (!pending && stop_requested) break;
        frame = std::move(pending);
        work_config = config;
        work_generation = generation;
        sequence = ++stream_sequence;
      }
      try {
        if (!encoder || encoder_generation != work_generation) {
          encoder = make_ffmpeg_encoder(work_config);
          encoder_generation = work_generation;
          sequence = 1U;
          std::lock_guard lock(mutex);
          stream_sequence = 1U;
        }
        auto encoded = encoder->encode(*frame, sequence);
        const auto bytes = serialize(encoded);
        write_atomic(output_file, bytes);
        if (!trace_directory.empty()) {
          write_atomic(trace_directory /
                           ("post_encoding_" +
                            std::to_string(encoded.identity.capture_sequence) +
                            ".nve"), bytes);
        }
        std::lock_guard lock(mutex);
        ++stats_value.encoded;
        stats_value.encoded_bytes += encoded.bytes.size();
        stats_value.last_frame_bytes = encoded.bytes.size();
        stats_value.last_encode_ms = encoded.encode_ms;
        last_error.clear();
      } catch (const std::exception& exception) {
        std::lock_guard lock(mutex);
        ++stats_value.failures;
        last_error = exception.what();
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
      condition.notify_all();
    }
    if (worker.joinable()) worker.join();
  }

  std::filesystem::path control_file;
  std::filesystem::path output_file;
  std::filesystem::path trace_directory;
  Configuration fallback;
  mutable std::mutex mutex;
  std::condition_variable condition;
  Configuration config;
  Stats stats_value{};
  std::shared_ptr<const FrameSetStorage> pending;
  std::string last_error;
  std::optional<std::filesystem::file_time_type> observed_modified;
  bool observed_file{};
  bool stop_requested{};
  std::uint64_t generation{};
  std::uint64_t stream_sequence{};
  std::uint64_t last_submitted_capture_ns{};
  std::thread worker;
};

AsyncFrameEncoder::AsyncFrameEncoder(std::filesystem::path control_file,
                                     std::filesystem::path output_file,
                                     Configuration fallback,
                                     std::filesystem::path trace_directory)
    : impl_(std::make_unique<Impl>(std::move(control_file),
                                   std::move(output_file),
                                   std::move(fallback),
                                   std::move(trace_directory))) {}

AsyncFrameEncoder::~AsyncFrameEncoder() noexcept { stop(); }

SubmitResult AsyncFrameEncoder::submit(
    std::shared_ptr<const FrameSetStorage> frame) {
  return impl_->submit(std::move(frame));
}

Configuration AsyncFrameEncoder::configuration() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->config;
}

Stats AsyncFrameEncoder::stats() const noexcept {
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
