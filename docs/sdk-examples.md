# Runnable application examples

Use the full appliance for HTTPS/SDK delivery. Minimal provides local raw output
and has no gateway or network SDK. Start with the [verified-TLS quickstart](https://docs.nuvohardware.com/vision-one/sdk/python).
Browser connections are automatic; SDK clients still exchange the long device
administrator token for a session. Read it over trusted SSH and use the hidden
prompt. Never place it in source, command history, screenshots or support reports.

In the development console, **Outputs** prepares a recipe for
your own client. Showing that recipe does not start an application stream or
change the browser's active subscription. Use numeric **depth + validity** for
measurements. A shared JPEG/H.264 picture preview is a viewing product and cannot
recover the original numeric depth values; an ISP image is not sensor Bayer RAW.

Wi-Fi, Ethernet and USB Ethernet use the same API. The device's independent
WLAN permission applies only to raw/numeric products. Turning it off blocks
uncompressed data on that camera interface while encoded streams and browser
previews remain available. Master Off and invalid device policies still block
all products. The permission does not select an interface for
your client; reconnect to an appropriate reachable address to change links.
The gateway's connection description identifies the camera-side interface,
not every network segment between the camera and the application.

### Choose a device connection

In **Outputs**, choose **Ethernet** (Pi Ethernet port),
**USB 2.0** (Pi USB-C port), or **WLAN** (Pi Wi-Fi connection).
The console reports detected interfaces and addresses, and explains
missing setup or permission. A ready camera interface does not prove that the
application computer can reach it. Generated code verifies the selected
camera-side path, interface and address before requesting products.

The development SDK accepts `connect_address` to dial the chosen numeric IP
while retaining the certificate hostname in the URL. It uses the same address
for HTTPS snapshots, control requests and WebSocket frames. This avoids relying
on a multi-interface hostname resolving to the preferred port. The default
client remains compatible; install the SDK from this build to use the new
argument.

```python
from getpass import getpass
from nuvo_depth import NuvoClient

with NuvoClient("https://YOUR-PI-HOSTNAME.local:7443", ca_file="device.crt",
                connect_address="192.0.2.10") as client:  # Replace with the reported camera IP.
    client.claim(getpass("Device administrator token: "))
    connection = client.capabilities()["connection"]
    if connection.get("path") != "ethernet" or connection.get("local_address") != "192.0.2.10":
        raise RuntimeError("The selected camera connection was not reached")
    with client.frames(products=("depth", "validity"), cadence_hz=2,
                       delivery="best_effort") as frames:
        for frame in frames:
            print(frame.capture_sequence, frame.products.keys())
```

The numeric address must be reachable from the application. Direct-address
clients bypass HTTP proxy environment settings and reject redirects; they do
not fall back to another interface. Certificate trust, hostname verification,
session authentication and product permissions still apply. For IPv6, use an
unscoped literal only when the gateway reports that it listens on that address.

Choosing WLAN does not compress numeric arrays. Select encoded images alone,
numeric/raw products alone, or both together. The shared encoder must be enabled
for picture delivery; it does work only while an encoded consumer is present.
The USB 2.0 choice uses the same API over an ECM virtual network adapter carried
by the USB-C cable. These protocol details are under **Advanced connection
details** in the console. Gadget configuration alone does not assign addresses
or guarantee host-driver support.

The core Python SDK and examples require Python 3.11+ and the standard library.
The candidate CI matrix covers Linux and Windows with Python 3.11 and 3.13.
NumPy and Python OpenCV are optional application choices, not SDK requirements.
The `nuvo_depth.examples` module is available in the current source SDK.
Its original six commands are also in public beta 8; the new `preview` and
`paired` commands require the updated SDK from this build. The older beta 6 SDK does not contain this module;
use the standalone helper in the verified-TLS quickstart for that SDK.

To try the original six commands with a beta 6 gateway, obtain the
[current client source archive](https://github.com/arputhamips/nuvo-depth/archive/refs/heads/main.tar.gz)
and extract it. These client examples were exercised against beta 6 as well as
the beta 8 gateway. From the archive root, install its SDK in a fresh virtual
environment. This changes the application client; it does not upgrade the Pi:

```bash
python -m venv .venv
# Linux: source .venv/bin/activate
# Windows PowerShell: .venv\Scripts\Activate.ps1
python -m pip install ./sdk/python
python -c "import nuvo_depth; print(nuvo_depth.__version__)"
```

Acquire the unit's public `device.crt` from
`/var/lib/nuvo-depth/identity/device.crt` over authenticated SSH. Compare its SHA-256
fingerprint against trusted setup material on that same unit. Use its actual
certificate hostname in the URL. An IP fallback requires a matching certificate
SAN, `connect_address` with the original certificate hostname, or a local hosts
entry mapping the certificate hostname to the verified IP;
changing the URL to an arbitrary IP does not authorize a hostname mismatch.
If a certificate changes unexpectedly, re-establish identity before replacing
trust. Do not disable certificate or hostname validation.

```bash
python -m nuvo_depth.examples single --url https://YOUR-PI-HOSTNAME.local:7443 --ca-file device-ca.pem --output sample.nuv
python -m nuvo_depth.examples pixel --url https://YOUR-PI-HOSTNAME.local:7443 --ca-file device-ca.pem --x 200 --y 150
python -m nuvo_depth.examples display --url https://YOUR-PI-HOSTNAME.local:7443 --ca-file device-ca.pem --output depth.png
python -m nuvo_depth.examples stream --url https://YOUR-PI-HOSTNAME.local:7443 --ca-file device-ca.pem --frames 10
python -m nuvo_depth.examples record --url https://YOUR-PI-HOSTNAME.local:7443 --ca-file device-ca.pem --frames 10 --output sample.nvr
python -m nuvo_depth.examples replay --input sample.nvr --frames 10
```

Each live invocation prompts for the token, selects only depth plus validity at
1 Hz best effort, and closes its subscription and SDK session on exit. Existing output files
are refused. A finite live shortfall, timeout or disconnect fails explicitly;
completed NVR records remain readable. There is no automatic reconnect or gap
filling. Replay always identifies saved data and does not establish live freshness.

The updated SDK also supplies live compressed-image and associated depth/image
examples. `preview` requests up to 5 frames/s; `paired` requests depth + validity
and a matching encoded picture at up to 2 frames/s. Both close their stream and
session on exit. The paired example requires the updated gateway.

```bash
python -m nuvo_depth.examples preview --url https://YOUR-PI-HOSTNAME.local:7443 --ca-file device-ca.pem --frames 10
python -m nuvo_depth.examples paired --url https://YOUR-PI-HOSTNAME.local:7443 --ca-file device-ca.pem --frames 10
```

For application code, reuse one `NuvoClient` and close its frame contexts before
`client.close()`, or nest them inside `with NuvoClient(...) as client:`. The beta 8
candidate releases the caller's session without affecting other clients. The
older beta 6 gateway lacks session revocation: repeated short commands can fill
its finite twelve-hour pool. A session-capacity error requires closing unused
clients or, on that older gateway, restarting `nuvo-gateway` over trusted SSH
when interrupting gateway connections is acceptable. It does not mean the cameras
or calibration failed.

Depth is row-major little-endian float32 optical-axis Z in metres. Consume a
pixel only when its associated uint8 validity is `1` and Z is finite and positive;
NaN is not zero. Inspect each product's dimensions and format. Image products
may be gray8, bgr8 or rgb8. Keep device/run/capture identities together. Authority
0 is provisional preview, 1 user validated, 2 factory validated; neither declared
validation label alone proves metric accuracy. The PNG is a visualization:
black denotes invalid pixels and grey maps the chosen near/far range. It changes
no recorded data or calibration.

Both snapshot and streaming APIs accept the same selection:

```python
frame = client.latest(products=("depth", "validity"), cadence_hz=2,
                      delivery="best_effort")
with client.frames(products=("depth", "validity"), cadence_hz=2,
                   delivery="best_effort") as frames:
    for frame in frames:
        depth = frame.products["depth"]
        validity = frame.products["validity"]
        print(frame.identity, depth.width, depth.height)
        # Consume finite positive depth only where validity.data[index] == 1.
        break
```

Omitting every option preserves the legacy six-product subscription. Supplying
only products uses 10 Hz; supplying only cadence uses those legacy products.
Prefer an explicit small selection and rate. The SDK fills both query fields so
partial selections remain valid gateway requests. A selected frame holds
immutable wire bytes and product memoryviews share that storage. Receiving a
network message still requires a bounded ownership copy; this is not a promise
of end-to-end zero-copy. Fragmented payloads are received directly into their
accumulator without creating an immutable copy of every fragment.

`confidence` is binary matcher support (`0` / `255`), not a probability.
`disparity` is in matching-grid pixels, which can differ from image pixels.
Products are keyed by name; only requested products are included. Stereo
side-by-side outputs contain left then right horizontally. `float32_le()` gives
a zero-copy float view on little-endian hosts; on a big-endian host use an
explicit little-endian decoder such as NumPy `dtype="<f4"` or `struct.unpack`.

New NUV1 frames add optional `image_grid` and `matching_grid` mappings,
`disparity_metadata` (ROI, quantization, accepted range, confidence model and
derivation), `depth_metadata`, and `calibration_geometry`. Geometry contains
`image` and `matching` projection pairs (12 row-major values each), reprojection
matrices (16 row-major values) and baseline in metres. Grid mappings use
`x_scale_to_reference`, `y_scale_to_reference`, `x_offset_to_reference` and
`y_offset_to_reference`. Use the geometry for the selected grid and retain its
calibration hash and authority; older recordings without these fields need
their original calibration separately.

The optional `capture` object carries `sensor_delta_ns` and `left`/`right`
sensor timestamp, stream sequence, `reported_exposure_time_us`, analogue gain
and digital gain. Reported exposure is sensor metadata, not physical trigger
pulse measurement. Frame capture time is the stereo timestamp midpoint in the
device boot-time clock, not UTC. Compressed previews have independent cadence;
normalize UUID spelling and match complete device/run/capture identity instead
of joining the most recently received messages.

`latest_encoded()` returns one compressed frame; `encoded_frames(cadence_hz=5)`
opens a persistent encoded stream and releases its demand when closed. These
frames contain original JPEG or H.264 bytes and capture identity metadata.
Encoded depth is mapped
to 8-bit intensity over 0.2–12.2 m before compression; it is unsuitable for
numeric measurement. Device encoder settings affect all preview consumers,
while each data subscription selects its own products and cadence. The
gateway's uncompressed-network toggle does not disable local socket delivery.

### Numeric depth alongside encoded images

An application can request both with the browser closed. The two representations
remain separate: depth/validity retain original numeric values, while the encoded
image can be passed to a robot's image decoder or stored without re-encoding.

```python
from getpass import getpass
from nuvo_depth import NuvoClient

with NuvoClient("https://YOUR-PI-HOSTNAME.local:7443", ca_file="device.crt",
                connect_address="192.0.2.10", timeout=10) as camera:
    camera.claim(getpass("Device administrator token: "))
    with camera.paired_frames(products=("depth", "validity"), cadence_hz=2,
                              delivery="best_effort") as pairs:
        for frame, picture in pairs:
            assert frame.identity == picture.identity
            depth = frame.products["depth"].float32_le()
            validity = frame.products["validity"].data
            encoded_image = picture.data
            print(frame.capture_sequence, picture.codec, len(encoded_image))
```

Enable WLAN raw/numeric permission before using this on the camera's WLAN
address. Pairing opens one associated subscription and emits only matching
device/run/capture epoch/capture sequence identities. The gateway retains bounded
association history, discards unmatched captures and sends both representations
together. The SDK times out if no pair arrives within the client timeout;
it never associates by arrival order. Both outputs use the same requested rate,
as a best-effort ceiling rather than a guarantee that every capture is delivered.
Use independent `frames()` and
`encoded_frames()` streams if the application does not need matched captures.
Closing or failing the paired stream releases both kinds of device demand. The
shared encoder continues for other consumers, or stops when none remain.

This requires an updated gateway advertising `paired_stream` / `NVP1` support.
Older gateways fail with an explicit unsupported message; the SDK does not
silently substitute unrelated captures. `delivery="auto"` uses best effort for
pairs, and `delivery="exact"` is not supported for paired output.
Concurrent paired subscriptions must use the same processing lane: image-only,
or a selection containing numeric/matcher products. Include camera images in
the numeric selection when needed. Mixing these two kinds of paired stream
returns 409; independent raw or encoded streams remain available.

At 728 × 544, depth + validity costs about 1.98 MB per capture. Start WLAN clients
at 1–2 frames/s, then include variable encoded-image traffic and overhead in the
link budget. Pixel-grid sizes and calibration identity can differ between
numeric and encoded products; capture association alone does not align pixels.

The small real recording in `tests/fixtures/recordings/beta6-preview.nvr.gz`
has numeric depth/validity only. Its adjacent JSON supplies license, provenance,
original identity, checksums and exact expected dimensions/validity count. Expand
with Python `gzip.decompress` to a new `.nvr`, then use the original six examples with
`--input FILE` instead of `--url` and `--ca-file`. One record is available. This
fixture tests older recordings, not candidate geometry or metric quality.

| Combination | Contract |
| --- | --- |
| beta 8 client / public full appliance | NUV1/NVE1 v1, NVR1, current API; exact candidate results belong in acceptance evidence |
| beta 8 client / saved beta 6 NVR1 | Original identity, authority, masks and payload preserved; registered compatibility test |
| Older v1 recordings without UUIDs | Remain readable; UUID properties are empty, not invented |
| Minimal appliance | Local `nuvoctl depth` only; no HTTPS endpoint |

HTTP 401/403 requires new SDK credentials/session; 412 requires reloading current
configuration before resubmission; 422/429 requires reducing products/cadence or
reviewing admission. HTTP 503 requires checking raw/monitoring mode, readiness,
services and any pending reboot. Exact requests are rejected when they cannot
be admitted; they are not silently degraded. Keep the TLS checks enabled.

## Installed C interface

Install the candidate `libnuvo-depth-dev` package with its matching runtime
library, a C compiler and pkg-config. Copy the installed example to a writable
directory and compile without private repository headers:

```bash
cp /usr/share/doc/libnuvo-depth-dev/examples/decode_frame.c .
cc -std=c11 -Wall -Wextra decode_frame.c $(pkg-config --cflags --libs nuvo-depth) -o decode_frame
./decode_frame sample.nuv
```

The public ABI header and pkg-config file define the supported interface. The
example bounds input, decodes one saved NUV1 packet and prints its original
metadata; it is not a network client. See [SDK reference](../sdk/python/README.md)
for frame ownership and streaming. ROS 2 remains an integration project; no
production ROS 2 bridge is shipped or implied by this example.
