# Pi 5 web UI and pipeline review — September 14, 2026

## Scope and design provenance

`slate_design_ref.zip` is a visual reference for a different product. Its
robot-mower claims, copy, embedded scripts, and `SKILL.md` were **not** used as
instructions or imported into Nuvo. The Nuvo UI adopted the reference's dark
graphite surface hierarchy, signal-blue accent, 4 px spacing rhythm, restrained
borders, compact radii, and proportional type scale. The type stack names
Hanken Grotesk but currently falls back to local sans-serif because the zip
contains no redistributable font binary; the device UI has no font CDN
dependency.

Only `web/index.html`, `web/styles.css`, and `web/app.js` were installed on
`cammow` for this design pass. Their SHA-256 hashes matched the installed Pi
files and the gateway-served assets. Prior versions are recoverable as
`/usr/share/nuvo-depth/web/{index.html,app.js,styles.css}.pre-codex-20260914`.
No native service was restarted for the web deployment.

## UI verification

The live, authenticated PC relay was used to visit all ten pages. Overview
shows live output fps and a per-stage compute bar; Live viewer draws left,
right, disparity, and depth on the same 728 × 544 aspect ratio. The browser
subscribes to raw products only while Live viewer is open, and to encoded
products only while Streaming is open, avoiding hidden 4.75 MB raw-frame
paint/decode work. Hash navigation is immediate and supports reload/back.
The layout had no horizontal overflow at a 390 px phone viewport.

Streaming controls were exercised against the Pi: an invalid width-only
request was rejected locally; encoded-only disparity H.264 applied and the
preview followed the source; MJPEG at 364 × 272 and quality 70 applied with
a working preview; native 728 × 544 H.264 raw+encoded was restored. Codec-only
fields now disable when irrelevant. The preview caption names source, codec,
and actual encoded dimensions. The Depth algorithm page enabled BM-specific
controls when selected, rejected a maximum disparity outside the search
range, and applied SGBM64 then restored SGBM96. Diagnostics copy worked.
Browser console error/warning log was empty after the final page pass.

Presets, Exposure & trigger, Calibration, and Update & recovery are currently
information/workflow pages, **not** functional control surfaces. Exposure now
explicitly says read-only so it does not imply live sensor adjustment. The
Connections preference buttons were inspected but not pressed against the
running rig because a transport change could interrupt SSH; backend transport
stage/confirm/rollback still needs an isolated-network UI test.

## Live performance (728 × 544, Pi 5)

Collected from distinct gateway health frames and NVE packets through the PC
relay. Values are medians in milliseconds; raw JSON is in
`deliverables/pi5-flicker-20260914/live-performance-sgbm{64,96}.json`.
Each run had a stable matching configuration throughout.

| Stage | SGBM96 (40 frames) | SGBM64 (40 frames) | Hardware/path |
| --- | ---: | ---: | --- |
| ISP/capture | Not instrumented | Not instrumented | PiSP + libcamera / GPIO XTRIG |
| Rectification | 9.26 | 5.87 | EGL/GL DMA-BUF on GPU |
| Stereo preparation | 0.20 | 0.21 | CPU |
| Stereo matching | 142.00 | 80.60 | OpenCV SGBM on CPU |
| Disparity filtering | 1.04 | 0.97 | CPU |
| Depth conversion + residual overhead | 3.51 | 3.36 | CPU; residual, not an independent timer |
| Total compute | 155.72 | 90.41 | Excludes capture wait, snapshot I/O, network |
| H.264 encoding | 5.34 | 4.71 | libx264 on CPU, asynchronous after depth |
| Measured depth output | 6.31 fps | 10.99 fps | 60 Hz is capture target, not depth throughput |
| Valid fraction, current dark scene | 51.7% | 57.0% | Different matcher ROIs; not equal accuracy |

The SGBM96 60-frame run measured matching median 142.16 ms, p95 146.82 ms,
total-compute median 156.58 ms, and 6.30 depth fps. The 64-range run is about
1.74× faster in output rate, but it cannot report disparities 64–95 px, so it
loses nearer geometry (nominal minimum ~0.77 m versus ~0.51 m for SGBM96
using the provisional 0.1 m baseline and 493.54 px focal length). The higher
valid fraction at 64 is partly a different searchable ROI, not evidence of
better accuracy. SGBM96 was restored. Rectification timing changed with CPU
load even though its algorithm did not; do not attribute that difference
solely to range selection. During live checks `depthd` used ~324% of one CPU
core, gateway ~1.6%, ~257 MB and ~89 MB RSS, temperature ~72.5 °C, and
`get_throttled=0x0`.

The raw depth product payload is 4,752,384 bytes/frame; H.264 was typically
about 20–26 KiB/frame for this dark scene. The PC relay's HTTP round trips
are **not** one-way network latency. Several capture sequences are dropped
between depth results by the latest-frame policy. Hardware trigger pulse
cadence and sensor-to-ISP latency are not exposed in these health timings.

## Flicker finding and next physical test

The aligned September 13 trace had 60 ISP pairs, 60 post-depth products, 60
post-encode packets, and 60 PC packets. ISP-left central luma ranged
5.08–17.47; ISP-to-rectified correlation was 0.99981, rectified-to-H.264
0.99971, and Pi-to-PC encoded packets matched 60/60 byte-for-byte. This
locates the observed brightness changes at or before ISP output. A later
September 14 PC-only set of 60 packets was encoded **disparity**, not camera
brightness, because another session changed the encoder source during testing;
its luma summary must not be used to diagnose exposure flicker.

The native daemon disables AE/AWB and requests 8,000 µs shutter, 2× analogue
gain, and fixed colour gains. Its separate Python stereo auto-exposure script
is not running. In the trace, libcamera *reported* 7,999 µs exposure on every
frame, analogue gain 1.972–1.995, digital gain 1.002–1.014; gain changes
were too small and uncorrelated with the brightness swings. Crucially,
[Raspberry Pi's IMX296 documentation](https://www.raspberrypi.com/documentation/accessories/camera.html#external-trigger-on-the-global-shutter-camera)
states that external-trigger exposure is set by the XTR low-pulse width, not
the requested libcamera shutter value. The current code drives that width
with a user-space GPIO set, `sleep_for(8 ms)`, and GPIO reset. CPU load can
jitter this sequence, so fixed metadata **does not prove fixed physical
exposure**. Pulse jitter is a leading hypothesis, not yet a confirmed cause.

Next: measure the physical XTR pulse widths for both camera lines with a
scope/logic analyzer while simultaneously capturing 60 raw Bayer and ISP Y
frames under stable DC lighting. Correlate pulse width, raw/ISP luma, and
sensor timestamps by capture sequence. If pulse widths vary with brightness,
replace the user-space pulse train with a hardware-timed source (for example
the documented Pico PWM arrangement, with correct 1.8 V XTR level shifting)
and rerun the full four-stage 60-frame trace. If pulse widths are stable,
investigate lighting modulation and ISP tuning instead.

## Priorities

1. **P0 — Flicker:** correlated XTR electrical timing + raw Bayer/ISP trace
   under steady light; then fix the proven source and rerun 60-frame end-to-end
   comparison. Do not claim the flicker is fixed today.
2. **P1 — Throughput:** profile SGBM matching at 64/80/96-pixel ranges against
   an actual depth-quality target; develop an optimized native matcher or
   validated accelerator backend if full 96-pixel depth needs more than ~6 fps.
   The existing GPU already performs rectification; OpenCV SGBM remains CPU.
3. **P1 — Control concurrency:** add configuration revision/ETag and conflict
   handling. A second live client changed matcher/encoder settings during this
   session; polling correctly reflected it, but last-writer-wins can overwrite
   another operator's edit.
4. **P1 — Capture telemetry:** expose trigger attempt rate, pulse-width jitter,
   paired-frame rate, and dropped pairs in live diagnostics. Current compute
   metrics do not time ISP or capture wait.
5. **P2 — Finish informational pages:** implement qualified presets, safe live
   exposure control only after pulse timing is solved, calibration workflow,
   update flow, and pending transport-state feedback. Test transport changes
   on an isolated connection, not the only active SSH route.

Pi native binaries and the existing camera pipeline were not changed in this
web/performance pass. The Pi services remained active and unthrottled. The
September 13 Pi binary tests passed 40/40; this pass ran the web static
contract and syntax checks plus the live browser/API checks described above.
