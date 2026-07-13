#pragma once

#include "depthd/rectification_backend.hpp"

namespace depthd {

inline constexpr std::string_view kOpenCvRectificationBackendName =
    "cpu_opencv";

// Adds the production OpenCV implementation to a caller-owned registry.
// Registration is explicit so runtime construction remains deterministic.
void register_opencv_rectification_backend(
    RectificationBackendRegistry& registry);

}  // namespace depthd
