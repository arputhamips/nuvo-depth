#include "depthd/native_output_c.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <string>

#include "depthd/native_output.hpp"

struct depthd_native_output_handle {
  std::unique_ptr<depthd::NativeOutputPublisher> publisher;
};

namespace {

void set_error(char* destination, const std::size_t capacity,
               const std::string& message) noexcept {
  if (destination == nullptr || capacity == 0U) {
    return;
  }
  const auto size = std::min(capacity - 1U, message.size());
  std::memcpy(destination, message.data(), size);
  destination[size] = '\0';
}

void clear_error(char* destination, const std::size_t capacity) noexcept {
  if (destination != nullptr && capacity > 0U) {
    destination[0] = '\0';
  }
}

bool valid_health(const std::uint32_t value) noexcept {
  return value <=
         static_cast<std::uint32_t>(depthd::ipc::HealthState::kFault);
}

}  // namespace

extern "C" {

void depthd_native_output_default_config(
    depthd_native_output_config* config) {
  if (config == nullptr) {
    return;
  }
  std::memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  const auto transform = depthd::make_nuvo_left_optical_to_base_transform();
  std::copy(transform.matrix.begin(), transform.matrix.end(),
            config->optical_to_base);
}

depthd_native_output_handle* depthd_native_output_create(
    const depthd_native_output_config* config,
    char* error,
    const std::size_t error_capacity) {
  clear_error(error, error_capacity);
  try {
    if (config == nullptr || config->struct_size != sizeof(*config)) {
      throw std::invalid_argument("native output config ABI size mismatch");
    }
    depthd::NativeOutputConfig native;
    if (config->result_ring_path != nullptr &&
        config->result_ring_path[0] != '\0') {
      native.ring.path = config->result_ring_path;
    }
    std::copy(std::begin(config->optical_to_base),
              std::end(config->optical_to_base),
              native.optical_to_obstacle_frame.matrix.begin());
    auto handle = std::make_unique<depthd_native_output_handle>();
    handle->publisher =
        std::make_unique<depthd::NativeOutputPublisher>(std::move(native));
    return handle.release();
  } catch (const std::exception& exception) {
    set_error(error, error_capacity, exception.what());
    return nullptr;
  }
}

int depthd_native_output_publish(depthd_native_output_handle* handle,
                                 const depthd_native_output_frame* frame,
                                 depthd_native_output_stats* stats,
                                 char* error,
                                 const std::size_t error_capacity) {
  clear_error(error, error_capacity);
  try {
    if (handle == nullptr || handle->publisher == nullptr) {
      throw std::invalid_argument("native output handle is null");
    }
    if (frame == nullptr || frame->struct_size != sizeof(*frame)) {
      throw std::invalid_argument("native output frame ABI size mismatch");
    }
    if (stats == nullptr || stats->struct_size != sizeof(*stats)) {
      throw std::invalid_argument("native output stats ABI size mismatch");
    }
    if (!valid_health(frame->health)) {
      throw std::invalid_argument("native output health value is invalid");
    }
    if (frame->left_rectified_mono_bytes > 0U &&
        frame->left_rectified_mono == nullptr) {
      throw std::invalid_argument("native output mono pointer is null");
    }
    if (frame->left_rectified_jpeg_bytes > 0U &&
        frame->left_rectified_jpeg == nullptr) {
      throw std::invalid_argument("native output JPEG pointer is null");
    }

    depthd::NativeOutputFrameView native;
    native.capture_realtime_ns = frame->capture_realtime_ns;
    native.health = static_cast<depthd::ipc::HealthState>(frame->health);
    native.disparity = {
        frame->disparity,
        frame->width,
        frame->height,
        frame->disparity_stride_elements,
        128,
        0,
        depthd::ipc::kImageWidth - 128U,
        depthd::ipc::kImageHeight,
        0.0F,
        128.0F,
    };
    native.left_rectified_mono = std::span<const std::uint8_t>(
        frame->left_rectified_mono, frame->left_rectified_mono_bytes);
    native.left_rectified_jpeg = std::span<const std::uint8_t>(
        frame->left_rectified_jpeg, frame->left_rectified_jpeg_bytes);
    const auto result = handle->publisher->publish(native);
    stats->sequence = result.sequence;
    stats->obstacle_point_count = result.obstacle_point_count;
    stats->obstacle_cloud_ms = result.obstacle_cloud_ms;
    stats->result_ring_publish_ms = result.result_ring_publish_ms;
    stats->total_ms = result.total_ms;
    stats->sampled_pixels = result.obstacle_cloud.sampled_pixels;
    stats->accepted_raw_points =
        result.obstacle_cloud.accepted_raw_points;
    stats->accumulated_voxels =
        result.obstacle_cloud.accumulated_voxels;
    stats->supported_voxels = result.obstacle_cloud.supported_voxels;
    stats->accepted_clusters = result.obstacle_cloud.accepted_clusters;
    stats->voxel_capacity_overflow =
        result.obstacle_cloud.voxel_capacity_overflow;
    return 0;
  } catch (const std::exception& exception) {
    set_error(error, error_capacity, exception.what());
    return -1;
  }
}

void depthd_native_output_destroy(depthd_native_output_handle* handle) {
  delete handle;
}

}  // extern "C"
