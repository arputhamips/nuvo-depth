#include "depthd/semantic_ring.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

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
  const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("depthd-semantic-ring-" + std::to_string(token) + ".ring");
}

depthd::SemanticFrameResult result() {
  depthd::SemanticFrameResult value;
  value.source_sequence = 42U;
  value.capture_realtime_ns = 1'780'000'000'000'000'000LL;
  value.ready_realtime_ns = value.capture_realtime_ns + 30'000'000LL;
  value.health = depthd::SemanticHealthState::kOk;
  depthd::FusedSemanticObject person;
  person.observation.source_kind = depthd::SemanticSourceKind::kInstance;
  person.observation.semantic_class = depthd::SemanticClass::kHuman;
  person.observation.observation_id = 8U;
  person.observation.model_class_id = 0U;
  person.observation.model_name = "yolov8s";
  person.observation.model_label = "person";
  person.observation.confidence = 0.91F;
  person.observation.box = {10.0F, 20.0F, 30.0F, 50.0F};
  person.range_valid = true;
  person.point = {1.5F, 0.2F, 0.3F};
  person.range_m = 1.51F;
  person.bearing_rad = 0.13F;
  person.range_stddev_m = 0.08F;
  person.valid_fraction = 0.75F;
  person.valid_samples = 30U;
  value.objects.push_back(person);
  auto region = person;
  region.observation.source_kind = depthd::SemanticSourceKind::kDenseRegion;
  region.observation.semantic_class = depthd::SemanticClass::kVegetation;
  region.observation.observation_id = 9U;
  region.observation.model_name = "segformer_b0_bn";
  region.observation.model_label = "vegetation";
  region.observation.pixel_count = 1200U;
  value.objects.push_back(region);
  value.hazard.active = true;
  value.hazard.source_observation_id = 8U;
  value.hazard.semantic_class = depthd::SemanticClass::kHuman;
  value.hazard.range_m = 1.51F;
  value.hazard.bearing_rad = 0.13F;
  value.hazard.reason = "living_obstacle_in_stop_zone";
  value.models.push_back(
      {"yolov8s", "robot-validated", "abc123",
       depthd::SemanticHealthState::kOk, 7U, 0U, 12.5, 10.0, 2.5});
  value.models.push_back(
      {"segformer_b0_bn", "v2.17.0", "def456",
       depthd::SemanticHealthState::kOk, 5U, 0U, 18.0, 14.0, 4.0});
  return value;
}

depthd::SemanticPerceptionStats stats() {
  depthd::SemanticPerceptionStats value;
  value.submitted_frames = 10U;
  value.replaced_pending_frames = 2U;
  value.processed_frames = 8U;
  value.published_results = 7U;
  value.last_frame_latency_ms = 31.0;
  value.max_frame_latency_ms = 40.0;
  return value;
}

void write_u32(const std::filesystem::path& path, const std::uint64_t offset,
               const std::uint32_t value) {
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  file.seekp(static_cast<std::streamoff>(offset));
  file.write(reinterpret_cast<const char*>(&value), sizeof(value));
  file.flush();
}

void write_u64(const std::filesystem::path& path, const std::uint64_t offset,
               const std::uint64_t value) {
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  file.seekp(static_cast<std::streamoff>(offset));
  file.write(reinterpret_cast<const char*>(&value), sizeof(value));
  file.flush();
}
#endif

}  // namespace

int main() {
  expect(depthd::ipc::kSemanticRingSchemaVersion == 1U,
         "semantic ABI starts at schema version one");
  expect(depthd::ipc::kSemanticRingSlotCount == 4U,
         "semantic ring has four newest-result slots");
  expect(depthd::ipc::kSemanticObjectCapacity == 64U,
         "semantic ring object capacity is fixed");
  expect(sizeof(depthd::ipc::SemanticObjectRecord) == 256U,
         "semantic object ABI size is fixed");
  expect(sizeof(depthd::ipc::SemanticModelRecord) == 256U,
         "semantic model provenance ABI size is fixed");

#if defined(__linux__) || defined(__APPLE__)
  const auto path = temporary_ring_path();
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  depthd::SemanticRingConfig config;
  config.path = path.string();
  depthd::SemanticRingWriter writer(config);
  depthd::SemanticRingReader reader(path.string());
  depthd::SemanticRingSnapshot output;
  expect(reader.read_latest(output).status ==
             depthd::SemanticRingReadStatus::kUnavailable,
         "new semantic ring has no complete result");

  auto semantic_result = result();
  auto module_stats = stats();
  const std::vector<std::uint8_t> preview{0xff, 0xd8, 1U, 2U, 0xff, 0xd9};
  depthd::SemanticRingSnapshotView view;
  view.result = &semantic_result;
  view.stats = &module_stats;
  view.preview_jpeg = preview;
  expect(writer.publish(view) == 1U, "first semantic sequence is one");
  const auto read = reader.read_latest(output);
  expect(read.status == depthd::SemanticRingReadStatus::kOk,
         "reader accepts a stable semantic slot");
  expect(output.header.core.source_sequence == 42U,
         "depth source sequence is preserved");
  expect(output.header.core.capture_realtime_ns ==
             semantic_result.capture_realtime_ns,
         "capture timestamp is preserved exactly");
  expect(output.header.core.instance_count == 1U &&
             output.header.core.dense_region_count == 1U,
         "instance and dense region counts stay distinct");
  expect(output.objects.size() == 2U &&
             output.objects[0].core.observation_id == 8U &&
             output.objects[1].core.pixel_count == 1200U,
         "fixed semantic records preserve compact results");
  expect(output.header.core.hazard.core.active == 1U &&
             output.header.core.hazard.core.semantic_class ==
                 static_cast<std::uint16_t>(depthd::SemanticClass::kHuman),
         "hazard state is transported with the observations");
  expect(std::string(output.header.core.models[0].core.model_version.data()) ==
                 "robot-validated" &&
             std::string(output.header.core.models[1].core.model_sha256.data()) ==
                 "def456" &&
             output.header.core.models[1].core.postprocess_latency_ms == 4.0F,
         "model version, hash, and postprocess latency are preserved");
  expect(output.preview_jpeg == preview,
         "optional preview bytes are present only when supplied");
  expect(reader.read_latest(output).status ==
             depthd::SemanticRingReadStatus::kStale,
         "same semantic sequence is not delivered twice");

  const auto schema_offset =
      offsetof(depthd::ipc::SemanticRingHeader, core) +
      offsetof(depthd::ipc::SemanticRingHeaderCore, schema_version);
  write_u32(path, schema_offset, 99U);
  depthd::SemanticRingReader schema_reader(path.string());
  expect(schema_reader.read_latest(output).status ==
             depthd::SemanticRingReadStatus::kSchemaMismatch,
         "incompatible semantic schemas fail closed");
  write_u32(path, schema_offset, depthd::ipc::kSemanticRingSchemaVersion);

  const auto slot_offset = depthd::ipc::kSemanticRingHeaderBytes +
      (1U % depthd::ipc::kSemanticRingSlotCount) *
          depthd::ipc::kSemanticSlotStrideBytes;
  write_u64(path, slot_offset, 3U);
  depthd::SemanticRingReader torn_reader(path.string());
  const auto torn = torn_reader.read_latest(output);
  expect(torn.status == depthd::SemanticRingReadStatus::kTorn &&
             torn.torn_reads >= 1U,
         "odd generation guards reject torn semantic data");

  std::filesystem::remove(path, ignored);
#endif
  return failures == 0 ? 0 : 1;
}
