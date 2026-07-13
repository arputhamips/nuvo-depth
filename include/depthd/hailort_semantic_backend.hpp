#pragma once

#include <memory>
#include <string>
#include <vector>

#include "depthd/semantic_backend.hpp"

namespace depthd {

struct HailortSemanticModelConfig {
  std::string yolo_hef_path{};
  std::string segformer_hef_path{};
  double yolo_hz{5.0};
  double segformer_hz{5.0};
};

// Creates all registered Hailo semantic backends around one persistent VDevice.
// The returned backends share that owner and the native worker invokes them
// serially, so the single Hailo-8L is never opened by competing model owners.
[[nodiscard]] std::vector<SemanticBackendRegistration>
make_hailort_semantic_backends(const HailortSemanticModelConfig& config);

}  // namespace depthd
