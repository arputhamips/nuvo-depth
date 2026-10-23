#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "depthd/runtime.hpp"

namespace depthd::encoding {

enum class StreamMode : std::uint8_t {
  kRawOnly = 0,
  kEncodedOnly = 1,
  kBoth = 2,
};

enum class Codec : std::uint8_t {
  kJpeg = 0,
  kH264 = 1,
};

enum class Source : std::uint8_t {
  kLeftRectified = 0,
  kRightRectified = 1,
  kDisparity = 2,
  kDepth = 3,
  kLeftColor = 4,
  kRightColor = 5,
  kLeftMono = 6,
  kRightMono = 7,
  kLeftRgb = 8,
  kRightRgb = 9,
  kLeftRectifiedRgb = 10,
  kRightRectifiedRgb = 11,
  kRectifiedStereo = 12,
  kRectifiedStereoRgb = 13,
};

struct Configuration {
  std::uint32_t schema_version{1U};
  StreamMode mode{StreamMode::kBoth};
  Codec codec{Codec::kH264};
  Source source{Source::kLeftRectified};
  // Zero preserves the selected product's native dimension. Non-zero output
  // dimensions must be supplied as an even width/height pair.
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t cadence_hz{10U};
  std::uint32_t bitrate_bps{4'000'000U};
  std::uint32_t jpeg_quality{80U};
  std::uint32_t keyframe_interval{1U};
  std::uint32_t threads{1U};
  bool low_latency{true};
  std::string preset{"veryfast"};
};

[[nodiscard]] bool raw_enabled(const Configuration& config) noexcept;
[[nodiscard]] bool encoded_enabled(const Configuration& config) noexcept;
[[nodiscard]] bool requires_depth(Source source) noexcept;
[[nodiscard]] std::string_view name(StreamMode value) noexcept;
[[nodiscard]] std::string_view name(Codec value) noexcept;
[[nodiscard]] std::string_view name(Source value) noexcept;
void validate(const Configuration& config);
// Latest-frame delivery can replace or skip any access unit. H.264 therefore
// requires independent frames. Standalone/offline FrameEncoder still permits
// predictive GOPs through validate().
void validate_latest_configuration(const Configuration& config);
[[nodiscard]] Configuration parse_configuration(std::string_view document);
[[nodiscard]] std::string configuration_json(const Configuration& config);
[[nodiscard]] Configuration load_configuration(
    const std::filesystem::path& path,
    const Configuration& fallback = Configuration{});

struct EncodedFrame {
  FrameSetIdentity identity{};
  CalibrationIdentity calibration{};
  CalibrationAuthority calibration_authority{
      CalibrationAuthority::kProvisionalRecalibrationRequired};
  std::uint64_t encoding_configuration_generation{};
  std::string encoding_applied_revision;
  Source source{Source::kLeftRectified};
  Codec codec{Codec::kH264};
  std::uint32_t width{};
  std::uint32_t height{};
  bool keyframe{};
  std::uint64_t stream_sequence{};
  double encode_ms{};
  double preparation_ms{};
  double queue_ms{};
  double total_ms{};
  std::string mime_type;
  std::string codec_string;
  std::vector<std::byte> bytes;
};

inline constexpr std::size_t kEncodedHeaderBytes = 32U;
inline constexpr std::size_t kMaximumEncodedMetadataBytes = 64U * 1024U;
inline constexpr std::size_t kMaximumEncodedPayloadBytes = 32U * 1024U * 1024U;

[[nodiscard]] std::vector<std::byte> serialize(const EncodedFrame& frame);

class FrameEncoder {
 public:
  virtual ~FrameEncoder() = default;
  [[nodiscard]] virtual EncodedFrame encode(
      const FrameSetStorage& frame, std::uint64_t stream_sequence) = 0;
  [[nodiscard]] virtual EncodedFrame encode_image(
      const ImageView& image, const ProductSetMetadata& metadata,
      std::uint64_t stream_sequence);
  [[nodiscard]] virtual bool accepts_yuv420() const noexcept { return false; }
  [[nodiscard]] virtual EncodedFrame encode_yuv420(
      std::span<const std::uint8_t> packed, std::uint32_t width,
      std::uint32_t height, const ProductSetMetadata& metadata,
      std::uint64_t stream_sequence);
};

[[nodiscard]] std::unique_ptr<FrameEncoder> make_ffmpeg_encoder(
    const Configuration& config);

enum class SubmitResult : std::uint8_t {
  kDisabled = 0,
  kCadenceLimited = 1,
  kAccepted = 2,
  kReplacedPending = 3,
  kStopped = 4,
  kConfigurationChanged = 5,
  kPreviewUnavailable = 6,
};

struct CapturePlan {
  FrameSetIdentity identity{};
  std::uint64_t generation{};
  ColorPreviewRequest color_preview{ColorPreviewRequest::kNone};
  SubmitResult result{SubmitResult::kDisabled};
};

struct Stats {
  std::uint64_t submitted{};
  std::uint64_t cadence_limited{};
  std::uint64_t pending_replaced{};
  std::uint64_t encoded{};
  std::uint64_t failures{};
  std::uint64_t configuration_reloads{};
  std::uint64_t configuration_errors{};
  std::uint64_t encoded_bytes{};
  std::uint64_t capture_preflights{};
  std::uint64_t capture_cadence_limited{};
  std::uint64_t color_preview_requests{};
  std::uint64_t stale_discarded{};
  std::uint64_t preview_unavailable{};
  std::uint64_t image_admitted{};
  std::uint64_t image_copies{};
  std::uint64_t image_pool_dropped{};
  std::uint64_t image_rectified{};
  std::uint64_t image_color_conversions{};
  std::uint64_t numeric_admitted{};
  double demand_cadence_hz{};
  std::uint64_t configuration_generation{1U};
  std::string requested_revision;
  std::string applied_revision;
  std::string configuration_error;
  std::string last_error;
  std::size_t last_frame_bytes{};
  double last_encode_ms{};
  double last_preparation_ms{};
  double last_queue_ms{};
  double last_total_ms{};
  bool running{};
};

// Image previews admit capture copies directly into a bounded owning pool and
// rectify/encode on this private worker, independently from stereo matching.
// Numeric disparity/depth previews instead accept completed native frame sets.
// One pending slot favors the newest frame. No queued work retains a camera
// request or blocks the capture/rectification/depth worker.
class AsyncFrameEncoder final {
 public:
  using EncoderFactory =
      std::function<std::unique_ptr<FrameEncoder>(const Configuration&)>;
  using PreviewRectifierFactory =
      std::function<std::unique_ptr<RectificationBackend>()>;
  // A generation-checked immutable publication; empty bytes invalidate it.
  // An empty output_file disables diagnostic file snapshots.
  using OutputPublisher = std::function<void(
      const FrameSetIdentity&, std::uint64_t, std::span<const std::byte>)>;
  // Copies tightly packed mono or ISP I420 into already allocated pool memory.
  // Called synchronously only after demand, cadence, and pool admission.
  using CaptureImageReader = std::function<void(
      CameraRole, bool color, std::span<std::uint8_t>)>;
  AsyncFrameEncoder(std::filesystem::path control_file,
                    std::filesystem::path output_file,
                    Configuration fallback = Configuration{},
                    std::filesystem::path trace_directory = {},
                    EncoderFactory encoder_factory = {},
                    PreviewRectifierFactory preview_rectifier_factory = {},
                    OutputPublisher output_publisher = {});
  ~AsyncFrameEncoder() noexcept;

  AsyncFrameEncoder(const AsyncFrameEncoder&) = delete;
  AsyncFrameEncoder& operator=(const AsyncFrameEncoder&) = delete;

  [[nodiscard]] SubmitResult submit(
      std::shared_ptr<const FrameSetStorage> frame);
  [[nodiscard]] SubmitResult submit(
      std::shared_ptr<const FrameSetStorage> frame, const CapturePlan& plan);
  // Native completions can outlive source/demand changes. Only accepted
  // numeric plans enter the encoder; raw-only/default plans are skipped before
  // strict capture association checks.
  [[nodiscard]] SubmitResult submit_native(
      std::shared_ptr<const FrameSetStorage> frame, const CapturePlan& plan);
  // Ask before mapping/copying optional ISP color. A plan does not consume
  // cadence: dropped captures must not starve a slow latest-frame processor.
  // Keep the returned value with this pair's publication context.
  [[nodiscard]] CapturePlan prepare_capture(const FrameSetIdentity& identity);
  // Commit numeric cadence after its protected native request enters the
  // scheduler. Failed admission leaves the next capture due.
  [[nodiscard]] bool commit_native_capture(const CapturePlan& plan);
  [[nodiscard]] SubmitResult submit_capture(
      const ProductSetMetadata& metadata, const CaptureImageReader& reader);
  // Native daemon sets this from persistent subscribed consumers. Zero drops
  // pending work and invalidates the latest encoded output immediately.
  void set_demand(double cadence_hz);
  [[nodiscard]] Configuration configuration() const;
  // Refreshes an atomically replaced control file before capture decides
  // whether an optional color plane is needed for the next frame.
  void refresh_configuration();
  [[nodiscard]] Stats stats() const;
  [[nodiscard]] std::string take_last_error();
  void stop() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace depthd::encoding
