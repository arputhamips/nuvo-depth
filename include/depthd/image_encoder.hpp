#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
  std::uint32_t keyframe_interval{30U};
  std::uint32_t threads{4U};
  bool low_latency{true};
  std::string preset{"veryfast"};
};

[[nodiscard]] bool raw_enabled(const Configuration& config) noexcept;
[[nodiscard]] bool encoded_enabled(const Configuration& config) noexcept;
[[nodiscard]] std::string_view name(StreamMode value) noexcept;
[[nodiscard]] std::string_view name(Codec value) noexcept;
[[nodiscard]] std::string_view name(Source value) noexcept;
void validate(const Configuration& config);
[[nodiscard]] Configuration parse_configuration(std::string_view document);
[[nodiscard]] std::string configuration_json(const Configuration& config);
[[nodiscard]] Configuration load_configuration(
    const std::filesystem::path& path,
    const Configuration& fallback = Configuration{});

struct EncodedFrame {
  FrameSetIdentity identity{};
  Source source{Source::kLeftRectified};
  Codec codec{Codec::kH264};
  std::uint32_t width{};
  std::uint32_t height{};
  bool keyframe{};
  std::uint64_t stream_sequence{};
  double encode_ms{};
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
};

[[nodiscard]] std::unique_ptr<FrameEncoder> make_ffmpeg_encoder(
    const Configuration& config);

enum class SubmitResult : std::uint8_t {
  kDisabled = 0,
  kCadenceLimited = 1,
  kAccepted = 2,
  kReplacedPending = 3,
  kStopped = 4,
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
  std::size_t last_frame_bytes{};
  double last_encode_ms{};
  bool running{};
};

// Encoding owns only an already-complete frame-set and runs after native depth
// processing. Its one-item pending slot always favors the newest frame; it can
// never hold a camera request or block the capture/rectification/depth worker.
class AsyncFrameEncoder final {
 public:
  AsyncFrameEncoder(std::filesystem::path control_file,
                    std::filesystem::path output_file,
                    Configuration fallback = Configuration{},
                    std::filesystem::path trace_directory = {});
  ~AsyncFrameEncoder() noexcept;

  AsyncFrameEncoder(const AsyncFrameEncoder&) = delete;
  AsyncFrameEncoder& operator=(const AsyncFrameEncoder&) = delete;

  [[nodiscard]] SubmitResult submit(
      std::shared_ptr<const FrameSetStorage> frame);
  [[nodiscard]] Configuration configuration() const;
  [[nodiscard]] Stats stats() const noexcept;
  [[nodiscard]] std::string take_last_error();
  void stop() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace depthd::encoding
