# Native runtime, IPC v2, and shared IP gateway

This document records the implementation boundary after Steps 4–6. It is a
status document, not a claim that hardware or release qualification is done.

## Implemented skeleton

The native runtime accepts one already-associated stereo capture while both
camera leases and CPU mapping owners are retained. Independent image and
matcher workers each own their stateful backends and bounded queues. A slow
match cannot delay the image worker. Each produces an immutable frame set with:

- rectified logical-left and logical-right images on the image grid;
- disparity, confidence, metric optical-axis depth, and validity on the native
  matcher grid;
- one device/run/capture/configuration identity and one calibration hash and
  authority for the complete set; and
- backend names and bounded processing statistics.

The live runtime divides a checked 256 MiB payload allowance between its workers.
The normal 728 × 544 configuration uses four slots per worker; larger geometry
must fit the combined budget with at least two slots per worker. Factory
construction rejects smaller pools, so retaining the latest publication cannot
prevent its replacement. Pools never expand at runtime. The image worker
reserves no matching products or matching scratch and tracks stereo metadata
without constructing a matcher or changing the process-wide OpenCV thread count.

Explicit shared capture leases release each camera request once, after both
workers have dropped their mappings. A matcher result retains requested
capture-associated images even when independent mono frames have already
advanced. Late results cannot replace the newest image or restore an older
configuration. A bounded shared revision history gives both workers the same
configuration epoch, including when one skips intermediate control writes.
The request carries separate independent and associated image masks. An image
requested only by the independent group is produced once; it is retained in the
matcher publication only when a matcher subscriber explicitly requests it there.

Input and completed queues are bounded per worker. Backpressure rejects capture work or
drops the oldest *complete* result. Individual products are never combined
across captures. Provisional calibration does not authorize metric depth unless
an explicit engineering-only override is supplied.

The Linux capture bridge maps each YUV420 luma DMA-BUF with explicit
`DMA_BUF_SYNC` read bracketing. Mapping owners are destroyed before the paired
camera leases, including on processing failure and shutdown.

CPU rectification stages DMA inputs before OpenCV remapping; already-owned CPU
images are remapped directly. Copy time, remap time and copied bytes are measured
separately. Immutable fixed-point maps are shared across CPU workers while
staging buffers remain private. Input and output image spans must not overlap,
including partial overlap with different starting pointers; the CPU backend
rejects aliasing before remapping.

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

The gateway holds separate persistent image and matcher subscriptions to
`/run/nuvo-depth/data.sock`, each using its consumers' union of products and
maximum cadence. A depth subscription keeps all its associated images together;
independent image subscriptions do not raise its match rate. Each reader fetches
nothing while its group has no consumer.
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
Serialization caches are keyed by the immutable publication and selected product
mask. New image publications preserve cached messages for the retained matcher
frame. Weak frame ownership prevents this cache from pinning processing slots;
both the entry count and serialized-byte total remain bounded.

NuvoStream uses one common protocol for Ethernet, USB Ethernet, and WiFi:

- HTTPS/UI/control target: TCP 7443;
- authenticated HTTPS snapshot and WebSocket stream target: TLS/TCP 7443;
- discovery target: `_nuvo-depth._tcp` mDNS;
- 32-byte, network-byte-order `NUV1` header;
- at most 64 KiB metadata and 64 MiB product payload, further reduced by
  subscription negotiation; and
- bounded JSON metadata decoded by `sdk/python/nuvo_depth/frame.py` and the C
  SDK. `protocol/nuvo_stream_v1.proto` is an unused reference schema, not the
  shipping serialization or a build dependency.

Additive schema-1 metadata includes `image_grid` and `matching_grid`, including
their scale/offset to the reference image; `disparity_metadata` specifies its
valid ROI, matching-grid pixel units, 1/16-pixel quantization, accepted interval,
derivation and binary confidence model. `depth_metadata` declares its limits
and NaN invalid representation. Native publications also include
`calibration_geometry.image` and `.matching`, each with row-major 3×4 left/right
projection matrices, a row-major 4×4 `reprojection` matrix and `baseline_m`.
These are the active calibrated views, not a replacement for their hash/authority.
Older producers may omit this optional geometry; consumers must not invent it.
The optional `capture` object carries each camera's sensor timestamp, sequence,
reported exposure and gains plus stereo skew. Reported sensor exposure is not
an electrical measurement of the external XTR pulse. Missing (zero), nonpositive
or nonfinite exposure/gain values serialize as JSON null. The existing capture timestamp remains the pair's
boot-time midpoint.

The gateway admission core reserves the exact framed byte rate against the
configured capacity of the actual accepted Ethernet, USB Ethernet, or WiFi
interface. Preferences are `auto`, `wifi`, `ethernet`, `usb`, `both` (wired and
USB), or `off`. The optional `wifi_enabled` boolean independently permits Wi-Fi
products while retaining the wired preference; `off` remains a master stop.
Older documents without the flag retain their legacy behavior: `auto`/`wifi`
permit Wi-Fi and the other preferences do not. `auto` supports detected
connections without moving them to another link; `wifi` leaves wired delivery
disabled. The policy never silently removes products or reduces cadence. Each
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
either wired interface; `off` disables data. Disabling Wi-Fi closes/refuses
Wi-Fi numeric and compressed-picture streams without interrupting permitted
wired streams or authenticated console/control access. Preference changes
revalidate affected contracts; clients reconnect where their contract requires
it. A parsed policy cache keeps repeated file reads out of the frame path.

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
The encoded validator accepts the encoder's canonical hyphenated UUIDs and the
compact hex spelling. Both retain strict nonzero identity, header, scalar type,
keyframe and capture-freshness checks. Image-only completions cannot consume a
numeric preview plan before its matching depth/disparity completion arrives.

The first C++ SDK surface is `nuvo::StreamDecoder`. It is an incremental,
bounded decoder for bytes after TLS authentication/decryption and handles both
fragmented and coalesced TCP reads.

## Browser access and SDK compatibility

The HTTPS console opens directly. Anyone with network access to it can obtain
an administrative browser session; there is no browser password or setup-code
gate. `GET /api/v1/session` reuses a valid cookie or creates one within the
existing session cap and twelve-hour expiry. It returns a CSRF token and a
Secure/HttpOnly/SameSite=Strict cookie. Origin mismatches and foreign browser
Fetch Metadata are rejected. The remaining API and WebSocket routes still
require a session or compatible administrative bearer token; mutations also
require CSRF. The console refreshes an expired session automatically and retries
a rejected request once, retaining configuration edit-base revisions.

The existing token/password/code `POST /api/v1/session` interface and explicit
legacy owner APIs remain compatible. Automatic browser access does not consume
pairing material or rewrite ownership records. Fresh installation no longer
asks for a Nuvo owner password or issues an enrollment code. OS/SSH credentials,
protected Wi-Fi provisioning, certificate identity, calibration and recovery
requirements are unchanged.

## Installed candidate and remaining gates

`nuvo-depthd-native.service`, the sealed local frame service, TLS gateway,
automatic browser sessions, compatible SDK token claims, REST/WebSocket API, web console, mDNS record, C ABI, Python
SDK, bounded NVR1 recorder/replay, calibration activation, transport rollback
watchdog, and Debian package split are implemented. The installer applies the
selected startup policy. The parallel Python service and its private IPC/output
stack have been retired; actual asset/calibration/transport rollback remains.

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

Raw demand has two bounded processing groups: requests containing any matcher
product retain their entire associated product mask at that group's maximum
cadence; image-only requests aggregate separately. Thus 1 Hz depth plus 10 Hz
mono requires matching at 1 Hz. This is group scheduling, not independent
per-product scheduling. Each group retains its latest coherent frame; newer
mono publications cannot erase a pending depth result.

Encoded previews use event-driven waits and immutable sealed memfds on the
existing `data.sock` (`NVY2`, with `NVI2` invalidation). Only the latest preview
is retained, bounded to 4 MiB. Normal native delivery no longer writes or polls
`snapshots/latest.nve`. `--diagnostic-raw-snapshots` also enables this encoded
diagnostic file; gateway `--diagnostic-snapshot-fallback` can still read files
from older producers. External HTTP/WebSocket endpoints and NVE1 bytes are
unchanged. The resource profiler observes preview IPC directly.
The gateway validates each NVE1 header, bounded scalar metadata, capture
identity, codec/MIME pairing, and keyframe agreement before caching it. Only
verifiable `CLOCK_BOOTTIME` captures at most one second old are eligible;
future, stale, or invalid cached packets do not become fresh on reconnect.
Skipping them preserves the advanced local cursor and registered demand so a
later fresh capture can satisfy the original HTTP/WebSocket first-frame wait.

The gateway maps validated, size-sealed local messages read-only. Product
selection rewrites only header/metadata and sends owning payload views over
WebSocket; explicit vector APIs and HTTP snapshots still copy their complete
response. Session admission reserves the full retained source owner for both
pending and in-flight frames, including when a small subset pins a much larger
product union. This conservative bound can admit fewer heterogeneous slow
clients than compact copied subsets; the 64 MiB session budget is unchanged.
Routine HTTP responses can reuse a TLS connection for at most 32 requests and
30 seconds, with the existing five-second socket deadline. At most four idle
HTTP connections (and at most one quarter of the connection limit) retain a
keep-alive slot, leaving capacity for streaming. Hidden consoles skip routine
status/settings polling and refresh when visible again.

Status retains total `published_frames` and adds `matched_frames` plus the
latest `matcher_statistics`. Depth rates and stereo timing history use these
dedicated measurements, so faster image-only publications cannot inflate the
reported matcher cadence or replace its timing samples.

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

## Source map

For specific changes, start at these functions and their regression tests:

| Behavior | Implementation entry point | Regression coverage |
| --- | --- | --- |
| Camera negotiation, completion metadata and recycling | `Pi5LibcameraCapture::configure_camera`, `request_complete`, `start_request_recycler` in `platform/pi5/capture_libcamera.cpp`; negotiation checks in `platform/pi5/camera_identity.cpp` | `camera_identity_test.cpp`, `capture_lease_test.cpp` |
| Trigger generation and physical camera binding | `platform/pi5/pio_pwm_trigger.cpp`, `bind_pi5_fixed_csi_ports` in `platform/pi5/camera_identity.cpp` | `pio_pwm_trigger_test.cpp`, `camera_identity_test.cpp`; physical edges need Pi measurements |
| Timestamp pairing and observed sensor sequence gaps | `StereoPairer::submit` in `src/core/pairer.cpp` | `pairer_test.cpp` |
| DMA mapping, cache ownership and CPU fallback | `platform/linux/capture_mapping.cpp` | `capture_mapping_cache_test.cpp`, `native_capture_runner_test.cpp` |
| Exposure metering, stable hold and applied-control acknowledgement | `Controller::observe`, `update_applied_controls`, `measure` in `src/exposure_control.cpp`; every-pair wiring in `apps/depthd_main.cpp` | `exposure_control_test.cpp` |
| Image/matcher scheduling and pool accounting | `BoundedNativeScheduler::try_submit`, `NativeProcessingPipeline::process`, `required_frame_pool_payload_bytes` in `src/core/runtime.cpp` | `runtime_test.cpp`, `runtime_factory_test.cpp` |
| CPU rectification and prepared map sharing | `OpenCvRectificationBackend::rectify`, `prepare_opencv_rectification_maps` in `src/opencv_rectification_backend.cpp` | `opencv_rectification_backend_test.cpp`, `runtime_opencv_integration_test.cpp` |
| OpenCV SGBM 3-way/BM and Nuvo postprocessing | `src/cpu_sgbm_backend.cpp`; factories selected by `src/core/runtime_factory.cpp` | `cpu_sgbm_test.cpp`; `sgbm_benchmark` is explicitly the legacy full-image API |
| Validity and metric depth | `src/cpu_sgbm_backend.cpp`, `src/products.cpp`, `src/depth_conversion.cpp` | `disparity_validity_test.cpp`, `depth_conversion_test.cpp` |
| Preview copy admission, conversion and encoding | `AsyncFrameEncoder::Impl::submit_capture`, `render_preview`, `run`, and `FfmpegFrameEncoder` in `src/image_encoder.cpp` | `image_encoder_test.cpp` |
| Raw serialization, sealed publication and cache reuse | `src/frame_wire.cpp`, `platform/linux/local_frame_server.cpp` | `frame_wire_test.cpp`, `local_frame_server_test.cpp`, `frame_wire_sdk_roundtrip_test.py` |
| Serving assets, APIs, TLS and streams | `apps/nuvo_gateway_main.cpp`, `src/gateway.cpp` | `gateway_integration_test.py`, `gateway_browser_test.cjs` |
| Output selection, preview rendering and reconnects | `web/device-settings.js`, `web/app.js`, `web/frame-codec.js` | `web_output_selection_test.js`, `web_frame_codec_test.js`, `web_stream_lifecycle_test.js` |
| SDK transport, immutable decoded frames and recording | `sdk/python/nuvo_depth/client.py`, `frame.py`, `encoded.py`, `recording.py` | `python_sdk_test.py`, `sdk_tls_test.py`, `sdk_examples_test.py` |

Test filenames above are under `tests/`. Calibration authority and configuration
transactions are also essential: inspect `src/calibration_store.cpp`,
`src/config.cpp`, and `tools/nuvo_system_agent.py` before changing geometry,
controls or publication epochs.
