#include "depthd/capture.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

int fail(const int code, const std::string_view message) {
  std::cerr << "pairer_test: " << message << '\n';
  return code;
}

template <typename Function>
bool throws(Function&& function) {
  try {
    function();
  } catch (const std::exception&) {
    return true;
  }
  return false;
}

depthd::CameraBinding binding(const depthd::CameraRole role,
                              std::string id,
                              const std::uint32_t index) {
  return {role, {std::move(id), "imx296", "csi", index}};
}

depthd::CapturedFrame frame(const depthd::CameraRole role,
                            const std::string& id,
                            const std::uint64_t epoch,
                            const std::uint64_t sequence,
                            const std::int64_t timestamp_ns,
                            std::atomic<std::uint64_t>& releases,
                            const depthd::CaptureClock clock =
                                depthd::CaptureClock::kBoottime) {
  depthd::CapturedFrame result;
  result.role = role;
  result.camera_stable_id = id;
  result.capture_epoch = epoch;
  result.stream_sequence = sequence;
  result.sensor_timestamp_ns = timestamp_ns;
  result.sensor_clock = clock;
  result.grid = depthd::make_reference_grid(728U, 544U);
  result.pixel_format = "YUV420";
  result.planes.push_back({9, 0U, 728U * 544U, 728U});
  result.lease = depthd::CaptureBufferLease(
      epoch == 0U ? 1U : epoch, sequence == 0U ? 1U : sequence,
      [&releases] { ++releases; });
  return result;
}

}  // namespace

int main() {
  const auto left = binding(depthd::CameraRole::kLogicalLeft,
                            "platform/csi0/imx296", 1U);
  const auto right = binding(depthd::CameraRole::kLogicalRight,
                             "platform/csi1/imx296", 0U);
  std::atomic<std::uint64_t> releases{};
  std::uint64_t created = 0U;

  depthd::StereoPairer pairer({500'000U, 4U});
  const auto epoch1 = pairer.begin_epoch(left, right);
  if (epoch1 == 0U || !pairer.running() || pairer.active_epoch() != epoch1) {
    return fail(1, "first capture epoch did not start");
  }

  for (std::uint64_t sequence = 1U; sequence <= 1'000U; ++sequence) {
    const auto timestamp =
        1'000'000'000LL + static_cast<std::int64_t>(sequence) * 1'000'000LL;
    ++created;
    if (pairer.submit(frame(depthd::CameraRole::kLogicalLeft,
                            left.camera.stable_id, epoch1, sequence, timestamp,
                            releases))) {
      return fail(2, "left frame paired without a right frame");
    }
    ++created;
    auto pair = pairer.submit(frame(
        depthd::CameraRole::kLogicalRight, right.camera.stable_id, epoch1,
        sequence, timestamp + static_cast<std::int64_t>(sequence % 31U) * 100U,
        releases));
    if (!pair || pair->pair_sequence != sequence ||
        pair->capture_epoch != epoch1 || pair->sensor_delta_ns > 3'000U ||
        pair->left.camera_stable_id != left.camera.stable_id ||
        pair->right.camera_stable_id != right.camera.stable_id) {
      return fail(3, "1,000-event association changed identity or timing");
    }
    if (sequence == 1U) {
      std::array<std::uint8_t, 16> device{};
      std::array<std::uint8_t, 16> run{};
      device[0] = 1U;
      run[0] = 2U;
      const auto identity =
          depthd::make_frame_set_identity(*pair, device, run, 3U);
      if (identity.capture_epoch != epoch1 ||
          identity.capture_sequence != 1U ||
          identity.capture_clock != depthd::CaptureClock::kBoottime) {
        return fail(4, "capture-to-product identity bridge is incoherent");
      }
    }
  }

  auto stats = pairer.stats();
  if (stats.left_submitted != 1'000U || stats.right_submitted != 1'000U ||
      stats.paired != 1'000U || stats.maximum_sensor_delta_ns > 3'000U) {
    return fail(5, "1,000-event statistics are incorrect");
  }

  // Reject missing timestamps, wrong clocks, duplicate sequence numbers,
  // wrong physical identity, and frames from a previous generation.
  ++created;
  (void)pairer.submit(frame(depthd::CameraRole::kLogicalLeft,
                            left.camera.stable_id, epoch1, 1'001U, 0,
                            releases));
  ++created;
  (void)pairer.submit(frame(depthd::CameraRole::kLogicalLeft,
                            left.camera.stable_id, epoch1, 1'001U,
                            3'000'000'000LL, releases,
                            depthd::CaptureClock::kUnknown));
  ++created;
  (void)pairer.submit(frame(depthd::CameraRole::kLogicalLeft,
                            "platform/csi9/imx296", epoch1, 1'001U,
                            3'000'000'000LL, releases));
  ++created;
  (void)pairer.submit(frame(depthd::CameraRole::kLogicalLeft,
                            left.camera.stable_id, epoch1 + 1U, 1'001U,
                            3'000'000'000LL, releases));

  ++created;
  (void)pairer.submit(frame(depthd::CameraRole::kLogicalLeft,
                            left.camera.stable_id, epoch1, 1'001U,
                            3'000'000'000LL, releases));
  ++created;
  (void)pairer.submit(frame(depthd::CameraRole::kLogicalLeft,
                            left.camera.stable_id, epoch1, 1'001U,
                            3'001'000'000LL, releases));

  // A missing right event makes the old left frame stale once a later right
  // timestamp proves it cannot be its mate.
  ++created;
  (void)pairer.submit(frame(depthd::CameraRole::kLogicalRight,
                            right.camera.stable_id, epoch1, 1'001U,
                            3'002'000'000LL, releases));
  ++created;
  auto recovered = pairer.submit(frame(depthd::CameraRole::kLogicalLeft,
                                       left.camera.stable_id, epoch1, 1'002U,
                                       3'002'100'000LL, releases));
  if (!recovered || recovered->sensor_delta_ns != 100'000U) {
    return fail(6, "pairer did not recover deterministically after a missing event");
  }
  recovered.reset();

  stats = pairer.stats();
  if (stats.zero_timestamp_rejected != 1U ||
      stats.wrong_clock_rejected != 1U || stats.identity_rejected != 1U ||
      stats.epoch_rejected != 1U ||
      stats.duplicate_sequence_rejected != 1U || stats.stale_dropped != 1U) {
    return fail(7, "invalid-event classifications are incomplete");
  }

  pairer.end_epoch();
  if (pairer.running() || pairer.active_epoch() != 0U) {
    return fail(8, "capture epoch did not stop");
  }

  const auto epoch2 = pairer.begin_epoch(left, right);
  if (epoch2 <= epoch1) {
    return fail(9, "restart did not create a new capture epoch");
  }
  ++created;
  (void)pairer.submit(frame(depthd::CameraRole::kLogicalLeft,
                            left.camera.stable_id, epoch1, 1U,
                            4'000'000'000LL, releases));
  ++created;
  (void)pairer.submit(frame(depthd::CameraRole::kLogicalLeft,
                            left.camera.stable_id, epoch2, 1U,
                            4'100'000'000LL, releases));
  ++created;
  (void)pairer.submit(frame(depthd::CameraRole::kLogicalLeft,
                            left.camera.stable_id, epoch2, 2U,
                            4'000'000'000LL, releases));
  if (pairer.stats().epoch_rejected != 1U ||
      pairer.stats().nonmonotonic_rejected != 1U) {
    return fail(10, "old-generation frame was accepted after restart");
  }
  pairer.end_epoch();

  pairer.configure({100'000U, 1U});
  const auto tolerance_epoch = pairer.begin_epoch(left, right);
  ++created;
  (void)pairer.submit(frame(depthd::CameraRole::kLogicalLeft,
                            left.camera.stable_id, tolerance_epoch, 1U,
                            4'150'000'000LL, releases));
  ++created;
  if (pairer.submit(frame(depthd::CameraRole::kLogicalRight,
                          right.camera.stable_id, tolerance_epoch, 1U,
                          4'150'200'000LL, releases))) {
    return fail(12, "configured 100 us tolerance accepted a 200 us pair");
  }
  if (!throws([&] { pairer.configure({500'000U, 4U}); })) {
    return fail(13, "pairer allowed reconfiguration during an epoch");
  }
  pairer.end_epoch();
  pairer.configure({500'000U, 4U});

  const auto changed_right = binding(depthd::CameraRole::kLogicalRight,
                                     "platform/csi2/imx296", 2U);
  if (!throws([&] { (void)pairer.begin_epoch(left, changed_right); })) {
    return fail(14, "physical right-camera rebinding was accepted silently");
  }

  const auto capacity_epoch = pairer.begin_epoch(left, right);
  std::uint64_t previous_epoch = capacity_epoch;
  for (std::uint64_t sequence = 1U; sequence <= 5U; ++sequence) {
    ++created;
    (void)pairer.submit(frame(depthd::CameraRole::kLogicalLeft,
                              left.camera.stable_id, capacity_epoch, sequence,
                              4'200'000'000LL +
                                  static_cast<std::int64_t>(sequence),
                              releases));
  }
  if (pairer.stats().capacity_dropped != 1U) {
    return fail(15, "per-camera pending queue did not enforce its bound");
  }
  pairer.end_epoch();

  // Repeated clean lifecycle transitions remain possible on one object.
  for (int cycle = 0; cycle < 50; ++cycle) {
    const auto epoch = pairer.begin_epoch(left, right);
    if (epoch <= previous_epoch) {
      return fail(16, "capture epoch is not monotonic across repeated starts");
    }
    previous_epoch = epoch;
    ++created;
    (void)pairer.submit(frame(depthd::CameraRole::kLogicalLeft,
                              left.camera.stable_id, epoch, 1U,
                              5'000'000'000LL + cycle, releases));
    pairer.end_epoch();
  }

  if (releases.load() != created) {
    return fail(17, "a simulated camera request lease leaked");
  }
  return 0;
}
