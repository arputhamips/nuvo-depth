#pragma once

#include <memory>
#include <string>

#include "depthd/semantic_types.hpp"

namespace depthd {

struct SemanticBackendInfo {
  std::string backend_name{};
  std::string model_name{};
  std::string model_version{};
  std::string model_sha256{};
  SemanticSourceKind output_kind{SemanticSourceKind::kInstance};
};

struct SemanticBackendTiming {
  double inference_latency_ms{};
  double postprocess_latency_ms{};
};

class ISemanticBackend {
 public:
  virtual ~ISemanticBackend() = default;
  [[nodiscard]] virtual SemanticBackendInfo info() const = 0;
  [[nodiscard]] virtual SemanticBackendOutput infer(
      const SemanticImageView& image) = 0;
  [[nodiscard]] virtual SemanticBackendTiming last_timing() const noexcept {
    return {};
  }
};

struct SemanticBackendRegistration {
  std::unique_ptr<ISemanticBackend> backend{};
  double cadence_hz{5.0};
};

}  // namespace depthd
