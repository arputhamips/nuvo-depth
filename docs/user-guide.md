# Nuvo Depth Pi 5 user guide

## First connection

1. Power the Pi 5 camera appliance and connect WiFi or Ethernet. Select a product
   set and rate within the connection's displayed bandwidth budget; use Ethernet
   for larger raw streams.
2. Open `https://nuvo-depth.local:17443/`. If mDNS is unavailable, use the Pi's
   IP address with port `17443`.
3. Enter the device claim code when prompted. The browser exchanges it for a
   secure session; it does not store the code. On an engineering image only,
   an administrator can read the provisioned code from
   `/var/lib/nuvo-depth/identity/claim.token`.
4. Check **Overview**. Camera must show **Streaming**, dimensions must be
   728 × 544, and no red health message should be present.
5. Open **Live view** and select the products you need under **Outputs**. The
   browser initially requests only the left mono image at 5 Hz. Selected products
   share the same capture identity.

Use the sun/moon control in the header to select light or dark appearance. The
choice is kept in that browser; without an explicit choice the console follows
the operating-system color preference.

The overview shows live camera, depth-rate, compute-time, and calibration status.
Expand **Setup guide** for first-use checks. The full **Performance** monitor
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

The experimental node editor is paused and excluded from production builds.
Use **Capture**, **Matcher**, **Streaming**, **Outputs**, and **Live view** to
configure and check the camera. Its source is retained in the
[pipeline workspace](../web/pipeline-ui/README.md) for future development.

## Recommended daily workflow

Use **Manual** exposure for repeatable stereo depth. Start at the live shutter
limit shown by the console and 2× analogue gain, then lower shutter time to
reduce motion blur or lower gain to reduce noise. Both sensors receive one
shared command and report their applied values on the same page.

Use **Quality** when clean full-resolution disparity is the priority. It uses
full-resolution eight-direction SGBM HH, a 96-pixel search, 5-pixel block,
5 x 5 validity-preserving smoothing. **Balanced** is the default: two-thread
3-way SGBM with 128 disparities, no median smoothing and a 10 Hz capture target.
It has lower latency, but it does not suppress
low-texture scanline failures as completely as HH. **Fast diagnostic** uses
3-way aggregation, a narrower 64-pixel search and no median smoothing. It can
produce long horizontal streaks and gives up nominal near range below about
0.771 m. Use **Wide search** only when near geometry needs disparities
from 96 to 127; it costs more CPU. The **Matcher** page exposes every
parameter and the SGBM/StereoBM switch.

Every profile is an unqualified candidate. The retained Pi Quality/HH run
measured 2.84 depth frames/s at a 60 Hz capture target; the configured trigger
rate does not promise the same depth rate. See [performance profiles](performance-profiles.md)
for exact settings and the evidence required to qualify a profile.

Leave **Monitoring encoder** off when your application only needs raw products.
Keep **Raw product delivery** on and choose only the required images or numeric
products. Local SDK, WiFi, Ethernet, and approved USB Ethernet clients receive
their selected associated products without spending CPU on an unused codec.

Turn **Monitoring encoder** on for a browser/support preview. It is independent
of raw delivery, so the valid combinations are:

| Monitoring encoder | Raw delivery | Result |
| --- | --- | --- |
| off | on | Maximum depth headroom; SDK/product output only |
| on | on | Product output plus a monitoring preview |
| on | off | Encoded monitoring stream only |

The UI requires at least one delivery mode to remain available. With no active
output consumers, raw processing, serialization and encoding stay idle. H.264 on
Pi 5 is software encoding. Image previews run independently of the matcher;
numeric previews request matching only at their own cadence. Encoder settings
are ceilings and cannot increase the selected source's throughput.

## Exposure and flicker

**Manual · constant** disables libcamera AE and AWB, applies one exposure,
analogue gain, and colour-gain command to both cameras, and uses that exposure
time for the external active-low IMX296 XTR pulse. The permitted gain range is
1–3×. The live shutter maximum is the smaller of 8,000 µs and the trigger
period minus a 250 µs guard; the console shows it in both microseconds and
milliseconds.

**Auto · stereo locked** meters the central 60% of both ISP luma planes and
updates one shared command. It prefers shutter time before analogue gain,
quantizes changes, confirms a normal bright/dark direction twice, and waits for
the configured settle interval. Clipping in either camera is handled
immediately. Switching from Manual starts Auto at the live command instead of
jumping to a seed value. This avoids the left/right drift caused by independent
3A, but it cannot correct lighting that alternates within or between external
trigger pulses.

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
  or isolated matches.
- **Saturated-pixel threshold** rejects bright pixels below 256; 256 disables
  that rejection.
- **Disparity smoothing** applies a 3 × 3 or 5 × 5 median only to matcher-valid
  subpixel values. It never turns an invalid pixel into a valid one.
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

The authoritative network product is `GET /api/v1/frame/latest` or WebSocket
`/api/v1/stream` over authenticated TLS. It contains one associated NUV1 frame
set rather than unrelated image requests. The Python SDK decodes this envelope:

```python
from nuvo_depth import NuvoClient

camera = NuvoClient("https://nuvo-depth.local:17443", ca_file="device-ca.pem")
camera.claim("DEVICE-CLAIM-CODE")
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
