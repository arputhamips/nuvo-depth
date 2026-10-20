#include "depthd/image_encoder.hpp"

#include <algorithm>
#include <atomic>
#include <future>
#include <array>
#include <cassert>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

void require(const bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

std::array<int, 3> decoded_luma_samples(
    const depthd::encoding::EncodedFrame& encoded) {
  const auto codec_id = encoded.codec == depthd::encoding::Codec::kH264
                            ? AV_CODEC_ID_H264
                            : AV_CODEC_ID_MJPEG;
  const auto* decoder = avcodec_find_decoder(codec_id);
  require(decoder != nullptr, "test decoder is unavailable");
  auto* context = avcodec_alloc_context3(decoder);
  auto* packet = av_packet_alloc();
  auto* decoded = av_frame_alloc();
  require(context != nullptr && packet != nullptr && decoded != nullptr,
          "test decoder allocation failed");
  require(avcodec_open2(context, decoder, nullptr) >= 0,
          "test decoder open failed");
  require(av_new_packet(packet, static_cast<int>(encoded.bytes.size())) >= 0,
          "test packet allocation failed");
  std::memcpy(packet->data, encoded.bytes.data(), encoded.bytes.size());
  require(avcodec_send_packet(context, packet) >= 0,
          "test decoder rejected packet");
  require(avcodec_receive_frame(context, decoded) >= 0,
          "test decoder produced no frame");
  require(decoded->width == 64 && decoded->height == 48,
          "test decoded frame dimensions differ");
  const std::array<int, 3> samples{
      decoded->data[0][8 * decoded->linesize[0] + 32],
      decoded->data[0][24 * decoded->linesize[0] + 32],
      decoded->data[0][40 * decoded->linesize[0] + 32]};
  av_frame_free(&decoded);
  av_packet_free(&packet);
  avcodec_free_context(&context);
  return samples;
}

std::array<int, 3> decoded_center_yuv(
    const depthd::encoding::EncodedFrame& encoded) {
  const auto codec_id = encoded.codec == depthd::encoding::Codec::kH264
                            ? AV_CODEC_ID_H264
                            : AV_CODEC_ID_MJPEG;
  const auto* decoder = avcodec_find_decoder(codec_id);
  require(decoder != nullptr, "color test decoder is unavailable");
  auto* context = avcodec_alloc_context3(decoder);
  auto* packet = av_packet_alloc();
  auto* decoded = av_frame_alloc();
  require(context != nullptr && packet != nullptr && decoded != nullptr,
          "color test decoder allocation failed");
  require(avcodec_open2(context, decoder, nullptr) >= 0,
          "color test decoder open failed");
  require(av_new_packet(packet, static_cast<int>(encoded.bytes.size())) >= 0,
          "color test packet allocation failed");
  std::memcpy(packet->data, encoded.bytes.data(), encoded.bytes.size());
  require(avcodec_send_packet(context, packet) >= 0,
          "color test decoder rejected packet");
  require(avcodec_receive_frame(context, decoded) >= 0,
          "color test decoder produced no frame");
  require(decoded->width == 64 && decoded->height == 48 &&
              decoded->data[1] != nullptr && decoded->data[2] != nullptr,
          "decoded color frame layout differs");
  const std::array<int, 3> samples{
      decoded->data[0][24 * decoded->linesize[0] + 32],
      decoded->data[1][12 * decoded->linesize[1] + 16],
      decoded->data[2][12 * decoded->linesize[2] + 16]};
  av_frame_free(&decoded);
  av_packet_free(&packet);
  avcodec_free_context(&context);
  return samples;
}

std::shared_ptr<depthd::FrameSetStorage> frame() {
  auto result = std::make_shared<depthd::FrameSetStorage>();
  result->metadata.frame.device_uuid[0] = 1U;
  result->metadata.frame.run_uuid[0] = 2U;
  result->metadata.frame.capture_epoch = 1U;
  result->metadata.frame.capture_sequence = 1U;
  result->metadata.frame.configuration_epoch = 1U;
  result->metadata.frame.capture_time_ns = 1'000'000'000;
  result->metadata.frame.capture_clock = depthd::CaptureClock::kBoottime;
  const auto grid = depthd::make_reference_grid(64U, 48U);
  const depthd::RegionU32 roi{0U, 0U, 64U, 48U};
  result->metadata.left_rectified = {
      grid, roi, depthd::CameraRole::kLogicalLeft,
      depthd::ImageProductFormat::kGray8};
  result->metadata.right_rectified = {
      grid, roi, depthd::CameraRole::kLogicalRight,
      depthd::ImageProductFormat::kGray8};
  result->metadata.disparity = {
      grid, roi, 0.0F, 64.0F, 0.0625F, true,
      depthd::ConfidenceModel::kBinarySgbmSupportV1,
      depthd::DisparityDerivation::kNativeMatcherGrid};
  result->metadata.depth = {grid, 0.1F, 12.0F};
  const auto pixels = static_cast<std::size_t>(grid.width) * grid.height;
  result->left_rectified.resize(pixels);
  result->right_rectified.resize(pixels);
  result->disparity.assign(pixels, 8.0F);
  result->depth_m.assign(pixels, 2.0F);
  result->confidence.assign(pixels, 255U);
  result->validity.assign(pixels, 1U);
  for (std::size_t index = 0; index < pixels; ++index) {
    result->left_rectified[index] = static_cast<std::uint8_t>(index % 256U);
    result->right_rectified[index] =
        static_cast<std::uint8_t>((index + 8U) % 256U);
  }
  return result;
}

std::uint32_t u32(const std::byte* value) {
  return (std::to_integer<std::uint32_t>(value[0]) << 24U) |
         (std::to_integer<std::uint32_t>(value[1]) << 16U) |
         (std::to_integer<std::uint32_t>(value[2]) << 8U) |
         std::to_integer<std::uint32_t>(value[3]);
}


template <class Predicate>
void eventually(Predicate predicate, const char* message) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(predicate(), message);
}

struct EncodeGate {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered{};
  bool release{};
};

struct ReleaseEncoder {
  std::shared_ptr<EncodeGate> gate;
  ~ReleaseEncoder() {
    std::lock_guard lock(gate->mutex);
    gate->release = true;
    gate->condition.notify_all();
  }
};

class BlockingEncoder final : public depthd::encoding::FrameEncoder {
 public:
  explicit BlockingEncoder(std::shared_ptr<EncodeGate> gate)
      : gate_(std::move(gate)) {}
  depthd::encoding::EncodedFrame encode(
      const depthd::FrameSetStorage& input, const std::uint64_t sequence) override {
    {
      std::unique_lock lock(gate_->mutex);
      gate_->entered = true;
      gate_->condition.notify_all();
      gate_->condition.wait(lock, [&] { return gate_->release; });
    }
    depthd::encoding::EncodedFrame result;
    result.identity = input.metadata.frame;
    result.codec = depthd::encoding::Codec::kJpeg;
    result.mime_type = "image/jpeg";
    result.codec_string = "jpeg";
    result.width = 64U;
    result.height = 48U;
    result.keyframe = true;
    result.stream_sequence = sequence;
    result.bytes = {std::byte{0xff}, std::byte{0xd8}, std::byte{0xff}};
    return result;
  }
 private:
  std::shared_ptr<EncodeGate> gate_;
};


class PreviewCopyRectifier final : public depthd::RectificationBackend {
 public:
  std::string_view name() const noexcept override { return "test-preview-copy"; }
  void rectify(depthd::RectificationCamera, const depthd::ImageView& input,
               const depthd::MutableImageView& output,
               depthd::RectificationStats&) override {
    const auto channels = input.encoding == depthd::ImageEncoding::kGray8 ? 1U : 3U;
    for (std::uint32_t row = 0; row < input.height; ++row) {
      std::copy_n(input.data + row * input.stride_bytes, input.width * channels,
                  output.data + row * output.stride_bytes);
    }
  }
};

struct ColorProbe {
  std::array<std::array<int, 3>, 3> samples{};
};

class ColorProbeEncoder final : public depthd::encoding::FrameEncoder {
 public:
  explicit ColorProbeEncoder(std::shared_ptr<ColorProbe> probe) : probe_(std::move(probe)) {}
  depthd::encoding::EncodedFrame encode(const depthd::FrameSetStorage&,
                                        std::uint64_t) override {
    throw std::runtime_error("camera color preview entered the numeric branch");
  }
  depthd::encoding::EncodedFrame encode_image(const depthd::ImageView& image,
      const depthd::ProductSetMetadata& metadata, const std::uint64_t sequence) override {
    require(image.encoding == depthd::ImageEncoding::kRgb8,
            "camera color conversion did not produce RGB");
    for (std::size_t band = 0; band < 3U; ++band) {
      const auto* pixel = image.data + (8U + band * 16U) * image.stride_bytes + 32U * 3U;
      for (std::size_t channel = 0; channel < 3U; ++channel)
        probe_->samples[band][channel] = pixel[channel];
    }
    depthd::encoding::EncodedFrame result;
    result.identity = metadata.frame;
    result.codec = depthd::encoding::Codec::kJpeg;
    result.width = image.width;
    result.height = image.height;
    result.mime_type = "image/jpeg";
    result.codec_string = "jpeg";
    result.keyframe = true;
    result.stream_sequence = sequence;
    result.bytes = {std::byte{0xff}, std::byte{0xd8}, std::byte{0xff}};
    return result;
  }
 private:
  std::shared_ptr<ColorProbe> probe_;
};

void test_sycc_full_range_conversion() {
  using depthd::encoding::Source;
  const auto root = std::filesystem::temp_directory_path() / "depthd-sycc-range-test";
  const auto input = frame();
  const auto reader = [](depthd::CameraRole, const bool color, std::span<std::uint8_t> bytes) {
    require(color && bytes.size() == 64U * 48U * 3U / 2U,
            "sYCC test did not receive exact I420 storage");
    for (std::size_t y = 0; y < 48U; ++y)
      std::fill_n(bytes.data() + y * 64U, 64U, y < 16U ? 16U : y < 32U ? 235U : 100U);
    for (std::size_t y = 0; y < 24U; ++y) {
      std::fill_n(bytes.data() + 64U * 48U + y * 32U, 32U, y < 16U ? 128U : 90U);
      std::fill_n(bytes.data() + 64U * 48U * 5U / 4U + y * 32U, 32U, y < 16U ? 128U : 180U);
    }
  };
  for (const auto source : {Source::kLeftRgb, Source::kLeftRectifiedRgb}) {
    depthd::encoding::Configuration config;
    config.codec = depthd::encoding::Codec::kJpeg;
    config.source = source;
    auto probe = std::make_shared<ColorProbe>();
    depthd::encoding::AsyncFrameEncoder encoder({}, root / "latest.nve", config, {},
        [probe](const auto&) { return std::make_unique<ColorProbeEncoder>(probe); },
        [] { return std::make_unique<PreviewCopyRectifier>(); });
    require(encoder.submit_capture(input->metadata, reader) == depthd::encoding::SubmitResult::kAccepted,
            "sYCC preview was not admitted");
    eventually([&] { return encoder.stats().encoded == 1U; }, "sYCC preview failed");
    encoder.stop();
    for (std::size_t channel = 0; channel < 3U; ++channel) {
      require(std::abs(probe->samples[0][channel] - 16) <= 1 &&
                  std::abs(probe->samples[1][channel] - 235) <= 1,
              "sYCC full-range luma was expanded as limited range");
      // Rec.601 full range: R=Y+1.402Cr, G=Y-.344136Cb-.714136Cr,
      // B=Y+1.772Cb, with Y=100, Cb=-38 and Cr=52.
      constexpr std::array<int, 3> reference{173, 76, 33};
      require(std::abs(probe->samples[2][channel] - reference[channel]) <= 2,
              "sYCC preview used the wrong chroma coefficients");
    }
  }
  input->left_color_yuv420.resize(64U * 48U * 3U / 2U);
  reader(depthd::CameraRole::kLogicalLeft, true, input->left_color_yuv420);
  for (const auto codec : {depthd::encoding::Codec::kJpeg, depthd::encoding::Codec::kH264}) {
    depthd::encoding::Configuration config;
    config.codec = codec;
    config.source = Source::kLeftColor;
    const auto encoded = depthd::encoding::make_ffmpeg_encoder(config)->encode(*input, 1U);
    const auto samples = decoded_luma_samples(encoded);
    const auto expected = codec == depthd::encoding::Codec::kJpeg
        ? std::array<int, 3>{16, 235, 100} : std::array<int, 3>{30, 218, 102};
    for (std::size_t band = 0; band < 3U; ++band)
      require(std::abs(samples[band] - expected[band]) <= 2,
              "encoded sYCC luma did not match the codec output range");
  }
  std::filesystem::remove_all(root);
}

void test_independent_capture_sources() {
  using depthd::encoding::Source;
  const auto root = std::filesystem::temp_directory_path() / "depthd-preview-sources-test";
  const auto output = root / "latest.nve";
  auto metadata = frame()->metadata;
  metadata.calibration = depthd::calibration_identity_from_sha256(std::string(64U, 'a'));
  metadata.calibration_authority = depthd::CalibrationAuthority::kUserValidated;
  for (const auto source : {Source::kLeftRectified, Source::kRightRectified,
      Source::kLeftColor, Source::kRightColor, Source::kLeftMono, Source::kRightMono,
      Source::kLeftRgb, Source::kRightRgb, Source::kLeftRectifiedRgb,
      Source::kRightRectifiedRgb, Source::kRectifiedStereo, Source::kRectifiedStereoRgb}) {
    depthd::encoding::Configuration config;
    config.codec = depthd::encoding::Codec::kJpeg;
    config.source = source;
    if (source == Source::kRectifiedStereoRgb) config.codec = depthd::encoding::Codec::kH264;
    config.width = 64U;
    config.height = 48U;
    require(depthd::encoding::parse_configuration(
        depthd::encoding::configuration_json(config)).source == source,
        "extended image source did not round-trip");
    unsigned factories{};
    unsigned copies{};
    std::vector<depthd::CameraRole> copied_roles;
    depthd::encoding::AsyncFrameEncoder encoder({}, output, config, {}, {}, [&] {
      ++factories;
      return std::make_unique<PreviewCopyRectifier>();
    });
    encoder.set_demand(0.0);
    const auto reader = [&](depthd::CameraRole role, const bool color,
                            std::span<std::uint8_t> pixels) {
      ++copies;
      copied_roles.push_back(role);
      std::fill(pixels.begin(), pixels.end(), role == depthd::CameraRole::kLogicalLeft ? 110U : 150U);
      if (color) {
        std::fill(pixels.begin() + 64U * 48U, pixels.begin() + 64U * 48U * 5U / 4U, 32U);
        std::fill(pixels.begin() + 64U * 48U * 5U / 4U, pixels.end(), 224U);
      }
    };
    require(encoder.submit_capture(metadata, reader) == depthd::encoding::SubmitResult::kDisabled &&
                factories == 0U && copies == 0U,
            "idle preview allocated rectification/copies");
    encoder.set_demand(10.0);
    require(encoder.submit_capture(metadata, reader) == depthd::encoding::SubmitResult::kAccepted,
            "active preview capture was not admitted");
    eventually([&] { return encoder.stats().encoded == 1U; },
               "extended image source failed to encode independently");
    auto next = metadata;
    next.frame.capture_sequence = 2U;
    next.frame.capture_time_ns += 16'666'667;
    require(encoder.submit_capture(next, reader) == depthd::encoding::SubmitResult::kCadenceLimited,
            "preview cadence copied an early capture");
    encoder.stop();
    const bool stereo = source == Source::kRectifiedStereo || source == Source::kRectifiedStereoRgb;
    require(copies == (stereo ? 2U : 1U) && encoder.stats().image_copies == copies &&
                encoder.stats().failures == 0U,
            "preview source copied an unselected camera or failed");
    const bool right = source == Source::kRightRectified || source == Source::kRightColor ||
        source == Source::kRightMono || source == Source::kRightRgb || source == Source::kRightRectifiedRgb;
    require(copied_roles.front() == (right ? depthd::CameraRole::kLogicalRight :
                                            depthd::CameraRole::kLogicalLeft) &&
                (!stereo || copied_roles.back() == depthd::CameraRole::kLogicalRight),
            "preview copied the wrong logical camera");
    std::ifstream encoded_file(output, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(encoded_file)), {});
    require(bytes.size() > depthd::encoding::kEncodedHeaderBytes, "preview envelope missing");
    const auto metadata_bytes = u32(reinterpret_cast<const std::byte*>(bytes.data()) + 8U);
    const std::string encoded_metadata(bytes.data() + depthd::encoding::kEncodedHeaderBytes, metadata_bytes);
    require(encoded_metadata.find(std::string(64U, 'a')) != std::string::npos &&
                encoded_metadata.find(std::string(depthd::encoding::name(source))) != std::string::npos,
            "preview lost capture calibration/source provenance");
    require(encoder.stats().applied_revision.size() == 64U &&
                encoded_metadata.find(encoder.stats().applied_revision) != std::string::npos,
            "published preview did not identify its applied control document");
    depthd::encoding::EncodedFrame encoded;
    encoded.codec = config.codec;
    const auto payload_offset = depthd::encoding::kEncodedHeaderBytes + metadata_bytes;
    encoded.bytes.assign(reinterpret_cast<const std::byte*>(bytes.data() + payload_offset),
                         reinterpret_cast<const std::byte*>(bytes.data() + bytes.size()));
    const auto samples = decoded_center_yuv(encoded);
    const bool color = source == Source::kLeftColor || source == Source::kRightColor ||
        source == Source::kLeftRgb || source == Source::kRightRgb ||
        source == Source::kLeftRectifiedRgb || source == Source::kRightRectifiedRgb ||
        source == Source::kRectifiedStereoRgb;
    require(color ? samples[1] < 90 && samples[2] > 170 :
                    samples[1] > 110 && samples[1] < 145 && samples[2] > 110 && samples[2] < 145,
            "independent preview lost its selected mono/RGB representation");
  }
  std::filesystem::remove_all(root);
}

void test_event_publication_without_snapshot_and_inflight_stop() {
  auto gate = std::make_shared<EncodeGate>();
  auto config = depthd::encoding::Configuration{};
  config.codec = depthd::encoding::Codec::kJpeg;
  std::atomic<unsigned> publications{}, invalidations{};
  depthd::encoding::AsyncFrameEncoder encoder({}, {}, config, {},
      [gate](const auto&) { return std::make_unique<BlockingEncoder>(gate); }, {},
      [&](const auto& identity, std::uint64_t sequence, std::span<const std::byte> bytes) {
        if (bytes.empty()) { ++invalidations; return; }
        require(identity.capture_sequence == 1U && sequence > 0U && bytes.size() > 32U,
                "event output lost frame identity or serialized bytes");
        ++publications;
      });
  const ReleaseEncoder release_on_failure{gate};
  require(encoder.submit(frame()) == depthd::encoding::SubmitResult::kAccepted,
          "event encoder rejected frame");
  eventually([&] { std::lock_guard lock(gate->mutex); return gate->entered; }, "event encoder did not start");
  {
    std::lock_guard lock(gate->mutex); gate->release = true; gate->condition.notify_all();
  }
  eventually([&] { return publications == 1U; }, "event encoder still requires a snapshot file");
  encoder.set_demand(0.0);
  require(invalidations >= 2U, "disconnect did not invalidate event output");
  encoder.set_demand(10.0);
  {
    std::lock_guard lock(gate->mutex); gate->entered = false; gate->release = false;
  }
  auto next = frame(); next->metadata.frame.capture_sequence = 8U;
  next->metadata.frame.capture_time_ns += 1'000'000'000;
  require(encoder.submit(next) == depthd::encoding::SubmitResult::kAccepted, "inflight stop frame was rejected");
  eventually([&] { std::lock_guard lock(gate->mutex); return gate->entered; }, "inflight stop encode did not start");
  auto stopped = std::async(std::launch::async, [&] { encoder.stop(); });
  eventually([&] { return encoder.prepare_capture(next->metadata.frame).result == depthd::encoding::SubmitResult::kStopped; },
             "encoder did not begin stop");
  {
    std::lock_guard lock(gate->mutex); gate->release = true; gate->condition.notify_all();
  }
  stopped.get();
  require(publications == 1U && encoder.stats().failures == 0U,
          "stopping encoder published a stale in-flight result");
}

void test_image_completion_does_not_consume_numeric_plan() {
  using namespace depthd::encoding;
  auto config = Configuration{};
  config.codec = Codec::kJpeg;
  config.source = Source::kDepth;
  std::atomic<unsigned> publications{};
  AsyncFrameEncoder async({}, {}, config, {}, {}, {},
      [&](const auto&, std::uint64_t, std::span<const std::byte> bytes) {
        if (!bytes.empty()) ++publications;
      });
  auto complete = frame();
  const auto plan = async.prepare_capture(complete->metadata.frame);
  require(plan.result == SubmitResult::kAccepted, "numeric preview not admitted");
  require(async.commit_native_capture(plan), "numeric capture commit failed");
  auto image = frame();
  image->available_products = depthd::ipc_v2::product_bit(depthd::ipc_v2::ProductKind::kLeftMono);
  require(async.submit_native(image, plan) == SubmitResult::kPreviewUnavailable,
          "image-only completion consumed a numeric encoder plan");
  require(async.submit_native(complete, plan) == SubmitResult::kAccepted,
          "numeric completion was starved by its earlier image");
  eventually([&] { return publications == 1U; }, "numeric completion was not encoded");
  async.stop();
  require(async.stats().failures == 0U, "numeric completion caused an encoding failure");
}

void test_capture_plans_and_reload() {
  using depthd::encoding::SubmitResult;
  const auto root = std::filesystem::temp_directory_path() / "depthd-encoder-reload-test";
  std::filesystem::create_directories(root);
  const auto control = root / "encoding.json";
  const auto output = root / "latest.nve";
  auto config = depthd::encoding::Configuration{};
  config.codec = depthd::encoding::Codec::kJpeg;
  config.source = depthd::encoding::Source::kLeftColor;
  unsigned revision{};
  const auto write_control = [&] {
    {
      std::ofstream file(control, std::ios::trunc);
      file << depthd::encoding::configuration_json(config);
    }
    std::filesystem::last_write_time(control,
        std::filesystem::file_time_type::clock::now() +
        std::chrono::seconds(++revision));
  };
  write_control();
  auto gate = std::make_shared<EncodeGate>();
  depthd::encoding::AsyncFrameEncoder async(control, output, config, {},
      [gate](const auto&) { return std::make_unique<BlockingEncoder>(gate); });
  const ReleaseEncoder release_on_failure{gate};
  auto input = frame();
  input->left_color_yuv420.assign(64U * 48U * 3U / 2U, 128U);
  const auto first = async.prepare_capture(input->metadata.frame);
  require(first.result == SubmitResult::kAccepted &&
              first.color_preview == depthd::ColorPreviewRequest::kLeft,
          "first color preflight failed");
  require(async.submit(input, first) == SubmitResult::kAccepted,
          "prepared color frame was rejected");
  eventually([&] {
    std::lock_guard lock(gate->mutex);
    return gate->entered;
  }, "blocking encoder did not start");
  require(async.stats().applied_revision.empty() &&
              async.stats().requested_revision.size() == 64U,
          "loading controls was reported as an applied encoder output");
  auto skipped = frame();
  skipped->metadata.frame.capture_sequence = 2U;
  skipped->metadata.frame.capture_time_ns += 16'666'667;
  const auto skipped_plan = async.prepare_capture(skipped->metadata.frame);
  require(skipped_plan.result == SubmitResult::kCadenceLimited &&
              skipped_plan.color_preview == depthd::ColorPreviewRequest::kNone,
          "60 Hz early capture requested an unnecessary color mapping");
  require(async.submit(skipped, skipped_plan) == SubmitResult::kCadenceLimited,
          "skipped color capture reached the encoder");
  auto due = frame();
  due->metadata.frame.capture_sequence = 7U;
  due->metadata.frame.capture_time_ns += 100'000'000;
  const auto due_plan = async.prepare_capture(due->metadata.frame);
  // No timestamp was consumed by preparing this subsequently dropped capture.
  auto replacement = frame();
  replacement->metadata.frame.capture_sequence = 8U;
  replacement->metadata.frame.capture_time_ns += 116'666'667;
  const auto replacement_plan = async.prepare_capture(replacement->metadata.frame);
  require(due_plan.color_preview == depthd::ColorPreviewRequest::kLeft &&
              replacement_plan.color_preview == depthd::ColorPreviewRequest::kLeft,
          "dropped capture reservation starved the newest replacement");
  require(async.submit(replacement, replacement_plan) == SubmitResult::kPreviewUnavailable,
          "missing optional preview was treated as an encode failure");
  require(async.stats().failures == 0U, "skipped preview incremented encode failures");
  config.source = depthd::encoding::Source::kRightColor;
  write_control();
  async.refresh_configuration();
  require(async.submit(due, due_plan) == SubmitResult::kConfigurationChanged,
          "old-source capture survived configuration reload");
  const auto right_plan = async.prepare_capture(replacement->metadata.frame);
  require(right_plan.color_preview == depthd::ColorPreviewRequest::kRight,
          "new source preflight retained stale color selection/cadence");
  config.mode = depthd::encoding::StreamMode::kRawOnly;
  write_control();
  async.refresh_configuration();
  {
    std::lock_guard lock(gate->mutex);
    gate->release = true;
    gate->condition.notify_all();
  }
  eventually([&] { return async.stats().stale_discarded >= 2U; },
             "old in-flight encoder output was not discarded");
  require(!std::filesystem::exists(output) && async.stats().encoded == 0U,
          "disabled/reconfigured encoder published stale output");
  config.mode = depthd::encoding::StreamMode::kBoth;
  write_control();
  async.refresh_configuration();
  auto reset = frame();
  reset->metadata.frame.run_uuid[0] = 9U;
  reset->metadata.frame.capture_epoch = 2U;
  reset->metadata.frame.capture_time_ns = 500'000'000;
  reset->right_color_yuv420.assign(64U * 48U * 3U / 2U, 128U);
  const auto reset_plan = async.prepare_capture(reset->metadata.frame);
  require(reset_plan.result == SubmitResult::kAccepted &&
              reset_plan.color_preview == depthd::ColorPreviewRequest::kRight,
          "new run/epoch retained prior cadence timestamp");
  require(async.submit(reset, reset_plan) == SubmitResult::kAccepted,
          "new run color frame was rejected");
  eventually([&] { return async.stats().encoded == 1U; },
             "new run did not produce a fresh encoded frame");
  async.stop();
  require(async.stats().failures == 0U, "reload safety caused encoder failures");
  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  auto config = depthd::encoding::parse_configuration(
      "{\"schema_version\":1,\"mode\":\"both\",\"codec\":\"jpeg\","
      "\"source\":\"left_rectified\",\"width\":0,\"height\":0,"
      "\"cadence_hz\":10,\"bitrate_bps\":4000000,\"jpeg_quality\":80,"
      "\"keyframe_interval\":30,\"threads\":2,\"low_latency\":true,"
      "\"preset\":\"veryfast\"}");
  assert(depthd::encoding::raw_enabled(config));
  assert(depthd::encoding::encoded_enabled(config));
  assert(depthd::encoding::parse_configuration(
             depthd::encoding::configuration_json(config)).codec ==
         depthd::encoding::Codec::kJpeg);

  const auto input = frame();
  // Predictive GOPs remain supported by standalone codecs, but never by the
  // latest-frame publisher or public live controls.
  auto predictive = config;
  predictive.codec = depthd::encoding::Codec::kH264;
  depthd::encoding::validate(predictive);
  bool rejected_predictive = false;
  try { depthd::encoding::validate_latest_configuration(predictive); }
  catch (const std::invalid_argument&) { rejected_predictive = true; }
  require(rejected_predictive, "latest publisher accepted predictive H.264");
  auto jpeg = depthd::encoding::make_ffmpeg_encoder(config)->encode(*input, 1U);
  assert(jpeg.mime_type == "image/jpeg");
  assert(jpeg.keyframe && jpeg.width == 64U && jpeg.height == 48U);
  assert(jpeg.bytes.size() > 100U);
  assert(jpeg.bytes[0] == std::byte{0xff} && jpeg.bytes[1] == std::byte{0xd8});
  const auto envelope = depthd::encoding::serialize(jpeg);
  assert(envelope.size() > jpeg.bytes.size() +
                              depthd::encoding::kEncodedHeaderBytes);
  assert(envelope[0] == std::byte{'N'} && envelope[3] == std::byte{'1'});
  assert(u32(envelope.data() + 8U) > 100U);

  config.codec = depthd::encoding::Codec::kH264;
  auto h264 = depthd::encoding::make_ffmpeg_encoder(config)->encode(*input, 1U);
  assert(h264.mime_type == "video/h264" && h264.keyframe);
  assert(h264.bytes.size() > 100U);

  // ISP-native I420 must retain chroma through both supported codecs. A gray
  // fallback would make both decoded chroma samples approximately 128.
  const auto color = frame();
  const auto color_pixels = 64U * 48U;
  color->left_color_yuv420.resize(color_pixels * 3U / 2U);
  std::fill_n(color->left_color_yuv420.begin(), color_pixels, 128U);
  std::fill_n(color->left_color_yuv420.begin() + color_pixels,
              color_pixels / 4U, 32U);
  std::fill_n(color->left_color_yuv420.begin() + color_pixels * 5U / 4U,
              color_pixels / 4U, 224U);
  config.source = depthd::encoding::Source::kLeftColor;
  require(depthd::encoding::parse_configuration(
              depthd::encoding::configuration_json(config)).source ==
              depthd::encoding::Source::kLeftColor,
          "color source did not round-trip through configuration");
  for (const auto color_codec : {depthd::encoding::Codec::kJpeg,
                                 depthd::encoding::Codec::kH264}) {
    config.codec = color_codec;
    const auto encoded =
        depthd::encoding::make_ffmpeg_encoder(config)->encode(*color, 1U);
    const auto samples = decoded_center_yuv(encoded);
    require(samples[0] > 105 && samples[0] < 150,
            "color source luminance was not preserved");
    require(samples[1] < 80 && samples[2] > 180,
            "color source chroma was discarded or swapped");
  }
  config.source = depthd::encoding::Source::kLeftRectified;
  config.codec = depthd::encoding::Codec::kH264;

  // The encoded preview must use the frame's actual disparity range, not a
  // fixed 64 px scale.  Check the decoded pixels so conversion and codec
  // behavior are covered together.
  auto contrast = frame();
  contrast->metadata.disparity.accepted_max_disparity_px = 96.0F;
  for (std::size_t y = 0; y < 48U; ++y) {
    const auto value = y < 16U ? std::numeric_limits<float>::quiet_NaN()
                       : y < 32U ? 32.0F
                                 : 80.0F;
    std::fill_n(contrast->disparity.begin() + y * 64U, 64U, value);
  }
  config.source = depthd::encoding::Source::kDisparity;
  for (const auto codec : {depthd::encoding::Codec::kJpeg,
                           depthd::encoding::Codec::kH264}) {
    config.codec = codec;
    const auto encoded =
        depthd::encoding::make_ffmpeg_encoder(config)->encode(*contrast, 1U);
    const auto samples = decoded_luma_samples(encoded);
    require(samples[0] < 20, "invalid disparity did not map to black");
    require(samples[1] > 70 && samples[1] < 105,
            "midrange disparity preview has the wrong level");
    require(samples[2] > 195 && samples[2] < 230,
            "high disparity preview was clipped or mis-scaled");
  }
  contrast->metadata.disparity.accepted_max_disparity_px = 0.0F;
  bool rejected_invalid_range = false;
  try {
    (void)depthd::encoding::make_ffmpeg_encoder(config)->encode(*contrast, 1U);
  } catch (const std::invalid_argument&) {
    rejected_invalid_range = true;
  }
  require(rejected_invalid_range, "invalid disparity range was accepted");
  config.codec = depthd::encoding::Codec::kH264;

  // Native disparity/depth grids may be odd even though YUV420 is not.  The
  // encoder must make the native result usable without requiring the UI to
  // know the current stereo working-grid dimensions.
  const auto odd = frame();
  const auto odd_grid = depthd::make_reference_grid(63U, 47U);
  odd->metadata.disparity.grid = odd_grid;
  odd->metadata.disparity.valid_roi = {0U, 0U, 63U, 47U};
  odd->disparity.assign(63U * 47U, 8.0F);
  config.source = depthd::encoding::Source::kDisparity;
  auto odd_h264 =
      depthd::encoding::make_ffmpeg_encoder(config)->encode(*odd, 1U);
  assert(odd_h264.width == 62U && odd_h264.height == 46U &&
         odd_h264.keyframe && odd_h264.bytes.size() > 100U);
  config.source = depthd::encoding::Source::kLeftRectified;
  config.keyframe_interval = 1U;
  auto independent_encoder = depthd::encoding::make_ffmpeg_encoder(config);
  for (const auto sequence : {1U, 7U, 32U}) {
    const auto encoded = independent_encoder->encode(*input, sequence);
    require(encoded.keyframe, "latest H.264 frame is predictive");
    // A fresh decoder for every selected frame proves skipped predecessors
    // do not provide a hidden dependency, including SPS/PPS state.
    (void)decoded_center_yuv(encoded);
  }
  for (const auto format : {depthd::ImageProductFormat::kBgr8,
                            depthd::ImageProductFormat::kRgb8}) {
    auto colored = frame();
    colored->metadata.left_rectified.format = format;
    colored->left_rectified.assign(64U * 48U * 3U, 0U);
    for (std::size_t index = 0; index < 64U * 48U; ++index) {
      colored->left_rectified[index * 3U +
          (format == depthd::ImageProductFormat::kBgr8 ? 0U : 2U)] = 255U;
    }
    const auto encoded = depthd::encoding::make_ffmpeg_encoder(config)->encode(*colored, 1U);
    const auto samples = decoded_center_yuv(encoded);
    require(samples[1] > 180 && samples[2] < 150,
            "rectified BGR/RGB encoder source discarded or swapped chroma");
  }

  const auto root = std::filesystem::temp_directory_path() /
                    "depthd-image-encoder-test";
  std::filesystem::create_directories(root);
  const auto control = root / "encoding.json";
  const auto output = root / "latest.nve";
  {
    std::ofstream file(control);
    file << depthd::encoding::configuration_json(config);
  }
  depthd::encoding::AsyncFrameEncoder async(control, output, config);
  assert(async.submit(input) == depthd::encoding::SubmitResult::kAccepted);
  for (int attempt = 0; attempt < 100 && !std::filesystem::exists(output);
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const auto jittered = frame();
  jittered->metadata.frame.capture_sequence = 2U;
  jittered->metadata.frame.capture_time_ns = 1'099'000'000;
  assert(async.submit(jittered) !=
         depthd::encoding::SubmitResult::kCadenceLimited);
  for (int attempt = 0; attempt < 100 && async.stats().encoded < 2U;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  config.mode = depthd::encoding::StreamMode::kRawOnly;
  {
    std::ofstream file(control, std::ios::trunc);
    file << depthd::encoding::configuration_json(config);
  }
  std::filesystem::last_write_time(
      control, std::filesystem::file_time_type::clock::now() +
                   std::chrono::seconds(1));
  const auto disabled = frame();
  disabled->metadata.frame.capture_sequence = 3U;
  disabled->metadata.frame.capture_time_ns = 1'200'000'000;
  assert(async.submit(disabled) ==
         depthd::encoding::SubmitResult::kDisabled);
  async.stop();
  assert(!std::filesystem::exists(output));
  assert(async.stats().encoded == 2U && async.stats().failures == 0U);
  assert(async.stats().configuration_reloads == 2U);
  std::filesystem::remove_all(root);
  test_capture_plans_and_reload();
  test_image_completion_does_not_consume_numeric_plan();
  test_event_publication_without_snapshot_and_inflight_stop();
  test_independent_capture_sources();
  test_sycc_full_range_conversion();

  bool rejected{};
  try {
    config.threads = 5U;
    depthd::encoding::validate(config);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected);
}
