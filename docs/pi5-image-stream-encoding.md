# Pi 5 image-stream encoding

## Decision

Nuvo Depth has separate bounded branches for requested numeric products and
monitoring images. Active subscriptions determine their product masks and
cadence. Mono/RGB camera previews, including rectified side-by-side images,
rectify/encode independently from stereo matching. Disparity/depth previews
use completed numeric products. With no subscribers, neither branch processes
products; paired capture remains available for fresh health and eligible
exposure metering.

Raspberry Pi 5 has no H.264 hardware encoder. The production implementation
therefore uses FFmpeg's NEON-optimized `libx264` encoder for low-latency H.264
and FFmpeg MJPEG for independently decodable image frames. Raspberry Pi's
current camera documentation recommends software `x264` on Pi 5 and describes
both H.264 and MJPEG network output. The Pi 5 H.264 performance whitepaper
reports real-time 1080p30 at low CPU cost and evaluates higher-quality and
1080p60 cases. These are the primary platform references:

- [Raspberry Pi camera software documentation](https://www.raspberrypi.com/documentation/computers/camera_software.html)
- [H.264 encoding performance on Raspberry Pi 5-series computers](https://pip-assets.raspberrypi.com/categories/685-app-notes-guides-whitepapers/documents/RP-010033-WP-1-H.264%20encoding%20performance%20on%20Raspberry%20Pi%205_series%20computers.pdf)

The generic `h264_v4l2m2m` encoder name may appear in an FFmpeg build even when
there is no H.264 encode device. The tested Nuvo Pi exposes camera/PiSP nodes and
an HEVC decoder, but no hardware H.264 encoder, so the runtime deliberately does
not select V4L2 M2M.

## Pipeline placement and backpressure

```text
trigger + paired capture
  |-> current capture status and eligible exposure metering
  |-> requested raw mask/cadence -> bounded native product worker
  |                                 -> rectification only where requested
  |                                 -> SGBM only for matcher/depth products
  |                                 -> metric conversion only where requested
  |                                 -> complete local product publication
  |                                 `-> requested disparity/depth preview
  `-> requested image preview cadence -> three-slot owning input pool
                                       -> private lazy CPU rectifier if needed
                                       -> RGB/mono and side-by-side conversion
                                       -> one-slot encoding worker
                                       -> atomic NVE1 snapshot and TLS fanout
```

The image branch admits a capture only when there is active encoded demand,
its cadence interval is due, and one of three fixed input slots is free. It
copies each selected camera's mono or ISP I420 plane once into that slot and
never retains a camera request. Cadence is consumed at this admission, so a
slow stereo matcher cannot multiply preview copies or delay image delivery.
The preview worker owns its CPU rectifier and immutable calibration maps;
it does not share native EGL/OpenCV execution state. The pending queue has one
slot and replaces old pending work. Pool saturation drops a complete preview
instead of expanding memory or blocking capture. Camera timestamps include a
bounded 2 ms cadence jitter tolerance.

The native product branch follows the union of live local subscriptions. A
mono/RGB-only request skips SGBM and depth conversion. No subscriber means no
native queue submission. The gateway holds persistent local demand leases for
raw and encoded clients; disconnect releases them. Explicit diagnostic trace
or raw-snapshot command-line options count as output requests. Capture status
reports requested masks/cadence, processing submissions, preview admissions
and copies, and activity flags for rectification, matching, depth, and encoding.

Numeric previews reserve their cadence only after a native request is admitted.
Raw cadence is evaluated separately, and only a due preview adds disparity/depth
dependencies. A pending reserved preview survives ordinary raw replacements;
newer reserved previews can replace it within the same bounded queue. Status
reports numeric admissions and raw requests dropped to protect that pending
capture. This prevents a 1 Hz depth preview from driving stereo at a concurrent
10 Hz mono subscriber's cadence.

## Configuration

The installed control file is `/etc/nuvo-depth/encoding.json`. The daemon checks
its modification time before capture, at completed-frame submission, and before
publishing an encoded result, and applies a valid change without a restart. An invalid or unreadable change leaves the last valid configuration
active. A missing initial file fails safe to raw-only operation.

The checked-in default is `config/encoding.json`:

```json
{"schema_version":1,"mode":"raw","codec":"h264","source":"left_rectified","width":0,"height":0,"cadence_hz":10,"bitrate_bps":4000000,"jpeg_quality":80,"keyframe_interval":1,"threads":1,"low_latency":true,"preset":"veryfast"}
```

`width` and `height` are both zero for native resolution, or both even values no
larger than 1920 x 1080. The image sources are:

| Source | View |
|---|---|
| `left_mono`, `right_mono` | Unrectified logical camera, mono |
| `left_rgb`, `right_rgb` | Unrectified logical camera, RGB |
| `left_rectified`, `right_rectified` | Rectified logical camera, mono |
| `left_rectified_rgb`, `right_rectified_rgb` | Rectified logical camera, RGB |
| `rectified_stereo` | Rectified left then right, side by side, mono |
| `rectified_stereo_rgb` | Rectified left then right, side by side, RGB |
| `left_color`, `right_color` | Compatibility names for unrectified color |
| `disparity`, `depth` | Numeric-product visualization, requires stereo |

Camera color inputs use [libcamera sYCC](https://github.com/libcamera-org/libcamera/blob/master/src/libcamera/color_space.cpp):
full-range luma and Rec.601 chroma. Conversion preserves that range in RGB and
JPEG; H.264 explicitly converts and signals video range. Color inputs preserve
ISP chroma through the RGB preview conversion. Only the
selected camera is copied for a single-camera source; side-by-side sources
admit both associated cameras. No preview mapping/copy occurs while encoded
demand is absent, encoding is disabled, or cadence/pool admission rejects a
capture. At native 728 x 544 resolution, side-by-side output is 1456 x 544;
larger source grids may require explicit output dimensions to stay within the
encoder's 1920 x 1080 output bound. Raw local RGB and mono products retain their
own lossless descriptors; encoded previews remain lossy.

Floating-point disparity and depth are mapped to an
8-bit preview plane before encoding; raw NUV1 remains the authoritative numeric
data path.
Disparity preview brightness uses each frame's accepted maximum disparity,
so changing the stereo range does not silently clip values above 64 pixels.
Invalid disparity pixels are black. The preview is lossy and must not be used
as a numeric disparity product.

| Mode | Raw NUV1 | Encoded NVE1 |
|---|---:|---:|
| `raw` | yes | no |
| `encoded` | no | yes |
| `both` | yes | yes |

H.264 emits baseline-profile Annex B access units with repeated SPS/PPS headers,
no B-frames, an independent keyframe per live image, and optional `zerolatency`
tuning. MJPEG
emits one complete JPEG per envelope. Runtime bounds are 60 Hz, 50 Mbit/s, four
encoder threads, 1920 x 1080 pixels, 32 MiB encoded payload, and 64 KiB metadata.
The live daemon and public controls require `keyframe_interval=1` for enabled
H.264. Every access unit is independently decodable, including its SPS/PPS
headers, so consumers can skip any number of frames. This costs compression
efficiency compared with predictive GOPs. Longer GOPs remain supported only by
the standalone `FrameEncoder`/benchmark API for offline callers that preserve
every encoded frame; the live latest-frame transport does not provide that
continuity. Valid configuration changes discard queued old work, remove the
old latest snapshot, and prevent an in-flight old encode from publishing after
the change. Encoded stream sequence remains monotonic across configuration
changes within the same encoder lifetime. Each envelope also identifies the
capture clock, selected calibration hash/authority, and the exact
`encoding_applied_revision` SHA-256 of the control document. Loading a control
file reports a requested revision; it becomes applied only when a frame from
that revision is successfully encoded and published. The independent encoding
generation is separate from the captured product's configuration epoch.

## Remote API and transport

The existing TLS gateway listens on the same interface for Wi-Fi, Ethernet, or
USB Ethernet. The image format is independent of the underlying IP link.

- `GET /api/v1/encoding` — active configuration
- `POST /api/v1/encoding` — validated privileged configuration update
- `GET /api/v1/frame/latest` and WebSocket `/api/v1/stream` — raw NUV1
- `GET /api/v1/encoded/latest` and WebSocket `/api/v1/encoded/stream` — encoded NVE1

Mode-disabled endpoints fail closed. Authentication, CSRF protection, bounded
sessions, TLS, response bounds, and the system-agent allowlist are shared with
the rest of the gateway. The web console exposes mode, codec, source,
resolution, cadence, bitrate/JPEG quality, a fixed independent-frame interval,
thread count,
latency tuning, and preset controls. It previews JPEG directly and H.264 through
WebCodecs when the browser supports it.

`NVE1` is a big-endian, bounded envelope:

| Offset | Size | Meaning |
|---:|---:|---|
| 0 | 4 | ASCII `NVE1` |
| 4 | 2 | schema version (`1`) |
| 6 | 2 | flags; bit 0 is keyframe |
| 8 | 4 | JSON metadata length |
| 12 | 8 | payload length |
| 20 | 8 | monotonic encoded-stream sequence |
| 28 | 4 | reserved, zero |
| 32 | variable | metadata, then encoded bytes |

The Python SDK's `decode_encoded_frame`, `latest_encoded`, `encoding`, and
`configure_encoding` methods provide the same interface without browser code.

## Measured Pi 5 performance

Measurements below were taken on the assembled Nuvo Pi on 2026-09-10 with the
distribution FFmpeg/libx264 (`libavcodec 61.19.101`), four threads, low-latency
mode, 8 Mbit/s H.264, and 300 synthetic structured grayscale frames. They measure
the encoder itself, not network throughput. The board reported `throttled=0x0`.

| 1920 x 1080 profile | Achieved | p50 encode | p95 encode | p99 encode |
|---|---:|---:|---:|---:|
| H.264 `veryfast` | 59.4 fps | 16.13 ms | 22.03 ms | 28.97 ms |
| H.264 `superfast` | 64.1 fps | 15.44 ms | 18.12 ms | 22.59 ms |
| H.264 `ultrafast` | 82.9 fps | 12.04 ms | 13.87 ms | 15.65 ms |
| MJPEG quality 80 | 159.8 fps | 6.16 ms | 6.21 ms | 66.85 ms |

Use `veryfast` for the native 728 x 544 / 10 Hz stereo profile. Use `ultrafast`
when 1080p60 must coexist with other Pi work; its measured p99 remains below the
16.67 ms frame period. `superfast` is a useful quality/throughput middle ground,
but its p95 exceeds a single 60 Hz period and the asynchronous newest-frame rule
may intentionally drop preview frames under concurrent load.

The live dual-camera run processed 100/100 associated frames with zero capture
sequence gaps, zero trigger errors, zero processing failures, zero encoder
failures, and 100 encoded H.264 frames. The last native-resolution encode took
7.63 ms. Raw and encoded snapshots were both served byte-for-byte through the
authenticated Pi gateway. This validates capture, rectification, depth, encoder,
and gateway integration at the current 728 x 544 / 10 Hz qualified profile; it
does not qualify 1080p60 camera capture, Wi-Fi airtime, or USB electrical design.

Separate Pi camera runs verified the mode boundaries: raw-only created
`latest.nuv` and no `latest.nve`; encoded-only created `latest.nve` and no raw
network snapshot. The gateway returned 200 only for the enabled representation
and 503 for the disabled one. The final ARM64 Debian cohort built successfully,
contained the encoder configuration, binaries, services, and updated web assets,
and declared the generated FFmpeg runtime dependencies. An extracted package
binary then completed another 20/20 live raw-plus-H.264 frames with no capture,
processing, or encoding failure and no unresolved shared library.

### Experimental native-image 60 Hz profile

`config/nuvo_pi5_depth_100mm_60hz_experimental.yaml` and
`config/encoding_728x544_60hz_experimental.json` request 60 Hz hardware
triggering and low-latency H.264 at 12 Mbit/s. The earlier 600-frame run below
used a 273 x 204 matcher grid, with 728 x 544 rectified/encoded images. That
historical performance result does **not** apply to the current profile: the
matcher now runs at the full 728 x 544 grid with a 128-pixel search interval.
The current full-resolution SGBM Pi benchmark measured about 215 ms median
compute while a previous 128-disparity StereoBM trial measured about 24 ms;
neither is a verified sustained live 60 fps result. See
`docs/pi5-fullres-stereo-flicker-trace.md` for the new live qualification plan.

The camera-only hardware run delivered 600 associated stereo pairs in 10.68 s,
with a 59.78 Hz sensor-timestamp rate, zero trigger or requeue failures, and no
throttling. The complete capture -> rectification -> disparity/depth -> H.264
run delivered 600/600 processed and 600/600 encoded frames without a component
failure, but sustained 45.72 fps over a separate 10.43 s live observation. The
last observed processing stages took 11.62 ms (4.43 ms rectification and 6.54 ms
stereo), while H.264 generally took about 3-5 ms asynchronously. The remaining
gap is therefore a dual-camera ISP/GPU/request-recycle throughput limit under
the combined workload, not an encoder limit. This profile is deliberately
named experimental and must not be represented as qualified 60 fps.

The gateway watches the atomic encoded snapshot once and fans new snapshots
out to bounded client sessions. The WebCodecs client also checks envelope
sequence and resets after a gap; live H.264 frames are always independent, so
the next delivered image can be decoded immediately.
Preview restarts now invalidate callbacks from the old WebSocket and ignore
frames whose codec, source, or explicit dimensions do not match the applied
configuration. MJPEG completions are ordered by arrival so an older asynchronous
decode cannot overwrite a newer image. The form tracks unapplied edits rather
than treating keyboard focus as a reason to leave controls stale.

In the earlier reduced-resolution trial, streaming updates were sent through
the socket-activated system agent and both
the gateway and encoder must reference `/etc/nuvo-depth/encoding.json`. A live
Pi check switched the web UI from left rectified 728 x 544 to disparity
272 x 204 and back, with the preview metadata and image changing after each
successful `Applied` response. Pointing a test gateway at a placeholder agent
socket reproduces the earlier `encoding control request failed` message and is
not a valid live-control setup.

Two runtime changes from this investigation remain useful at every rate:
camera leases are returned immediately after rectification so SGBM/depth can
overlap the next capture, and native disparity classification is partitioned
across the configured OpenCV worker pool. A dedicated camera-request recycler
also keeps libcamera queue operations off the processing worker. The Pi test
suite passed 40/40 after these changes, and the sustained hardware run reported
no capture, processing, encoding, queue-replacement, or thermal error.

For desktop inspection, `tools/live_stream_relay.js` can expose a loopback-only
HTTP UI while it tunnels the appliance's authenticated TLS WebSocket. It reads
the copied claim token once and deletes that temporary file immediately:

```powershell
node tools/live_stream_relay.js --token-file <temporary-token-file> `
  --upstream-host cammow.local --upstream-port 17443 --listen-port 8767
```

## Reproducing the encoder benchmark

```bash
build/pi5-release/depthd_encoder_benchmark \
  --codec h264 --width 1920 --height 1080 --frames 300 --fps 60 \
  --bitrate 8000000 --threads 4 --preset ultrafast
```

USB2 bulk capacity is not the same as guaranteed application throughput. At
8 Mbit/s the H.264 payload budget is modest for USB Ethernet and ordinary Wi-Fi,
whereas raw 1080p60 grayscale alone is about 995 Mbit/s before framing and is not
a practical USB2 stream. Keep raw local IPC available for on-device consumers;
select `encoded` for a constrained link or `both` when local raw data and a
remote visual stream are both required.
