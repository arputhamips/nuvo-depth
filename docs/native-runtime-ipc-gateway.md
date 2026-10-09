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

The production daemon publishes complete frame pointers to the local frame
service. It does not also copy them into an unused in-process broker or write
raw snapshots each frame. `depthd --snapshot-dir DIR` explicitly enables raw
disk diagnostics. The broker API remains available for library consumers.

The gateway holds one persistent raw subscription to
`/run/nuvo-depth/data.sock` for the union of active consumers' selected products
and their maximum cadence. It fetches nothing while no raw consumer is open.
A separate persistent subscription carries maximum encoded-preview demand;
preview encoding stops after its last consumer disconnects. Snapshot requests
also acquire demand, wait at most three seconds for matching fresh products,
then hold the selected immutable publication and reservation through the write.
A failed wait returns 503. Native `boottime` captures older than one second are
rejected before a retrieved cached memfd can satisfy that wait. First requests never depend on an eager all-product
background reader. Persistent `NVS2` subscription updates acknowledge with
`NVA2`; raw `NVW2` waits remain bounded to 500 ms. Disconnect releases demand.

All 14 product kinds are independently selectable: the legacy rectified mono
pair, disparity/depth/confidence/validity; unrectified `left_mono`/`right_mono`,
`left_rgb`/`right_rgb`; `left_rectified_rgb`/`right_rectified_rgb`; and
`rectified_stereo`/`rectified_stereo_rgb` side-by-side pairs. Stereo pair images
have twice the single-camera width. Explicit RGB products carry `rgb8` data;
legacy descriptors retain their actual `gray8`, `rgb8`, or `bgr8` format. Image
consumers do not request the depth matcher. Native processing follows aggregate
raw and encoded demands, so an image preview can continue while depth matching
is busy.

At most eight local clients are served concurrently. Local serialization caches
four masks within 64 MiB, and the gateway shares four variants within 16 MiB per
source generation. `selected_products` is rewritten for each filtered response;
`available_products` retains the source union. Publication identities and whole
RGB payloads are preserved. Shutdown wakes waiters and closes tracked network
connections. `--snapshot-file` remains an explicit diagnostic fallback.

NuvoStream uses one common protocol for Ethernet, USB Ethernet, and WiFi:

- HTTPS/UI/control target: TCP 7443;
- authenticated HTTPS snapshot and WebSocket stream target: TLS/TCP 7443;
- discovery target: `_nuvo-depth._tcp` mDNS;
- 32-byte, network-byte-order `NUV1` header;
- at most 64 KiB metadata and 64 MiB product payload, further reduced by
  subscription negotiation; and
- Protobuf metadata schema in `protocol/nuvo_stream_v1.proto`.

The gateway admission core reserves the exact framed byte rate against the
configured capacity of the actual accepted Ethernet, USB Ethernet, or WiFi
interface. Preferences are `auto`, `wifi`, `ethernet`, `usb`, `both` (wired and
USB), or `off`. `auto` supports every detected connection without reconnecting
to another link; `wifi` restricts data to WiFi. It never silently removes products or reduces cadence. Each
client has at most one unsent complete frame and one in-flight frame. Once any
TCP bytes have been sent, that frame must finish or the session closes.

The executable now uses that admission core and `SessionQueue`. Raw exact
subscriptions use `/api/v1/stream?cadence_hz=10&products=depth,confidence,validity`.
Both raw parameters are required together; duplicate/unknown products, invalid cadence,
and unknown parameters return 400. Before accepting the upgrade, the gateway
acquires the requested source demand, waits boundedly for matching products,
then admits their selected payload plus the
64 KiB metadata ceiling. Each accepted socket is matched to its actual local
network interface. A connection on USB cannot reserve Ethernet capacity. When
the preferred qualified interface can admit a request arriving on the other
interface, the request is rejected with a reconnect alternative. `both` permits
either wired interface; `off` disables data. Changing transport policy
closes existing data streams so their contracts must be negotiated again.

Exact source and link evidence is read at startup from
`/etc/nuvo-depth/gateway-qualification.json`, or `--qualification-file PATH`.
The installed `gateway-qualification.example.json` is disabled and contains
zero bounds. Enable it only after filling its retained measurement report ID,
source lower cadence bound, maximum frame size, applied stereo file SHA256,
calibration SHA256, configuration epoch, and each measured interface's framed
payload rate, maximum frame size, and session count. Frames must have validated
calibration authority, equal requested/applied stereo revisions, and no stereo
reload error. Requested cadence above the measured source bound, missing
qualification, or exhausted connected-interface capacity returns 409. No
nominal network speed or preset name is interpreted as measured capacity.
Existing qualified streams close when the profile/calibration identity or
qualification document changes. Restart the gateway to load new evidence.

WiFi is supported without an engineering override and always reports
`X-Nuvo-Stream-Qualification: best-effort`. The optional `delivery` parameter is
`auto`, `best_effort`, or `exact`. Explicit `auto` selects qualified exact only
when current evidence covers the source, requested cadence and wired link;
otherwise its handshake explicitly says best effort. `best_effort` applies the
requested cadence as a maximum on any supported link. `exact` rejects WiFi and
unqualified sources. Legacy paired products/cadence requests without `delivery`
retain their exact-on-wired requirement. Plain legacy streams remain best
effort at a 10 Hz maximum. No path silently changes the chosen products or
formats. Encoded stream/snapshot requests accept `cadence_hz` as a preview
maximum and never claim an exact raw-product contract.

`/api/v1/capabilities` includes the accepted connection's interface, detected
kind, whether wired link capacity has current measured evidence, resource byte
budget, preview recommendation, and active raw/encoded consumer counts. WiFi
has an explicit default 8 MiB/s resource budget; `--wifi-byte-budget BYTES`
allows a deployment to configure a measured or desired safe best-effort limit.
`--engineering-byte-budget` sets the unqualified wired resource limit (default
64 MiB/s). These limits are not substituted for exact qualification. All active
sessions reserve framed rates against the same link controller; an additional
shared token budget bounds actual variable-size output. Best-effort overload
can skip whole frames; qualified streams fail closed if a bound is violated.

`--engineering-interface lo` remains available for temporary local testing and
reports `engineering-best-effort`. It does not change production network
configuration. Handshakes also report requested cadence and `exact` or
`maximum-best-effort` cadence semantics; the SDK exposes `stream.qualification`.

The Outputs page defaults to unrectified left mono at 5 Hz, allows all 14 raw
product choices and independent preview cadence, and shows a conservative
byte-rate estimate beside the connection's budget. Users may select image-only,
matcher-only, or mixed outputs and choose delivery semantics explicitly.
Streaming configures preview codec/source/size/quality; merely saving an
encoder configuration does not create a consumer. Browser navigation away from
preview pages, hiding the tab, disabling output, and disconnecting release
browser demand. Selections are saved locally for that browser; other consumers
retain their own masks and rates.

Gateway source storage is limited to 16 MiB per raw serialized frame (checked
before allocating the memfd copy) and 4 MiB per encoded access unit. Product
subsets are shared per source generation and mask, with four cached masks and
16 MiB aggregate storage. Each session reserves two worst-case serialized
frames against a shared 64 MiB memory budget, including encoded sessions.
Snapshots without an explicit cadence hint register 1 Hz demand and rate
reservation, while first-frame processing can start immediately. Their actual
full byte size still charges the shared token budget, so rapid repeat callers
cannot bypass output limits. Explicit product/cadence queries retain their
requested contract.
HTTP raw and encoded snapshots reserve from the same budget before copying
their response bodies and hold that reservation through the write. Each
response pins the exact admitted publication; a newer, larger frame cannot
replace it between admission and serialization. Exhausted requests return 409,
and a failed partial write closes the connection.
Oversize or changed-format frames close the session. Event notifications are
coalesced; slow clients retain one complete pending frame and one in-flight
frame. WebSocket reads stay active for ping/pong and close frames. Writes must
complete within five seconds; shutdown interrupts tracked connections and has
a five-second client deadline. The service sets a ten-second stop timeout.
Removing or reconfiguring the native encoded output clears the gateway cache.

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

Configuration endpoints (`/api/v1/stereo`, `/api/v1/exposure`, and
`/api/v1/encoding`) return requested JSON with a strong SHA256 `ETag` of those
exact bytes. POST requires that ETag in `If-Match`: missing preconditions return
428 and changed revisions return 412. `nuvoctl` checks the revision and replaces
the file while holding the shared privileged configuration lock; the gateway's
precheck alone is not the concurrency protection. Successful writes return 202,
the exact saved JSON and its ETag, and `X-Nuvo-Configuration-State: pending`.
GET reports `requested`; no response implies that the camera or matcher applied
commands merely because the file was saved.

Stereo reloads hash and parse the same bounded document. Frames and status
carry `stereo_requested_revision`, `stereo_applied_revision`,
`stereo_configuration_generation`, and `stereo_configuration_error`. A valid
semantic change advances the processing `configuration_epoch` at the next
processing boundary. Formatting-only changes acknowledge a new file revision
without rebuilding or advancing the epoch. Rejected controls leave the prior
matcher and applied revision intact and expose the rejection error. Per-camera
exposure metadata describes actual settings separately from requested commands.
