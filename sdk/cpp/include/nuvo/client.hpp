#pragma once
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nuvo::client {
inline constexpr std::size_t maximum_wire_bytes =
    32U + 65536U + 64U * 1024U * 1024U;
struct Identity {
  std::string device_uuid, run_uuid;
  std::uint64_t capture_epoch{}, capture_sequence{}, configuration_epoch{};
  bool operator==(const Identity&) const = default;
};
struct Grid {
  std::uint32_t width{}, height{};
};
struct Geometry {
  Grid grid;
  std::array<double, 12> left_projection{}, right_projection{};
  std::array<double, 16> reprojection{};
  double baseline_m{};
};
struct Product {
  std::string kind, format;
  Grid grid;
  // Read-only views remain valid while their Frame (or a copy of it) is alive.
  std::span<const std::byte> bytes;
};
struct ValidRoi {
  std::uint32_t x{}, y{}, width{}, height{};
};
struct DepthConstraints {
  double minimum_m{}, maximum_m{};  // Inclusive accepted optical-axis Z range.
};
struct DisparityConstraints {
  double accepted_min_px{}, accepted_max_px{};  // Half-open native disparity range.
  double quantization_px{};
  ValidRoi valid_roi;
};
struct Frame {
  Identity identity;
  std::uint64_t stream_sequence{};
  std::int64_t capture_time_ns{};
  std::string capture_clock, calibration_sha256;
  unsigned calibration_authority{};
  std::optional<Geometry> image_geometry, matching_geometry;
  // Absent on older recordings. Declared constraints are validated at decode;
  // adapters can use these without parsing metadata or copying pixel buffers.
  std::optional<DepthConstraints> depth_constraints;
  std::optional<DisparityConstraints> disparity_constraints;
  std::string metadata_json;
  std::vector<Product> products;
  std::shared_ptr<const void> owner;
  std::span<const std::byte> wire_bytes;
  [[nodiscard]] const Product* find(std::string_view kind) const noexcept;
};
// Without an owner, freeze a caller buffer once. With an immutable owner (e.g.
// sealed memfd), retain it and decode product views without copying pixels.
[[nodiscard]] Frame decode_frame(std::span<const std::byte> bytes,
                                 std::shared_ptr<const void> owner = {});
[[nodiscard]] float float32_le(const std::byte* data) noexcept;
struct CalibrationOverride {
  std::string sha256;
  Geometry matching;
};
[[nodiscard]] CalibrationOverride load_calibration_override(
    const std::filesystem::path& path);
void apply_calibration_override(Frame& frame,
                                const CalibrationOverride& calibration);

struct Subscription {
  std::vector<std::string> products{"depth", "validity"};
  double cadence_hz{10.0};
  std::string delivery{"best_effort"};
};
void validate(const Subscription& subscription);
class SourceInvalidated : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};
class Source {
 public:
  virtual ~Source() = default;
  // One acquisition thread. cancel() may be called concurrently and interrupts
  // blocking I/O. A cancelled Source is terminal; construct a new one to retry.
  // A next() exception also makes the source terminal; later calls return empty.
  [[nodiscard]] virtual std::optional<Frame> next() = 0;
  virtual void cancel() noexcept = 0;
  [[nodiscard]] virtual bool finished() const noexcept { return false; }
};
[[nodiscard]] std::unique_ptr<Source> local_source(
    const std::filesystem::path& socket_path = "/run/nuvo-depth/data.sock",
    Subscription subscription = {});
struct HttpsOptions {
  std::string url;
  // Optional pre-resolved numeric address (e.g. mDNS resolved by the host).
  // TLS always verifies the URL hostname, never this routing address.
  std::string connect_address;
  std::filesystem::path ca_file, credential_file;
  Subscription subscription;
  std::chrono::milliseconds timeout{5000};
};
[[nodiscard]] std::unique_ptr<Source> https_source(const HttpsOptions& options);
[[nodiscard]] std::unique_ptr<Source> replay_source(
    const std::filesystem::path& recording);
struct ClockSample {
  std::int64_t boottime_ns{}, system_ns{}, uncertainty_ns{};
};
// Same Linux kernel/time namespace only. Default Docker on the native Pi shares
// this clock; this must never be used to correlate a remote device.
[[nodiscard]] ClockSample local_clock_sample();
}  // namespace nuvo::client
