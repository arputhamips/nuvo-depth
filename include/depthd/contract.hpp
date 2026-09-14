#pragma once

#include <cstdint>
#include <string_view>

namespace depthd {

inline constexpr std::uint32_t kImageWidth = 728;
inline constexpr std::uint32_t kImageHeight = 544;
inline constexpr std::uint32_t kModelInputWidth = 736;
inline constexpr std::uint32_t kModelInputHeight = 544;
inline constexpr float kRectifiedFocalLengthPx = 493.542046188F;
inline constexpr float kBaselineM = 0.100F;
inline constexpr std::string_view kLeftOpticalFrame = "camera_front_left_optical";
inline constexpr std::string_view kRightOpticalFrame = "camera_front_right_optical";

enum class PixelEncoding : std::uint16_t {
  kDisparityFloat32 = 1,
  kJpeg = 2,
};

enum class HealthState : std::uint16_t {
  kStarting = 0,
  kOk = 1,
  kDegraded = 2,
  kStale = 3,
  kFault = 4,
};

// Transport-neutral metadata shared by the daemon, replay tools, and adapters.
// Payload ownership and IPC mechanics deliberately do not live in this type.
struct FrameMetadata {
  std::uint64_t sequence{};
  std::int64_t capture_time_ns{};
  std::int64_t ready_time_ns{};
  std::uint32_t width{kImageWidth};
  std::uint32_t height{kImageHeight};
  std::uint32_t stride_bytes{kImageWidth * sizeof(float)};
  PixelEncoding encoding{PixelEncoding::kDisparityFloat32};
  HealthState health{HealthState::kStarting};
  std::uint64_t calibration_id{};
};

[[nodiscard]] bool is_valid(const FrameMetadata& metadata) noexcept;

}  // namespace depthd
