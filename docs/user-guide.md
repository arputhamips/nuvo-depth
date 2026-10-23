# Nuvo Depth Pi 5 user guide

For first-time installation, use the [main user wiki](https://docs.nuvohardware.com/vision-one/):
[bundled units](https://docs.nuvohardware.com/vision-one/install/bundled-pi),
[your own Pi or SD card](https://docs.nuvohardware.com/vision-one/install/byo-pi),
and [browser connection](https://docs.nuvohardware.com/vision-one/guide/getting-started).
This is the versioned console and runtime reference.

## First connection

1. Power the Pi 5 camera appliance and connect WiFi or Ethernet. Select a product
   set and rate within the connection's displayed bandwidth budget; use Ethernet
   for larger raw streams.
2. Open the printed `https://HOSTNAME.local:7443/` console URL. If mDNS is
   unavailable, use the Pi's IP address with port `7443`. Verify the device's
   certificate against the trusted setup material before using the console.
3. The console connects automatically. No browser password, setup code or
   owner-password setup is required, including on previously configured units.
4. Check **Overview**. With capture enabled, confirm **Streaming**, the installed
   profile's dimensions, and no red health message. Pending calibration can
   intentionally leave capture disabled while the console is available.
5. Open **Live view** and select the products you need under **Outputs**. The
   browser initially requests only the left mono image at 5 Hz. Selected products
   share the same capture identity. Choose **View the camera**, **Use depth**, or
   **Inspect stereo** first; individual products are under **Advanced**. Presets
   select a conservative browser cadence, which you can change within the
   connection budget before applying.

Use the sun/moon control in the header to select light or dark appearance. The
choice is kept in that browser; without an explicit choice the console follows
the operating-system color preference.

The overview shows live camera, depth-rate, compute-time, and calibration status.
Expand **Setup guide** for first-use checks. Public beta 8 Live view offers
**Show first depth** and **Save depth sample** after two advancing associated
depth/validity frames; the download preserves original NUV1 metadata. Older
beta 6 lacks these buttons. The [complete journey](https://docs.nuvohardware.com/vision-one/guide/getting-started)
covers the console controls, manual Outputs steps and SDK route. The full **Performance** monitor
appears on Overview and Diagnostics, with observed compute-time history,
sampled-frame P95, and a stage timing table. Its **Analysis target** compares
compute time with a frame budget; it does not change the device configuration.
Capture and Matcher keep camera and depth previews beside the controls, with
capture target, measured depth output rate, and compute time shown separately.
**Settings** contains Capture, Depth profiles, Matcher,
Streaming, and Outputs; **Device** contains Calibration, Diagnostics, and
Software update. Capture, Matcher, and Streaming use **Apply settings** to save
and **Discard changes** to reload the latest device settings. Each Advanced
controls card can collapse as a whole; its individual groups can also collapse.
Closing either preserves edits and shows pending changes or validation errors
in the header. Apply and Discard remain outside the card in a sticky action bar.

## Image pipeline status

Use **Capture**, **Matcher**, **Streaming**, **Outputs**, and **Live view** to
configure and check the camera. The experimental node editor has been retired;
the console uses the same shared device settings and frame codec directly.

## Recommended daily workflow

Use **Manual** exposure for repeatable stereo depth. Start at the live shutter
limit shown by the console and 2× analogue gain, then lower shutter time to
reduce motion blur or lower gain to reduce noise. Both sensors receive one
shared command and report their applied values on the same page.

Use **Quality** when clean full-resolution disparity is the priority. It uses
full-resolution eight-direction SGBM HH, a 96-pixel search, 5-pixel block,
5 x 5 validity-preserving smoothing. **Balanced** is the default: two-thread
3-way SGBM with 128 disparities, no additional median smoothing and a 10 Hz
capture target. It has lower latency, but it does not suppress
low-texture scanline failures as completely as HH. **Fast diagnostic** uses
3-way aggregation, a narrower 64-pixel search and no additional median smoothing.
It can produce long horizontal streaks and gives up nominal near range below about
0.688 m with the current provisional beta 5 seed. This is a theoretical search
bound, not a measured usable range. Use **Wide search** only when near geometry needs disparities
from 96 to 127; it costs more CPU. The **Matcher** page exposes every
parameter and the SGBM/StereoBM switch.

Every profile is an unqualified candidate. The retained Pi Quality/HH run
measured 2.84 depth frames/s at a 60 Hz capture target; the configured trigger
rate does not promise the same depth rate. See [performance profiles](performance-profiles.md)
for exact settings and the evidence required to qualify a profile.

Leave **Compressed preview encoder** off when your application only needs data.
Keep **Uncompressed network data** on for HTTPS/browser/SDK consumers and select
only the required images or numeric products. Local socket delivery is
independent of this gateway mode. None of these data products are sensor
RAW/Bayer: mono and colour images originate from the ISP; disparity and depth
are processed measurements.

Turn **Compressed preview encoder** on for a browser/support preview. Streaming
settings are device-wide: one encoder source, format and ceiling are shared by
all preview clients. Outputs settings apply only to this browser. Its requested
preview rate may be lower than the device ceiling. The valid network modes are:

| Preview encoder | Uncompressed network data | Network result |
| --- | --- | --- |
| off | on | Maximum depth headroom; SDK/product output only |
| on | on | Product output plus a monitoring preview |
| on | off | Compressed preview only; local socket data remains available |

The UI requires at least one delivery mode to remain available. With no active
output consumers, raw processing, serialization and encoding stay idle. H.264 on
Pi 5 is software encoding. Image previews run independently of the matcher;
numeric previews request matching only at their own cadence. Encoder settings
are ceilings and cannot increase the selected source's throughput.

**Depth preview** and **Disparity preview** are 8-bit visualizations followed by
lossy compression. The encoded depth display maps 0.2–12.2 m; it cannot recover
the original float measurement. For measurements, request **Depth** plus
**Validity** as uncompressed products. The browser bounds pending decode work,
replaces waiting JPEGs with the latest frame, and retries interrupted
WebSockets while temporarily polling. Routine skipped independent H.264 frames
do not reset the decoder.

## Exposure and flicker

**Manual · constant** disables libcamera AE and AWB, applies one exposure,
analogue gain, and colour-gain command to both cameras, and uses that exposure
time for the external active-low IMX296 XTR pulse. The permitted gain range is
1–3×. The live shutter maximum is the smaller of 8,000 µs and the trigger
period minus a 250 µs guard; the console shows it in both microseconds and
milliseconds.

**Auto · stereo locked** meters both ISP luma planes and updates one shared
command. The default metering region is the central 60% of each dimension;
the YAML metering and clipping settings are active. Auto prefers shutter time
before analogue gain, filters log brightness, confirms a direction across
three observations and caps requested changes at 0.2 EV before shutter/gain
quantization. Persistent highlights select a lower acceptable brightness
instead of repeatedly halving the exposure and brightening it again.
That highlight compromise stays selected for the current convergence, even if
one adjustment moves the highlight just below the clipping threshold.

Once acceptable or at a control limit, Auto holds a fixed scene baseline.
It resumes adjustment after raw median changes by more than 20% and filtered
brightness by more than 15% in the same direction for three observations.
Gradual changes accumulate against that baseline. Both cameras' reported
controls must match the command before the configured settling interval can
complete; missing or mismatched metadata is reported as a hold state.
Switching from Manual starts at the current command. These acknowledgements
are sensor metadata, not measurement of physical XTR edges. Auto balances
highlight retention and shadow brightness within the limits; it does not
measure noise directly or cancel mains-frequency illumination flicker.

The normal page keeps only mode, manual shutter/gain, and Auto target visible.
Open **Advanced capture controls → Auto exposure limits** for bounds or **Auto
response** for the brightness deadband and cadence. These groups are editable
in Auto mode. **White balance** provides separate red/blue colour gains.
**Apply settings** persists through the system agent and waits for camera
confirmation. **Discard group changes** restores that group's saved edit-base
values. **Discard changes** reloads the latest complete device configuration.

If brightness changes on alternate frames, switch to Manual. If the change is
still visible in the ISP panels, the source is illumination/trigger/sensor
timing rather than rectification, stereo matching, encoding, or the network.
See [Operations and troubleshooting](operations-troubleshooting.md).

## Depth algorithm controls

- **Minimum disparity** and **search range** define the OpenCV search window;
  search range must be a multiple of 16.
- **Maximum disparity** is the accepted upper bound and must stay inside that
  search window.
- **Block size** trades detail for robustness; larger blocks smooth more.
- **Uniqueness**, **speckle**, and **left-right consistency** reject ambiguous
  or isolated matches. In OpenCV's SGBM 3-way implementation, nonpositive
  left-right difference settings use 1 pixel; `-1` does not disable that check.
- **Saturated-pixel threshold** rejects bright pixels below 256; 256 disables
  that rejection.
- **Additional disparity smoothing** applies a 3 × 3 or 5 × 5 median only to
  matcher-valid subpixel values. It never turns an invalid pixel into a valid
  one. Off disables this additional pass; OpenCV SGBM still applies its own
  internal 3 × 3 median.
- **SGBM path aggregation** selects 3-way, standard five-path, HH4, or full
  eight-direction HH. The default Balanced profile uses 3-way. Quality uses HH
  because it removes the long
  horizontal scanline failures seen in low-texture camera imagery; it uses
  substantially more CPU and memory. HH4 is exposed for experiments but showed
  vertical streaking on the retained Pi captures.
- **OpenCV CPU threads** default to two for the selected Pi 5 profile.
- **StereoBM** is faster but was much sparser with the tested 5-pixel block.
  Use it only after checking the validity view for the intended scene.

All matcher changes are schema-validated, atomically written, and applied on a
subsequent processed frame without restarting camera capture.

## Product delivery

Camera images can use a larger grid than matcher products. The console's
bandwidth estimate uses each grid separately and live captions show actual
product dimensions. Read products by name, not array position. Side-by-side
images place left then right horizontally. The pipeline is ISP image →
rectification → disparity → calibrated depth. Omitted products are absent.

| Data | Interpretation |
| --- | --- |
| Depth | Little-endian float32 optical-axis Z in metres, not Euclidean range |
| Disparity | Little-endian float32 pixels of the matching grid |
| Validity | `1` is valid; other values describe rejection |
| Matcher support (`confidence`) | Binary `0` / `255`, not a probability |

Use associated validity plus finite, positive depth for measurements. Preview
and data subscriptions may use different rates: associate device/run UUID,
capture epoch and capture sequence, not arrival time. Capture timestamps use
the device's boot-time clock and are not UTC. New frames include optional
per-camera timestamps and reported controls, grid mappings, disparity semantics
and calibration projection matrices. Older recordings may lack those fields;
do not invent geometry from image size alone.

The authoritative network product is `GET /api/v1/frame/latest` or WebSocket
`/api/v1/stream` over authenticated TLS. It contains one associated NUV1 frame
set rather than unrelated image requests. The Python SDK decodes this envelope:

```python
from nuvo_depth import NuvoClient

camera = NuvoClient("https://HOSTNAME.local:7443", ca_file="device-ca.pem")
camera.claim("DEVICE-ADMIN-TOKEN")
with camera.frames(products=("depth", "validity"), cadence_hz=10,
                   delivery="auto") as frames:
    frame = next(frames)
    print(frame.capture_sequence, frame.products.keys())
```

On-device processes should prefer the restricted local frame socket and sealed
read-only descriptors to avoid TLS and network copies. The ROS compatibility
adapter consumes completed local products; ROS does not own capture or stereo
processing. Gigabit Ethernet is the normal remote path. USB Ethernet remains a
qualified transport option only after the product VID/PID, power design, and
target-host compatibility gates pass.

## Safe changes

Streaming, stereo, and exposure changes apply live. Connection-preference
changes are different: they are provisional for 60 seconds and must be
confirmed through the new path or rolled back. Calibration is not part of the
normal user flow. Trained operators use **Device → Calibration** for
the separate [advanced calibration process](advanced-calibration.md). Its
encoded view is only a positioning aid; synchronized unrectified ISP frames are
captured and solved locally on the Pi.
