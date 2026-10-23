# Runnable application examples

Use the full appliance for HTTPS/SDK delivery. Minimal provides local raw output
and has no gateway or network SDK. Start with the [verified-TLS quickstart](https://docs.nuvohardware.com/vision-one/sdk/python).
Browser connections are automatic; SDK clients still exchange the long device
administrator token for a session. Read it over trusted SSH and use the hidden
prompt. Never place it in source, command history, screenshots or support reports.

The core Python SDK and examples require Python 3.11+ and the standard library.
The candidate CI matrix covers Linux and Windows with Python 3.11 and 3.13.
NumPy and Python OpenCV are optional application choices, not SDK requirements.
The six-command `nuvo_depth.examples` module is available in the current source
SDK and beta 8 candidate. The public beta 6 SDK does not contain this module;
use the standalone helper in the verified-TLS quickstart for that SDK.

To try these six commands with a beta 6 gateway, obtain the
[current client source archive](https://github.com/arputhamips/nuvo-depth/archive/refs/heads/main.tar.gz)
and extract it. These client examples were exercised against beta 6 as well as
the candidate gateway. From the archive root, install its SDK in a fresh virtual
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
SAN or a local hosts entry mapping the certificate hostname to the verified IP;
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

The small real recording in `tests/fixtures/recordings/beta6-preview.nvr.gz`
has numeric depth/validity only. Its adjacent JSON supplies license, provenance,
original identity, checksums and exact expected dimensions/validity count. Expand
with Python `gzip.decompress` to a new `.nvr`, then use every example with
`--input FILE` instead of `--url` and `--ca-file`. One record is available. This
fixture tests older recordings, not candidate geometry or metric quality.

| Combination | Contract |
| --- | --- |
| beta 8 client / candidate full appliance | NUV1/NVE1 v1, NVR1, current API; exact candidate results belong in acceptance evidence |
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
