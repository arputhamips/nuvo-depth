#include "depthd/image_encoder.hpp"

#include <algorithm>
#include <array>
#include <cassert>
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
  assert(std::filesystem::file_size(output) > 100U);
  assert(async.stats().encoded == 2U && async.stats().failures == 0U);
  assert(async.stats().configuration_reloads == 2U);
  std::filesystem::remove_all(root);

  bool rejected{};
  try {
    config.threads = 5U;
    depthd::encoding::validate(config);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected);
}
