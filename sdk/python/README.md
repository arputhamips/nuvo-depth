# Nuvo Depth Python SDK

[NuvoHardware](https://nuvohardware.com/) ·
[SDK user guide](https://docs.nuvohardware.com/vision-one/sdk/python) ·
[Install on your Pi](https://docs.nuvohardware.com/vision-one/install/byo-pi)

The dependency-light SDK reads associated NUV1 raw frame sets, optional NVE1
encoded monitoring frames, and device configuration through the authenticated
TLS gateway shared by Ethernet and approved USB Ethernet.

```bash
python -m pip install ./sdk/python
```

```python
from nuvo_depth import NuvoClient

camera = NuvoClient("https://YOUR-PI-HOSTNAME.local:7443", ca_file="device-ca.pem")
camera.claim("DEVICE-ADMIN-TOKEN")

print(camera.health())
frame = camera.latest()
print(frame.capture_sequence, frame.products.keys())

exposure = camera.exposure()
exposure.update(mode="manual", manual_exposure_us=4000,
                manual_analogue_gain=1.5)
camera.configure_exposure(exposure)

stereo = camera.stereo()
stereo.update(algorithm="sgbm", search_range=96, max_disparity=96,
              block_size=7)
camera.configure_stereo(stereo)

encoding = camera.encoding()
encoding["mode"] = "raw"       # "encoded" or "both" are also valid
camera.configure_encoding(encoding)
```

Configuration writes require a claimed session and automatically include its
CSRF token. Server validation is authoritative: submit the complete document
returned by the matching getter, changing only intended fields.

Use the device's actual hostname and verified certificate. `NuvoClient.claim()`
and `nuvo-record --claim-token` currently use the long administrative token,
which a trusted Pi administrator can obtain from
`/var/lib/nuvo-depth/identity/claim.token`. The short first-use setup code and
owner-password browser flow are documented in the
[setup guide](https://docs.nuvohardware.com/vision-one/guide/getting-started);
they are separate from this SDK method.

`camera.latest()` returns one raw frame snapshot. `camera.latest_encoded()`
returns an optional monitoring snapshot and reports an error when the encoder
is disabled. Raw image products may use `gray8`, `bgr8`, or `rgb8`; inspect the
product's `format` before interpreting its memoryview.

For continuous acquisition, use the persistent authenticated WebSocket stream:

```python
with camera.frames(cadence_hz=10, products=("depth", "confidence", "validity")) as frames:
    for frame in frames:
        print(frame.identity, frame.products["depth"].float32_le())
        # Break when finished; the context manager closes the connection.
```

Use `delivery="exact"` for an exact subscription, `delivery="best_effort"` for
an upper cadence bound, or `delivery="auto"` to follow the connection's policy.
The gateway rejects an exact request it cannot admit. Omit the options to use
the legacy six-product default subscription. Supported selections are
`left_rectified`, `right_rectified`, `disparity`, `depth`, `confidence`, `validity`,
`left_mono`, `right_mono`, `left_rgb`, `right_rgb`, `left_rectified_rgb`,
`right_rectified_rgb`, `rectified_stereo`, and `rectified_stereo_rgb`.
Mono and RGB products are unrectified ISP output; combined rectified stereo
products place left and right images side by side in each row, with twice the
single-camera width. Only requested products are computed and transported.
`frame.identity` includes device UUID, daemon run UUID, capture
epoch, and capture sequence, so a daemon restart cannot collide with the prior
run's capture counters. Legacy recordings that predate UUID metadata remain
readable and expose empty UUIDs.

The stream uses the claimed session cookie and the same certificate validation
as HTTPS requests, with no additional dependencies. It handles binary message
fragmentation, ping/pong, and close frames. It retains one bounded message at a
time, without a background queue. A normal peer close ends iteration; an idle
timeout, truncated frame, malformed message, or error close raises an exception.
The client's `timeout` is the maximum duration of the connection handshake or
one complete message, including fragments and control frames. Set it longer
than the expected inter-frame interval when requesting a low cadence. Optional
`frames.ping(b"check")` sends a heartbeat; iteration consumes its reply.

The recorder consumes the persistent stream, applies cadence on the gateway,
and skips consecutive duplicate capture identities:

```bash
nuvo-record https://YOUR-PI-HOSTNAME.local:7443 capture.nvr \
  --claim-token DEVICE-CLAIM-CODE --ca-file device-ca.pem \
  --frames 100 --cadence 10 --timeout 5 \
  --products depth,confidence,validity
```

Omit `--products` to record the legacy six products, and omit `--frames` to record until
interrupted or a normal peer close. The recorder closes the stream when its
frame count is reached. A disconnect or timeout leaves the completed records
readable and exits with an error; it does not silently reconnect across gaps.
A peer close before a requested finite frame count also exits with an error.
Use the NVR1 recording/replay helpers to read the resulting file.

Configuration reads return requested settings and retain a strong ETag. Read
before modifying; each `configure_*` method sends that revision with `If-Match`.
A concurrent update raises HTTP 412 instead of overwriting newer settings. You
can pass `expected_etag=` explicitly when preserving an older editing snapshot.
A successful write is HTTP 202 with a pending receipt, not proof of application:

```python
controls = camera.stereo()
controls["uniqueness_ratio"] = 3
camera.configure_stereo(controls)
receipt = camera.configuration_receipt("stereo")
print(receipt["state"], receipt["requested_revision"])
# Confirm on a later frame or health report:
if camera.health()["stereo_applied_revision"] == receipt["requested_revision"]:
    print("Matcher applied this revision")
```

`stereo_configuration_error` describes a rejected reload; the prior matcher and
configuration epoch remain active. The epoch advances only for an effective
matcher change, so formatting-only rewrites can have a new applied hash without
a new epoch. Exposure receipts describe saved commands; inspect actual left and
right camera metadata for the reported exposure and gain. Encoding receipts
remain pending until an output matching the requested settings is observed.
`frames.qualification` exposes the WebSocket admission label after opening:
`qualified-exact`, `best-effort`, or `engineering-best-effort` (or `unknown` on older gateways).
