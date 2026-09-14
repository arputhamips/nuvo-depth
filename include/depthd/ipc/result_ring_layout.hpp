#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace depthd::ipc {

// This header is the authoritative cross-process ABI for Depth Daemon result
// transport. Keep it dependency-free and C++17-compatible so the ROS bridge
// can vendor the exact file without linking the native processing libraries.
inline constexpr std::uint64_t kResultRingMagic = 0x31474E4952445044ULL;
inline constexpr std::uint32_t kResultRingSchemaVersion = 1;
inline constexpr std::uint32_t kLittleEndianTag = 0x04030201U;
inline constexpr std::uint32_t kResultRingSlotCount = 4;
inline constexpr std::uint32_t kResultRingHeaderBytes = 4096;
inline constexpr std::uint32_t kResultSlotHeaderBytes = 4096;

inline constexpr std::uint32_t kImageWidth = 728;
inline constexpr std::uint32_t kImageHeight = 544;
inline constexpr std::uint32_t kDisparityStrideBytes =
    kImageWidth * sizeof(float);
inline constexpr std::uint32_t kDisparityCapacityBytes =
    kDisparityStrideBytes * kImageHeight;
inline constexpr std::uint32_t kMonoCapacityBytes =
    kImageWidth * kImageHeight;
inline constexpr std::uint32_t kJpegCapacityBytes = 512U * 1024U;
inline constexpr std::uint32_t kObstaclePointCapacity = 32768;
inline constexpr std::uint32_t kObstacleCapacityBytes =
    kObstaclePointCapacity * 3U * sizeof(float);

constexpr std::uint32_t align_up(const std::uint32_t value,
                                 const std::uint32_t alignment) noexcept {
  return (value + alignment - 1U) / alignment * alignment;
}

inline constexpr std::uint32_t kDisparityOffset = kResultSlotHeaderBytes;
inline constexpr std::uint32_t kMonoOffset = align_up(
    kDisparityOffset + kDisparityCapacityBytes, 64U);
inline constexpr std::uint32_t kJpegOffset =
    align_up(kMonoOffset + kMonoCapacityBytes, 64U);
inline constexpr std::uint32_t kObstacleOffset =
    align_up(kJpegOffset + kJpegCapacityBytes, 64U);
inline constexpr std::uint32_t kResultSlotStrideBytes = align_up(
    kObstacleOffset + kObstacleCapacityBytes, 4096U);
inline constexpr std::uint64_t kResultRingFileBytes =
    static_cast<std::uint64_t>(kResultRingHeaderBytes) +
    static_cast<std::uint64_t>(kResultRingSlotCount) *
        kResultSlotStrideBytes;

inline constexpr std::uint32_t kFrameIdCapacity = 64;
inline constexpr std::uint32_t kDistortionCoefficientCapacity = 8;

enum class ProductFlags : std::uint32_t {
  kNone = 0,
  kDisparity = 1U << 0U,
  kLeftCameraInfo = 1U << 1U,
  kRightCameraInfo = 1U << 2U,
  kLeftRectifiedJpeg = 1U << 3U,
  kLeftRectifiedMono = 1U << 4U,
  kObstacleCloud = 1U << 5U,
};

constexpr ProductFlags operator|(const ProductFlags left,
                                 const ProductFlags right) noexcept {
  return static_cast<ProductFlags>(static_cast<std::uint32_t>(left) |
                                   static_cast<std::uint32_t>(right));
}

constexpr bool has_flag(const std::uint32_t flags,
                        const ProductFlags flag) noexcept {
  return (flags & static_cast<std::uint32_t>(flag)) != 0U;
}

enum class HealthState : std::uint32_t {
  kStarting = 0,
  kOk = 1,
  kDegraded = 2,
  kStale = 3,
  kFault = 4,
};

enum class DistortionModel : std::uint32_t {
  kPlumbBob = 1,
};

struct CameraModel {
  std::uint32_t width{kImageWidth};
  std::uint32_t height{kImageHeight};
  DistortionModel distortion_model{DistortionModel::kPlumbBob};
  std::uint32_t distortion_count{};
  std::array<double, kDistortionCoefficientCapacity> d{};
  std::array<double, 9> k{};
  std::array<double, 9> r{};
  std::array<double, 12> p{};
};

// The fields read concurrently by the bridge are explicitly 8-byte aligned.
// Implementations must use acquire/release atomic operations on the raw
// integers; the ABI itself intentionally contains no std::atomic objects.
struct alignas(64) ResultRingHeader {
  std::uint64_t magic{kResultRingMagic};
  std::uint32_t schema_version{kResultRingSchemaVersion};
  std::uint32_t endian_tag{kLittleEndianTag};
  std::uint32_t header_bytes{kResultRingHeaderBytes};
  std::uint32_t slot_header_bytes{kResultSlotHeaderBytes};
  std::uint32_t slot_count{kResultRingSlotCount};
  std::uint32_t slot_stride_bytes{kResultSlotStrideBytes};
  std::uint64_t file_bytes{kResultRingFileBytes};
  std::uint32_t disparity_offset{kDisparityOffset};
  std::uint32_t disparity_capacity_bytes{kDisparityCapacityBytes};
  std::uint32_t mono_offset{kMonoOffset};
  std::uint32_t mono_capacity_bytes{kMonoCapacityBytes};
  std::uint32_t jpeg_offset{kJpegOffset};
  std::uint32_t jpeg_capacity_bytes{kJpegCapacityBytes};
  std::uint32_t obstacle_offset{kObstacleOffset};
  std::uint32_t obstacle_capacity_bytes{kObstacleCapacityBytes};
  alignas(8) std::uint64_t instance_id{};
  alignas(8) std::uint64_t latest_sequence{};
  alignas(8) std::uint64_t latest_slot{};
  std::int64_t created_realtime_ns{};
  std::uint64_t writer_pid{};
  std::array<std::uint64_t, 8> reserved{};
};

struct alignas(64) ResultSlotHeader {
  alignas(8) std::uint64_t begin_guard{};
  alignas(8) std::uint64_t end_guard{};
  std::uint64_t sequence{};
  std::uint64_t instance_id{};
  std::int64_t capture_realtime_ns{};
  std::int64_t ready_realtime_ns{};
  std::uint64_t calibration_id{};
  HealthState health{HealthState::kStarting};
  std::uint32_t product_flags{};
  std::uint32_t width{kImageWidth};
  std::uint32_t height{kImageHeight};
  std::uint32_t disparity_stride_bytes{kDisparityStrideBytes};
  std::uint32_t valid_window_x_offset{128};
  std::uint32_t valid_window_y_offset{};
  std::uint32_t valid_window_width{kImageWidth - 128U};
  std::uint32_t valid_window_height{kImageHeight};
  float focal_length_px{493.542046188F};
  float baseline_m{0.100F};
  float min_disparity{};
  float max_disparity{128.0F};
  float delta_d{0.0625F};
  std::uint32_t disparity_bytes{};
  std::uint32_t mono_bytes{};
  std::uint32_t jpeg_bytes{};
  std::uint32_t obstacle_point_count{};
  CameraModel left_camera{};
  CameraModel right_camera{};
  std::array<char, kFrameIdCapacity> left_optical_frame{};
  std::array<char, kFrameIdCapacity> right_optical_frame{};
  std::array<char, kFrameIdCapacity> obstacle_frame{};
  std::uint64_t sampled_pixels{};
  std::uint64_t accepted_raw_points{};
  std::uint64_t accumulated_voxels{};
  std::uint64_t supported_voxels{};
  std::uint64_t accepted_clusters{};
  std::uint64_t voxel_capacity_overflow{};
  std::array<std::uint64_t, 8> reserved{};
};

static_assert(std::is_standard_layout_v<CameraModel>);
static_assert(std::is_trivially_copyable_v<CameraModel>);
static_assert(std::is_standard_layout_v<ResultRingHeader>);
static_assert(std::is_trivially_copyable_v<ResultRingHeader>);
static_assert(std::is_standard_layout_v<ResultSlotHeader>);
static_assert(std::is_trivially_copyable_v<ResultSlotHeader>);
static_assert(sizeof(ResultRingHeader) <= kResultRingHeaderBytes);
static_assert(sizeof(ResultSlotHeader) <= kResultSlotHeaderBytes);
static_assert(kResultRingFileBytes == 11620352ULL);

}  // namespace depthd::ipc
