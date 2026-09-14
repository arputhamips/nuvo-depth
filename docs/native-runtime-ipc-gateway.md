# Native runtime, IPC v2, and shared IP gateway

This document records the implementation boundary after Steps 4–6. It is a
status document, not a claim that hardware or release qualification is done.

## Implemented skeleton

The native runtime accepts one already-associated stereo capture while both
camera leases and CPU mapping owners are retained. One worker owns the
rectifier and stereo matcher. It produces an immutable, owning frame set with:

- rectified logical-left and logical-right images on the image grid;
- disparity, confidence, metric optical-axis depth, and validity on the native
  matcher grid;
- one device/run/capture/configuration identity and one calibration hash and
  authority for the complete set; and
- backend names and bounded processing statistics.

Input and completed queues are bounded. Backpressure rejects capture work or
drops the oldest *complete* result. Individual products are never combined
across captures. Provisional calibration does not authorize metric depth unless
an explicit engineering-only override is supplied.

The Linux capture bridge maps each YUV420 luma DMA-BUF with explicit
`DMA_BUF_SYNC` read bracketing. Mapping owners are destroyed before the paired
camera leases, including on processing failure and shutdown.

IPC v2 uses fixed-count slots with separate storage for every product. The
default untrusted subscription copies only requested products. Trusted
zero-copy subscriptions are separately capped and return immutable read leases.
A slot cannot be reused until every genuine lease is released; disconnect and
timeouts do not revoke mappings. A new broker requires a new run UUID.

On Linux, `/run/nuvo-depth/control.sock` and
`/run/nuvo-depth/data.sock` are the reserved endpoints. The transport support
uses Unix `SOCK_SEQPACKET`, `SO_PEERCRED`, `SCM_RIGHTS`, close-on-exec handles,
bounded packets, and inode-safe socket cleanup. Shared memory is created with a
producer mapping before `F_SEAL_FUTURE_WRITE`; clients receive a separate
read-only descriptor.

NuvoStream uses one common protocol for physical Ethernet and USB Ethernet:

- HTTPS/UI/control target: TCP 7443;
- authenticated HTTPS snapshot and WebSocket stream target: TLS/TCP 7443;
- discovery target: `_nuvo-depth._tcp` mDNS;
- 32-byte, network-byte-order `NUV1` header;
- at most 64 KiB metadata and 64 MiB product payload, further reduced by
  subscription negotiation; and
- Protobuf metadata schema in `protocol/nuvo_stream_v1.proto`.

The gateway admission core reserves the exact framed byte rate against the
qualified capacity of Ethernet or USB Ethernet. Preference can be Ethernet,
USB, both, or off. It never silently removes products or reduces cadence. Each
client has at most one unsent complete frame and one in-flight frame. Once any
TCP bytes have been sent, that frame must finish or the session closes.

The first C++ SDK surface is `nuvo::StreamDecoder`. It is an incremental,
bounded decoder for bytes after TLS authentication/decryption and handles both
fragmented and coalesced TCP reads.

## Installed candidate and remaining gates

`nuvo-depthd-native.service`, the sealed local frame service, TLS gateway,
claim/session flow, REST/WebSocket API, web console, mDNS record, C ABI, Python
SDK, bounded NVR1 recorder/replay, calibration activation, transport rollback
watchdog, and Debian package split are implemented. Package installation does
not enable the target. The original Python service and IPC v1 remain as a
rollback comparator until live parity is measured.

The September 9, 2026 rig pass now provides live Pi DMA-BUF coherency,
3 x 1,000-pair restart/capture evidence, 1,000 zero-gap native publications,
short service/thermal observations, authenticated SDK/API checks, and native
ARM64 package installation evidence. The remaining work is:

- long thermal, memory/descriptor, and endurance measurements;
- per-unit calibration capture and independent metric-depth validation;
- trigger waveform/electrical review and powered USB-C qualification;
- measured Ethernet and USB Ethernet capacities and host compatibility;
- clean arm64 install/upgrade/downgrade, power-cut recovery, and long soak;
- authorized USB VID/PID, frozen OS/kernel/libcamera/IPA/Mesa versions, project
  and third-party licensing, real maintainer identity, and release signing keys;
  and
- signed APT publication after every qualification gate passes.

NVR1 intentionally records complete NuvoStream frame sets without another
runtime dependency. MCAP export remains a host-tool extension point rather than
an undocumented compatibility claim.

## Current acceptance evidence

Portable tests cover pipeline identity and lease release, real OpenCV
rectification/SGBM integration, bounded scheduler behavior, selective IPC
products, pinned readers and disconnect, pool limits, run generations, strict
wire parsing and fuzzed headers, exact transport admission, and whole-frame
network backpressure. Linux tests cover peer credentials, descriptor transfer,
read-only sealed memory, and safe socket lifecycle.

Current hardware evidence and the still-pending manual gates are recorded in
`docs/pi5-hil-qualification-20260909.md`.
