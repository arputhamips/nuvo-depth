# Nuvo C++ acquisition client

This ROS-independent C++20 library acquires associated NuvoStream frames from
the native Linux data socket, verified HTTPS/WebSocket, or NVR1 recordings.
It builds without OpenCV, libcamera, FFmpeg, ROS or appliance installation.
The stable C decoder ABI remains unchanged.

```bash
sudo apt install cmake ninja-build g++ nlohmann-json3-dev libboost-system-dev libssl-dev libc-ares-dev
cmake -S sdk/cpp -B build/cpp-client -G Ninja -DNUVO_CLIENT_BUILD_TESTS=ON
cmake --build build/cpp-client -j2
ctest --test-dir build/cpp-client --output-on-failure
cmake --install build/cpp-client --prefix /your/prefix
```

Use `-DNUVO_CLIENT_WITH_HTTPS=OFF` to omit Boost/OpenSSL/c-ares. This option does not
change native daemon/minimal-package dependencies. The existing camera build
and installers do not build or install this optional SDK automatically.
Downstream consumers use `find_package(nuvo_depth_client CONFIG REQUIRED)` and
link `nuvo::client`. Installed public headers have no daemon-private dependencies.

```cpp
#include <nuvo/client.hpp>
#include <atomic>
auto source = nuvo::client::local_source(); // depth + validity, best effort 10 Hz
std::atomic_bool stop_requested{false};
while (!stop_requested) {
  auto frame = source->next();
  if (!frame) {
    if (source->finished()) break; // replay EOF
    continue;                     // ordinary bounded local wait
  }
  const auto *depth = frame->find("depth");
  // Keep frame alive while accessing depth->bytes. Use float32_le() for
  // unaligned little-endian samples; never cast the payload to float*.
}
// A control thread stops acquisition by setting stop_requested = true,
// then calling source->cancel() to interrupt the current wait.
```

`next()` returns no frame on a bounded IPC wait. Check `finished()` for replay
EOF. Transport errors and malformed input throw and make the source terminal.
`cancel()` is concurrent-safe, interrupts reads, and makes that source terminal.
Retry by constructing a new
source; do not retry a half-read message. Local mappings are read-only sealed
memfds; frame ownership survives source destruction. Network/replay buffers are
bounded owning allocations. Replay reads a finite snapshot of the regular file
at construction and rejects record lengths beyond its remaining bytes before
allocating. The SDK itself has no background queue or reconnect thread. The
caller owns its recovery, rate policy and publication queue.

`https_source()` uses a verified CA/hostname and a restricted 0600 token file.
It exchanges the existing admin token for a session, checks product capabilities,
and opens one persistent binary WebSocket. It performs no device configuration
writes. Cleanup attempts bounded session revocation; a broken connection can
prevent confirmation, and older gateways may lack DELETE session support.
Timeouts bound complete operations; low cadence needs a longer timeout. DNS uses
an interruptible c-ares socket loop with no resolver thread surviving source
destruction. It supports DNS and hosts-file names. For host-resolved mDNS, set
`HttpsOptions.connect_address` to the resolved numeric address; URL hostname
verification remains required and unchanged.

Frames retain exact device/run/capture/configuration identity and source boot
time. Depth is optical-axis Z in metres, validity code 1 means accepted, and
confidence is binary matcher support. No function changes calibration authority.
Decode rejects unknown validity codes, non-NaN invalid depth, infinite samples,
producer stereo configuration rejection and accepted measurements that contradict
declared depth/disparity ranges or valid ROI. When both products and matching
geometry are available, it verifies optical-axis Z against native disparity,
including the principal-point offset. A depth-only subscription uses the depth
and validity products directly; it does not request images or disparity.
`Frame::depth_constraints` and `Frame::disparity_constraints` expose validated,
typed declarations for adapters. They remain absent on old recordings that did
not declare them; the SDK cannot verify undeclared limits or missing geometry.
Image-only products have no measurement metadata prerequisite. Validation parses
metadata once and scans selected matching samples once without copying pixels.
Geometry is optional for old recordings/devices. Public beta 8 lacks the newer
geometry fields: geometry-dependent consumers must reject it or deliberately
apply a matching calibration hash/grid override. Override JSON contains
`calibration_sha256`, `matching_grid` (`width`,`height`) and `matching`
(`left_projection`, `right_projection`, `reprojection`, `baseline_m`). Never use
the shared seed as a silent per-unit override.

`local_clock_sample()` correlates CLOCK_BOOTTIME with system time only for a
native daemon and client sharing the native boottime namespace. Explicit Docker
time offsets are rejected. It does not synchronize a remote device. Network
consumers need a separate time-correlation capability, or explicitly labeled
receipt timestamps. Preserve source time in either case.

The canonical wire implementation remains `src/wire_protocol.cpp` and
`include/depthd/wire_protocol.hpp`. `tools/export_cpp_client.py DEST` packages
that private build dependency together with this subtree into a standalone
snapshot, with LF-normalized text hashes, base commit, license and notices. Only
`nuvo/client.hpp` is installed. The ROS vendor snapshot is generated from this
closure; edits belong here and are exported again.
