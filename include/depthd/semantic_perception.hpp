#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "depthd/semantic_backend.hpp"
#include "depthd/semantic_fusion.hpp"

namespace depthd {

struct SemanticBackendStats {
  std::string model_name{};
  std::uint64_t completed_runs{};
  std::uint64_t errors{};
  double last_latency_ms{};
  double max_latency_ms{};
};

struct SemanticPerceptionStats {
  std::uint64_t submitted_frames{};
  std::uint64_t replaced_pending_frames{};
  std::uint64_t processed_frames{};
  std::uint64_t published_results{};
  std::uint64_t worker_errors{};
  double last_frame_latency_ms{};
  double max_frame_latency_ms{};
  std::vector<SemanticBackendStats> backends{};
};

struct SemanticPerceptionConfig {
  SemanticFusionConfig fusion{};
  std::function<void(const SemanticFrameResult&)> result_callback{};
};

class SemanticPerceptionModule {
 public:
  SemanticPerceptionModule(
      std::vector<SemanticBackendRegistration> backends,
      SemanticPerceptionConfig config = {});
  ~SemanticPerceptionModule();

  SemanticPerceptionModule(const SemanticPerceptionModule&) = delete;
  SemanticPerceptionModule& operator=(const SemanticPerceptionModule&) = delete;

  // Capacity-one latest-only handoff. This never waits for inference.
  [[nodiscard]] bool submit(SemanticFrame frame) noexcept;
  [[nodiscard]] SemanticPerceptionStats stats() const;
  [[nodiscard]] std::optional<SemanticFrameResult> latest_result() const;
  [[nodiscard]] bool wait_for_result_after(
      std::uint64_t source_sequence,
      std::chrono::milliseconds timeout) const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace depthd
