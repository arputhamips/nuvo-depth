#pragma once

#include <mutex>
#include <shared_mutex>

namespace depthd::opencv_execution {

// OpenCV's worker count is process-global. Independent backend instances may
// compute concurrently, but changing that count must wait for all of them.
inline std::shared_mutex execution_mutex;

[[nodiscard]] inline auto compute_guard() {
  return std::shared_lock<std::shared_mutex>(execution_mutex);
}

[[nodiscard]] inline auto configuration_guard() {
  return std::unique_lock<std::shared_mutex>(execution_mutex);
}

}  // namespace depthd::opencv_execution
