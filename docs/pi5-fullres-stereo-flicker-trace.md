# Full-resolution stereo and 60-frame flicker trace

Both Pi 5 profiles now match disparity and depth directly on the 728 × 544
capture grid. `config/stereo.json` is the starting runtime control file, and
the new **Depth algorithm** web tab switches between OpenCV StereoSGBM and
StereoBM. Minimum disparity, search range, accepted maximum disparity, block
size, uniqueness, speckle filtering, left-right consistency, pre-filter cap,
BM texture controls, bright-pixel rejection, and all four Pi CPU cores are
configurable. Controls reload at the start of a depth frame without restarting
the cameras. The `specular_threshold` field is specifically an ISP-luma
brightness cutoff, not a general specular-reflection classifier; 256 disables
it. The native frame metadata names the selected matcher and full grid.

The 60 Hz trigger is a capture target, not a guarantee of 60 full-resolution
depth results. The quality profile now uses 96-disparity SGBM HH, block 5,
uniqueness 0, and a validity-preserving 5 x 5 median. The previous 3-way path
was faster but reproduced long horizontal disparity streaks in low-texture
regions. Eight-direction HH reduced the measured streak fraction from 11.1%
to 2.1% across three retained Pi captures. Sustained throughput and coverage
must be remeasured on the deployed build.

## Bounded stage capture

With the Pi available, build/deploy the updated native daemon and gateway,
put the matching `config/stereo.json` at `/etc/nuvo-depth/stereo.json`, and
start the PC receiver **before** the bounded Pi run:

```powershell
node tools/collect_stream_trace.js --output C:\Users\Workstation\Documents\NuvoDepthPcTrace --count 60 --timeout-seconds 180
```

On the Pi, run the live daemon with its normal calibration, identity, encoding,
status, and snapshot arguments, plus `--trace-dir /tmp/nuvo-flicker-60-UNIQUE
--frames 60`. The trace directory must be empty. Running the bounded daemon
requires stopping the existing capture owner first; the normal service can
then be restarted. Use `--trace-dir` only for diagnosis because it maps camera
DMA-BUFs and writes dozens of images and product packets. It intentionally
does not change the production fast path when omitted.

For each processed `capture_sequence`, the Pi trace writes:

| Stage | File | Contents |
| --- | --- | --- |
| ISP before rectification/depth | `isp_left_N.png`, `isp_right_N.png` | Original Y planes from paired camera DMA-BUFs |
| ISP controls | `isp_metadata_N.json` | Per-camera exposure time, analogue gain, digital gain, sensor timestamps |
| After rectification/depth | `post_depth_N.nuv` | Rectified left/right, native disparity, depth, confidence, validity |
| After encoding | `post_encoding_N.nve` | Exact encoded packet written by the encoder |
| After network/WebSocket on PC | `pc_stream_N.nve` | Exact packet delivered to this PC |

Transfer the Pi trace to this PC, then compare matching sequences:

```powershell
wsl -e python3 tools/pi/analyze_flicker_trace.py --pi-trace /mnt/c/Users/Workstation/Documents/NuvoDepthPiTrace --pc-trace /mnt/c/Users/Workstation/Documents/NuvoDepthPcTrace --output /mnt/c/Users/Workstation/Documents/NuvoDepthFlickerReport.json
```

The report measures the same central luma ROI at each stage, correlates it
with ISP-reported digital gain, records frame coverage and disparity grids,
and checks byte-for-byte equality between post-encode and PC packets. If
brightness swings are already in `isp_*`, the next investigation belongs to
sensor/ISP gain, XTRIG pulse shape, and lighting. If ISP luma is steady but
rectified luma changes, inspect DMA-BUF/GPU rectification. If only decoded
NVE changes, inspect the encoder. If PC packets differ bytewise, inspect the
gateway/transport.

The native live path sets `AeEnable=false`, `AwbEnable=false`, a fixed
*requested* `ExposureTime` and `AnalogueGain`, and fixed colour gains at camera
start. In IMX296 external-trigger mode, the requested shutter value is **not**
the physical exposure duration: the XTR low-pulse width controls it. See
[Raspberry Pi's Global Shutter external-trigger documentation](https://www.raspberrypi.com/documentation/accessories/camera.html#external-trigger-on-the-global-shutter-camera).
The native daemon owns the current shared stereo controller in
`src/exposure_control.cpp`; the historical
`tools/pi/stereo_auto_exposure.py` implementation is retained only as a test
and tuning reference. The IMX296 external trigger is driven by a user-space
GPIO low pulse. Live status reports the last/minimum/maximum process-side pulse
duration measured around the GPIO sleep and release calls. This can expose
large software scheduling excursions, but it is not an electrical XTR
measurement and cannot replace a scope or logic analyzer.

## September 13 Pi 5 validation

The updated native daemon, gateway, CLI, system agent, web assets, and control
files were installed on `cammow`. The native depth service and gateway are
enabled and active; the legacy Python depth service is disabled. The current
installed stereo control selected SGBM with a 96-pixel search range. The
packaged `config/stereo.json` now selects the corrected full-resolution quality
candidate: SGBM HH, range 96, block 5, uniqueness 0, and median 5. The
gateway engineering port is 17443, relayed to the PC at
`http://127.0.0.1:8767/`. The services were started and tested but **not
reboot-tested**. The prior installed files have `.pre-codex-20260913` backups
on the Pi. See the two `deployment/systemd/*fullres.conf` and
`*engineering-port.conf` drop-ins for explicit launch configuration.

Pi-side CTest passed 40/40. A bounded trace recorded 60 ISP pairs, 60
full-grid (728 × 544) post-depth products, and 60 H.264 packets; the PC
received the corresponding 60 packets. The Pi and PC encoded packet bytes
matched 60/60, with no decode failures. The full trace, analyzer JSON, and
rendered previews are in `deliverables/pi5-flicker-20260913/`.

Across the aligned frames, the central ISP-left luma ranged 5.08–17.47
(8-bit scale). Left/right ISP luma correlated at 0.966. ISP to rectified-left
luma correlated at 0.99981, rectified-left to PC H.264 decode at 0.99971,
and encoded to PC at 1.0. The *reported/requested* exposure was exactly
7,999 µs on every frame; actual XTR low widths were not measured.
Analogue gain varied only 1.97242–1.99526 and digital gain
1.00237–1.01398; digital-gain/ISP-luma correlation was 0.023. Thus the
observed brightness swings enter **at or before the ISP Y output**, not in
depth matching, rectification, encoding, or the network. This does not yet
distinguish PWM/ambient lighting from sensor/ISP or XTRIG timing. The current
Pi trigger path sets a GPIO low, sleeps for 8,000 µs in a user-space thread,
then sets it high. Because this pulse physically determines exposure, timing
jitter is a leading hypothesis even though libcamera metadata stays fixed.
The next controlled test is a scope/logic-analyzer measurement of XTR low
width across the same 60 frames, under steady DC lighting, with raw Bayer and
ISP Y compared. A hardware-timed pulse source should be considered only
after that evidence; avoid changing matcher or encoder settings as a
purported flicker fix.

The native capture target is 60 Hz. At full resolution, sustained live depth
throughput was about 5.2 fps with SGBM128 (about 186 ms matching and 200 ms
total), and about 6.8 fps with SGBM96 (about 149 ms matching and 163 ms
total). SGBM64 reached about 11 fps with a narrower disparity range. BM is
much faster (about 16–26 ms matcher time), but the dark scene made its strict
settings sparse (about 2% valid); relaxing filters increased coverage at the
cost of substantial speckle. Do not represent the configured 60 fps encoder
cadence as 60 fps full-resolution depth throughput. The production live
snapshot directory must remain on `/run` tmpfs: diagnostic writes to an
SD-backed `/srv` directory caused severe process I/O stalls.

The Streaming tab was checked interactively: raw-only, encoded-only, and
parallel raw+encoded modes, source switching, live H.264 preview, and control
apply all worked. The Depth algorithm tab switched BM and SGBM without a
camera restart and accepted a live 96-pixel range. A PC relay session-refresh
fix now retries expired HTTP and WebSocket credentials after a gateway restart;
the previously failing control request returned HTTP 200 after that restart.

In the final live check, `depthd` used roughly 289% CPU (about 2.9 Pi cores)
and 247 MB RSS; the gateway used about 16% CPU and 72 MB RSS. Pi temperature
was 73.6 °C with `throttled=0x0`. The capture ISP runs in PiSP hardware and
rectification uses EGL/GL DMA-BUF on the GPU (about 9 ms/frame). OpenCV SGBM
is the dominant CPU stage (about 132–149 ms at range 96). H.264 encoding is
software on the CPU (typically 4–8 ms), not a Pi 5 hardware video encoder.
Moving SGBM itself to GPU would require a separate, validated matcher backend;
the existing GPU rectification does not make OpenCV SGBM run there.

## September 15 horizontal-streak investigation

The earlier clean repository sample was produced by the historical 364 x 272
matcher followed by disparity-aware restoration to 728 x 544. Moving matching
to the full 728 x 544 grid removed that regularization and exposed a systematic
vertical rectification residual plus directional 3-way aggregation failures.
A robust transform fitted across retained capture sequences 1, 203, and 405
reduced median absolute vertical feature residual from about 1.77 px to
0.15 px without changing projection or depth geometry.

On the corrected images, full-resolution HH with median 5 reduced the
horizontal-streak fraction to 2.06%, retained 70.0% camera coverage, and reduced
three-capture disparity instability to 8.40 px p95. The half-resolution HH
candidate with 48 working disparities / 96 restored retained 74.2% camera
coverage, measured 2.16% streak fraction, and reduced the same instability to
3.17 px p95. StereoBM was substantially sparser in the tested scene. The
quality improvement is therefore attributed to calibration correction,
directional aggregation, and validity-preserving smoothing—not the encoder.
Both final profiles were rerun independently three times against all three
retained captures. Every color and grayscale output hash and every non-timing
metric matched exactly across the three runs.

## September 15 deployed Quality/HH verification

The complete current source cohort was rebuilt on the Pi 5 and passed 43/43
Pi-native tests before installation. A fresh aligned run then recorded 60 ISP
pairs, 60 full-grid post-depth products, 60 H.264 disparity packets on the Pi,
and the corresponding 60 packets on this PC. All 60 network packets matched
the Pi packets byte for byte and all decoded successfully.

Brightness variation remains upstream of the software pipeline. ISP-left luma
ranged 8.31-27.50 with an adjacent RMS change of 8.30. ISP to rectified-left
correlation was 0.999996, and the two sensors' ISP luma correlated at 0.993428.
Exposure was 7,999 us, analogue gain 1.99526x, and digital gain 1.00237x on
every frame. The matcher, rectifier, encoder, network, and browser are therefore
excluded as the source. The next hardware test must measure the physical XTR
low pulse with a scope or logic analyzer under verified DC illumination. If the
pulse is stable, compare raw Bayer against ISP Y and inspect the light driver.

Sixty separate live Quality/HH samples measured 8.43 ms median GPU
rectification, 0.18 ms stereo preparation, 325.58 ms HH matching, 3.70 ms
validity-preserving filtering, 3.60 ms depth/overhead, and 342.01 ms total
compute. Output rate was 2.84 depth frames/s. H.264 monitoring encoded in 7.10
ms median and was not the bottleneck. During a ten-second resource sample,
`depthd` used 100-118% CPU and 248-369 MiB RSS, the gateway used 0-2% CPU and
34 MiB RSS, temperature was 66.4 C, and `get_throttled=0x0`.

Evidence is retained in `deliverables/pi5-flicker-20260915-hh60/` and summarized
in `output/pdf/nuvo-depth-stereo-validation-20260915.pdf`.

## September 16 exposure-trigger instrumentation

The production native service now records process-side XTR pulse duration in
addition to requested/applied exposure, and counts pulse durations that exceed
the requested width by more than the 250 us scheduling guard. This measures the
time around the GPIO-low/sleep/GPIO-high calls; it is not an electrical probe.

A fresh-service 60-published-frame run held the request at 8,000 us / 3.0x.
Both cameras reported 7,999 us / 2.98538x with no inter-camera difference,
maximum sensor timestamp delta was 40 us, and 189 trigger attempts completed
without trigger errors or cadence deadline overruns. Process-side pulses ranged
from 8,057 to 8,702 us and one exceeded the 250 us guard. Continued service
telemetry subsequently reached 16 guard exceedances and a 10,256 us maximum.

This is direct evidence that the Linux user-space release path does not hold an
exact 8,000 us duration, and makes it the leading software-side source for the
previous at/before-ISP brightness variation. It remains a hypothesis about the
physical exposure until an oscilloscope or logic analyzer observes XTR under
verified DC illumination. The decisive test remains electrical XTR width plus
aligned raw Bayer and ISP Y; matcher, rectifier, encoder, network, and browser
changes are not credible flicker fixes based on the existing aligned evidence.
