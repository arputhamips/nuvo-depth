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
depth results. An earlier Pi measurement while the live stack was running
found 128-disparity SGBM at about 215 ms median per frame (4.6 Hz) and
StereoBM at about 24 ms (roughly 42 Hz matcher-only). Sustained live throughput
and coverage must be remeasured on the deployed build.

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
The separate `tools/pi/stereo_auto_exposure.py` controller is not called by
the native daemon. The IMX296 external trigger is driven by a user-space
GPIO low pulse; Linux scheduling jitter has been observed, so the ISP metadata
trace must be evaluated before attributing flicker to the trigger or encoder.

## September 13 Pi 5 validation

The updated native daemon, gateway, CLI, system agent, web assets, and control
files were installed on `cammow`. The native depth service and gateway are
enabled and active; the legacy Python depth service is disabled. The current
installed stereo control selects SGBM with a 96-pixel search range, while the
packaged `config/stereo.json` remains the 128-pixel starting profile. The
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
