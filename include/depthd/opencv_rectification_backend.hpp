#pragma once

#include "depthd/rectification_backend.hpp"

namespace depthd {

inline constexpr std::string_view kOpenCvRectificationBackendName =
    "cpu_opencv";

// Immutable fixed-point maps may be shared across workers; each backend keeps
// its own DMA staging buffers. The definition deliberately stays OpenCV-private.
struct PreparedOpenCvRectificationMaps;
[[nodiscard]] std::shared_ptr<const PreparedOpenCvRectificationMaps>
prepare_opencv_rectification_maps(const StereoRectificationMaps& maps);
[[nodiscard]] std::unique_ptr<RectificationBackend>
make_opencv_rectification_backend(
    const RectificationBackendConfig& config,
    std::shared_ptr<const PreparedOpenCvRectificationMaps> maps);

// Adds the production OpenCV implementation to a caller-owned registry.
// Registration is explicit so runtime construction remains deterministic.
void register_opencv_rectification_backend(
    RectificationBackendRegistry& registry);

}  // namespace depthd
