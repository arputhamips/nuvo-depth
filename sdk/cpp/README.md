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

## Explicit native calibration activation

Acquisition never selects calibration. Call `activate_local_calibration(bundle,
control_socket)` only when explicitly requested, before opening `local_source()`.
It uploads a complete accepted per-unit native bundle to the root broker at
`/run/nuvo-depth/calibration.sock`. The native controller validates trusted factory
signatures when applicable, unit/camera/profile bindings and matching geometry,
then uses its durable import/restart/rollback transaction and verifies two fresh
advancing depth/validity frames. The return value identifies the verified native
calibration hash, authority and run. This globally selects calibration for native
capture and may restart every connected consumer. It preserves the bundle's
canonical identity and authority; it does not solve or recalibrate a camera.

The input is an extracted directory or an **uncompressed tar** with exactly
`manifest.json`, `front_left.yaml`, `front_right.yaml` and `validation.json` as
regular files at its root. Each asset is limited to 4 MiB; links, extra entries,
sparse files, duplicate names and archive paths are rejected. For example:

```bash
tar -C /path/to/accepted-bundle -cf calibration.tar \
  manifest.json front_left.yaml front_right.yaml validation.json
```

A standard single-camera ROS YAML or a geometry override JSON is insufficient:
it lacks the native bundle's declared validation and identity contract. The
SDK's read-only frame geometry override remains a separate decoding operation.

The broker's socket is root:nuvo-video, mode 0660. Membership grants permission
to request this global operation, including user-validated bundles when no
factory calibration has precedence. Mount the existing runtime directory
read-only and add its numeric `nuvo-video` GID to a same-Pi Linux container.
Mount the explicit input file or directory read-only separately. The SDK sends
bytes; the container needs no host calibration paths, sudo or systemd access.
No request occurs unless the caller invokes this API. Same-bundle retries check
the active native selection, loaded hash/authority and fresh frames under the
transaction lock; a healthy identical selection avoids a restart.

The complete socket operation has a default five-minute timeout, bounded to ten
minutes maximum. A timeout or lost reply is a failure to confirm completion;
the already authorized native transaction may still finish. Inspect
`nuvoctl calibration status` on the host and retry the same bundle. A busy
transaction returns a retryable error after a bounded wait. The broker bounds
uploads to ten seconds and processes one transaction at a time with four kernel
backlog slots. An omitted calibration option preserves the existing selection.

This API requires the same-host Linux control socket. HTTPS activation is
unsupported; import the bundle on the appliance using
`sudo nuvoctl calibration import /path/to/extracted-bundle`, then connect without
requesting calibration activation. Host/container tests do not qualify Pi hardware
or establish metric accuracy.

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
