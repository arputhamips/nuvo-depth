#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "depthd/ipc/semantic_ring_layout.hpp"
#include "depthd/semantic_perception.hpp"

namespace depthd {

struct SemanticRingConfig {
  std::string path{"/run/depthd/semantic.ring"};
  std::uint32_t file_mode{0644};
  std::string target_frame{"base_link"};
};

struct SemanticRingSnapshotView {
  const SemanticFrameResult* result{};
  const SemanticPerceptionStats* stats{};
  std::span<const std::uint8_t> preview_jpeg{};
};

struct SemanticRingSnapshot {
  ipc::SemanticSlotHeader header{};
  std::vector<ipc::SemanticObjectRecord> objects{};
  std::vector<std::uint8_t> preview_jpeg{};
};

enum class SemanticRingReadStatus {
  kOk,
  kMissing,
  kSchemaMismatch,
  kTorn,
  kStale,
  kRestarted,
  kUnavailable,
};

struct SemanticRingReadResult {
  SemanticRingReadStatus status{SemanticRingReadStatus::kUnavailable};
  std::string message{};
  std::uint64_t observed_sequence{};
  std::uint64_t dropped_sequences{};
  std::uint64_t torn_reads{};
  std::uint64_t schema_mismatches{};
};

class SemanticRingWriter {
 public:
  explicit SemanticRingWriter(SemanticRingConfig config = {});
  ~SemanticRingWriter();
  SemanticRingWriter(SemanticRingWriter&&) noexcept;
  SemanticRingWriter& operator=(SemanticRingWriter&&) noexcept;
  SemanticRingWriter(const SemanticRingWriter&) = delete;
  SemanticRingWriter& operator=(const SemanticRingWriter&) = delete;

  [[nodiscard]] std::uint64_t instance_id() const noexcept;
  [[nodiscard]] std::uint64_t publish(const SemanticRingSnapshotView& snapshot);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

class SemanticRingReader {
 public:
  explicit SemanticRingReader(
      std::string path = "/run/depthd/semantic.ring");
  ~SemanticRingReader();
  SemanticRingReader(SemanticRingReader&&) noexcept;
  SemanticRingReader& operator=(SemanticRingReader&&) noexcept;
  SemanticRingReader(const SemanticRingReader&) = delete;
  SemanticRingReader& operator=(const SemanticRingReader&) = delete;

  [[nodiscard]] SemanticRingReadResult read_latest(
      SemanticRingSnapshot& snapshot);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] const char* semantic_ring_read_status_name(
    SemanticRingReadStatus status) noexcept;

}  // namespace depthd
