# Nuvo Depth Pi 5 user guide

For first-time installation, use the [main user wiki](https://docs.nuvohardware.com/vision-one/):
[bundled units](https://docs.nuvohardware.com/vision-one/install/bundled-pi),
[your own Pi or SD card](https://docs.nuvohardware.com/vision-one/install/byo-pi),
and [browser connection](https://docs.nuvohardware.com/vision-one/guide/getting-started).
This is the source-version console and runtime reference. The unified console
described here is a development update; the approved public beta 8 installation
can still show the older navigation until upgraded to this build.

## First connection

1. Power the Pi 5 camera appliance and connect Wi-Fi or Ethernet. Select a product
   set and rate within the connection's displayed resource budget; use Ethernet
   for larger uncompressed streams. A resource budget is not a measured network
   throughput guarantee.
2. Open the printed `https://HOSTNAME.local:7443/` console URL. If mDNS is
   unavailable, use the Pi's IP address with port `7443`. Verify the device's
   certificate against the trusted setup material before using the console.
3. The console connects automatically. No browser password, setup code or
   owner-password setup is required, including on previously configured units.
4. Check **Overview** for device health and calibration, then open **Live view**.
   With capture enabled, confirm advancing frames, the installed
   profile's dimensions, and no red health message. Pending calibration can
   intentionally leave capture disabled while the console is available.
5. Select the result you need under **Outputs**, then apply it to this browser
   or generate an application recipe. Selected products share a capture identity;
   they can have different dimensions. Numeric depth includes validity. Individual
   products and technical delivery settings remain available under Advanced.

Use the sun/moon control in the header to select light or dark appearance. The
choice is kept in that browser; without an explicit choice the console follows
the operating-system color preference.

Overview shows camera, depth-rate, compute-time, and calibration status.
Expand its **Setup guide** for first-use checks. Live view offers
**Show first depth** and **Save depth sample** after two advancing associated
depth/validity frames; the download preserves original NUV1 metadata. Older
beta 6 lacks these buttons. The [complete journey](https://docs.nuvohardware.com/vision-one/guide/getting-started)
covers installation and the SDK route; use this versioned guide for development
console navigation.

## Console navigation

The desktop sidebar contains four pages. On narrow screens it becomes compact
navigation above the page. Opening the console without a page link lands on
Overview; existing links to Live view and individual settings still work.

| Page | Purpose | Scope |
| --- | --- | --- |
| Overview | Device health, applied configuration, calibration, performance and setup guidance | Health monitoring only; starts no product stream |
| Live view | One primary viewer, camera/exposure controls and depth profile with advanced matcher tuning | Viewing is local; camera and matcher settings affect the device |
| Outputs | Choose products, representation, rate and application connection; review the browser's actual connection; manage delivery permissions | Browser selection and application recipe are local; delivery permissions are device-wide |
| Device | Calibration, diagnostics, updates and advanced shared preview encoder settings | Device-wide |

Profiles select a set of matcher parameters; they are not another output or
stream. Advanced edits are reflected as Custom when they differ from a named
profile. Applying camera, matcher or encoder settings changes the shared device;
merely changing the viewer or drafting an output recipe does not.

The Overview performance monitor's **Analysis target** compares compute time with a
frame budget. It does not change capture, matcher or delivery rates. Collapse
advanced controls without losing pending edits. Apply and Discard actions are
kept with their corresponding form and scope.

Returning to Overview releases this browser's live product demand while keeping
its selected outputs and unsaved settings. Open Live view or Outputs again to
resume the selected subscription. Performance shows the work requested by
active consumers; it can be idle when no consumer needs depth.

## Image pipeline status

Use **Live view** for camera/depth settings and **Outputs** to choose what a
consumer receives. The console uses the shared device settings and frame codec;
it does not create a separate pipeline for each navigation page.

Every viewer identifies its representation. An uncompressed image is rendered
in the browser; numeric depth is colored in the browser; a JPEG/H.264 picture is
decoded from the shared encoder. These can all look like images on screen, but
only the numeric product carries the original depth values. Compressed depth
pictures cannot be used to recover those measurements.

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
from 96 to 127; it costs more CPU. Advanced matcher controls under **Live view**
expose the parameters and the SGBM/StereoBM switch.

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

Turn **Compressed preview encoder** on for a browser/support preview. Encoder
settings under **Device** are device-wide: one source, format and ceiling are shared by
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

Changing the Outputs form creates a draft. **Apply to this browser** changes
the requested subscription; **Receiving** identifies actual arriving frames.
A pending camera selection must not label a still-running depth subscription
as camera output. Discard returns to the applied browser selection. Generating
connection code for an application starts no stream and does not change the
current browser's subscription. Run that code in your application to receive data.

Choose numeric depth plus validity for measurements. For a camera reference,
add the associated rectified image and read each product's grid metadata.
JPEG/H.264 picture delivery is an explicit viewing option. Uncompressed image
arrays preserve the supplied image values, but those images have already passed
through the ISP. Neither choice is sensor Bayer RAW.

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

## Ethernet, USB 2.0 and WLAN

All three links use the same HTTPS/WebSocket gateway and product names. USB here
means **USB Ethernet**, not a USB webcam or separate raw-image protocol. The
connection indicator describes the **camera's interface**. A computer connected
by Wi-Fi to a router can still reach the camera through Ethernet; the console
cannot establish the entire end-to-end route from its accepted socket.

Under **Outputs**, first choose the result and whether it is for **This browser**
or **My application**. Browser mode shows the connection already carrying the
console. Application mode adds **Connect my application to**:

| Connection | What it carries | What must be ready |
| --- | --- | --- |
| Ethernet · Pi Ethernet port | Selected images or numeric products; shared pictures are also supported | Camera Ethernet link and a reachable address |
| USB 2.0 · Pi USB-C port | The same selected products over the USB cable | Device-mode setup, compatible host, USB link and IP addressing |
| WLAN · Pi Wi-Fi connection | Selected products, with an explicit compressed-picture option to reduce picture bandwidth | Camera Wi-Fi connection and product permission |

These are the three physical delivery choices. USB is not an extra Ethernet
port: its current data protocol uses an ECM virtual network adapter over the
USB cable. Protocol and TLS details are collapsed under **Advanced connection
details** rather than used as the output names.

Each application choice shows the detected interface, readiness and available
addresses. If more than one usable address is reported, choose the address the
application can reach. **Show connection code** creates a recipe for that path;
it does not start an application or change the current browser stream. The SDK
uses the selected address while still verifying the device hostname, and checks
which camera interface it reached before requesting products. See the
[SDK connection example](sdk-examples.md#choose-a-device-connection).

USB-C status distinguishes controller/setup problems, an unplugged host and
missing IP addressing. A USB Ethernet adapter plugged into a host port is not
the Pi's USB-C output. The current Nuvo gadget uses ECM and does not assign
addresses automatically. Windows compatibility must be established for this
ECM implementation; do not assume the Windows support of Raspberry Pi's
separate ECM/RNDIS package applies. Raspberry Pi documents the Pi 5 USB-C device
connection and host requirements in its
[USB gadget guide](https://www.raspberrypi.com/news/usb-gadget-mode-in-raspberry-pi-os-ssh-over-usb/).

Selecting WLAN preserves numeric depth. **Use compressed picture instead** is
a separate choice that selects the shared JPEG/H.264 picture; it is not a
compressed float-depth array. The source and encoder state remain visible, and
enabling or changing the shared encoder requires an explicit device action.

The wired delivery preference is a separate device setting. Apply, confirm or
undo it explicitly. A preference does not reroute an existing TCP connection,
configure USB device mode or change the independent Wi-Fi permission. The
current browser connection label changes only when an actual new connection
uses that path.

**Allow product streaming over WLAN (Wi-Fi)** is a separate device-wide permission for
both compressed pictures and uncompressed products on the camera's Wi-Fi link.
Disabling it blocks those products while keeping console/control access and
permitted wired clients available. It does not disable the radio, configure an
SSID or move an existing connection to another interface. The advanced all-data
Off policy remains a master stop. Changing the chosen link requires reconnecting
to the appropriate reachable address.

Calibration positioning thumbnails remain available as part of the console
workflow. Their JPEGs are viewing aids, separate from the product stream.

Numeric products currently travel uncompressed. Reduce the requested rate or
select fewer products when a connection exceeds its budget. JPEG/H.264 picture
compression can discard image detail and is not an alternative encoding of the
original float depth array. No lossless numeric wire-compression mode is offered.
Changing matching resolution also changes geometry and processing; it is not a
network compression setting.

For pictures, Outputs provides **Smaller pictures**, **Balanced**, and
**More detail**, followed by **Apply shared picture quality**. With JPEG these
select quality 45, 70 and 90; with H.264 they select target bitrate 1, 4 and
8 Mbit/s. The summary names the actual codec/source and resulting setting.
They preserve the source, output dimensions, encoder rate ceiling, delivery
mode and numeric product selection. They affect all consumers of the shared
encoder, not just this browser or Wi-Fi clients. These settings are not measured
throughput guarantees. An unsaved advanced encoder draft must be saved or
discarded first, so a simple quality change cannot overwrite it silently.

## Safe changes

Encoder, stereo, and exposure changes apply live. Transport-policy changes,
including Wi-Fi permission, are provisional for 60 seconds and must be
confirmed or rolled back. The same console connection remains available when
Wi-Fi products are disabled. Calibration is not part of the
normal user flow. Trained operators use **Device → Calibration** for
the separate [advanced calibration process](advanced-calibration.md). Its
encoded view is only a positioning aid; synchronized unrectified ISP frames are
captured and solved locally on the Pi.
