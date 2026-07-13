#include "depthd/semantic_output_c.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "depthd/semantic_ring.hpp"

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

#if defined(__linux__) || defined(__APPLE__)
std::filesystem::path temporary_ring_path() {
  return std::filesystem::temp_directory_path() /
      ("depthd-semantic-output-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".ring");
}
#endif

}  // namespace

int main() {
#if defined(__linux__) || defined(__APPLE__)
  const auto path = temporary_ring_path();
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  depthd_semantic_output_config config{};
  depthd_semantic_output_default_config(&config);
  const auto path_string = path.string();
  config.semantic_ring_path = path_string.c_str();
  config.target_frame = "base_link";
  char error[512]{};
  auto* handle = depthd_semantic_output_create(&config, error, sizeof(error));
  expect(handle != nullptr, std::string("C publisher creates: ") + error);
  if (handle == nullptr) {
    return 1;
  }

  std::vector<float> disparity(728U * 544U,
      static_cast<float>(493.542046188 * 0.161458780 / 2.0));
  depthd_semantic_observation observation{};
  observation.struct_size = sizeof(observation);
  observation.source_kind = 1U;
  observation.semantic_class = 1U;
  observation.observation_id = 5U;
  observation.model_class_id = 0U;
  observation.model_label = "person";
  observation.model_name = "fake_yolo";
  observation.confidence = 0.9F;
  observation.x_min = 300.0F;
  observation.y_min = 180.0F;
  observation.x_max = 430.0F;
  observation.y_max = 480.0F;
  depthd_semantic_model_run model{};
  model.struct_size = sizeof(model);
  model.model_name = "fake_yolo";
  model.model_version = "test-v1";
  model.model_sha256 = "test-sha";
  model.health = 1U;
  model.completed_runs = 1U;
  model.latency_ms = 2.0;
  model.inference_latency_ms = 1.5;
  model.postprocess_latency_ms = 0.5;
  depthd_semantic_output_frame frame{};
  frame.struct_size = sizeof(frame);
  frame.source_sequence = 9U;
  frame.capture_realtime_ns = 1'780'000'000'000'000'000LL;
  frame.ready_realtime_ns = frame.capture_realtime_ns + 20'000'000LL;
  frame.health = 1U;
  frame.disparity = disparity.data();
  frame.width = 728U;
  frame.height = 544U;
  frame.disparity_stride_elements = 728U;
  frame.valid_x_offset = 128U;
  frame.valid_width = 600U;
  frame.valid_height = 544U;
  frame.min_disparity = 0.0F;
  frame.max_disparity = 128.0F;
  frame.observations = &observation;
  frame.observation_count = 1U;
  frame.model_runs = &model;
  frame.model_run_count = 1U;
  frame.submitted_frames = 1U;
  frame.processed_frames = 1U;
  frame.published_results = 1U;
  frame.frame_latency_ms = 2.5;
  frame.max_frame_latency_ms = 2.5;
  depthd_semantic_output_stats stats{};
  stats.struct_size = sizeof(stats);
  expect(depthd_semantic_output_publish(handle, &frame, &stats, error,
                                         sizeof(error)) == 0,
         std::string("C publisher writes fused result: ") + error);
  expect(stats.semantic_sequence == 1U && stats.object_count == 1U,
         "C seam publishes one fused object");
  expect(stats.hazard_active == 1U &&
             stats.hazard_fail_closed_required == 0U,
         "ranged human activates the normal stop policy");

  depthd::SemanticRingReader reader(path_string);
  depthd::SemanticRingSnapshot snapshot;
  expect(reader.read_latest(snapshot).status ==
             depthd::SemanticRingReadStatus::kOk,
         "C output is readable through the versioned C++ ring contract");
  expect(snapshot.header.core.source_sequence == 9U &&
             snapshot.objects[0].core.range_valid == 1U,
         "C output preserves sequence and native fusion result");
  expect(std::string(
             snapshot.header.core.models[0].core.model_version.data()) ==
             "test-v1" &&
             snapshot.header.core.models[0].core.postprocess_latency_ms ==
                 0.5F,
         "C seam preserves model provenance and split latency");

  frame.source_sequence = 10U;
  frame.health = 2U;
  frame.observations = nullptr;
  frame.observation_count = 0U;
  frame.ready_realtime_ns += 100'000'000LL;
  frame.capture_realtime_ns += 100'000'000LL;
  frame.published_results = 2U;
  expect(depthd_semantic_output_publish(handle, &frame, &stats, error,
                                         sizeof(error)) == 0,
         "fault result remains publishable");
  expect(stats.hazard_active == 1U &&
             stats.hazard_fail_closed_required == 1U,
         "degraded health cannot become an empty clear hazard");

  depthd_semantic_output_destroy(handle);
  std::filesystem::remove(path, ignored);
#endif
  return failures == 0 ? 0 : 1;
}
