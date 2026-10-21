# Pi 5 operations and troubleshooting

For first-use setup and network/connection recovery, start with the
[Vision One setup guide](https://docs.nuvohardware.com/vision-one/guide/getting-started).

## Fast health check

```bash
systemctl is-active nuvo-depthd-native.service nuvo-gateway.service \
  nuvo-system-agent.socket
curl --cacert device-ca.pem https://YOUR-PI-HOSTNAME.local:7443/api/v1/health
```

The camera service is `nuvo-depthd-native.service`. The similarly named
`nuvo-depthd.service` belonged to the removed Python pipeline; current packages
do not install it. Transport, calibration and verified-asset rollback remain
independent of that retired pipeline.

A healthy live response has `status: running`, an advancing
`capture_sequence`, 728 × 544 image and depth dimensions,
`rectification_backend: egl_gl_dmabuf`, the selected OpenCV stereo backend,
zero trigger/requeue errors, and matched left/right reported exposure values.
The Diagnostics page copies the same credential-free JSON.

## No browser image

1. Check Overview health and that capture sequence advances.
2. For Live viewer, enable **Raw product delivery**.
3. For Streaming preview, enable **Monitoring encoder** and wait for the next
   image. Live H.264 requires keyframe interval 1 so every image is independent.
4. If settings appear stale, reload the page and verify
   `GET /api/v1/encoding`; configuration is device authority, not browser state.
5. Inspect `journalctl -u nuvo-gateway.service -u nuvo-depthd-native.service`.

The encoder endpoint intentionally returns HTTP 503 when monitoring encoding is
off. The raw endpoint intentionally returns HTTP 503 in encoded-only mode.

## Flicker isolation

Use the 60-frame trace tool described in
[the full-resolution flicker report](pi5-fullres-stereo-flicker-trace.md). It
captures aligned samples at these boundaries:

1. ISP luma before rectification;
2. left/right rectified images;
3. disparity/depth after matching;
4. the encoder input/output envelope;
5. the frame received by the remote PC.

If mean luma alternates at boundary 1, do not tune the encoder or decoder. Keep
manual exposure/gain fixed and inspect the illumination frequency, XTR pulse,
sensor mode, and electrical timing. If boundary 1 is stable but boundary 2 is
not, inspect the EGL rectifier/fallback. If only encoded/remote output fails,
check stream sequence/keyframe continuity and stale-frame rejection.

## Exposure change does not apply

- Confirm the UI reports requested values and both cameras' reported metadata.
- Read `/etc/nuvo-depth/exposure.json`; malformed, unknown, or out-of-range
  fields are rejected and the last valid command remains active.
- Verify `nuvo-system-agent.socket` is active. Web writes go through its fixed
  allowlist; the gateway does not run as root.
- Manual exposure also changes the external trigger pulse duration. Values over
  8,000 µs are rejected for this hardware profile.

## Performance or heat

Use two OpenCV threads with the default 728 × 544, 128-disparity, 3-way
SGBM profile and active cooling. Full-resolution SGBM is the dominant cost.
Cached CPU remapping is the default rectifier after measurement on the Pi;
EGL/GLES remains selectable. The
Pi 5 has no general OpenCV CUDA/OpenCL path in this product. Turning off the
monitoring encoder recovers CPU headroom, but it cannot make the matcher itself
faster. StereoBM or a narrower disparity search is the supported speed/quality
tradeoff.

```bash
top -H -p "$(pidof depthd)"
vcgencmd measure_temp
vcgencmd get_throttled
```

Do not use an unqualified overclock as a release fix. A nonzero throttle flag or
sustained temperature near the platform limit invalidates performance results.

## Recovery

```bash
sudo systemctl restart nuvo-system-agent.socket \
  nuvo-depthd-native.service nuvo-gateway.service
sudo systemctl --no-pager --full status nuvo-depthd-native.service \
  nuvo-gateway.service
```

Configuration updates are atomic. If a newly selected connection path fails,
use the Connections page rollback within 60 seconds. Package rollback must use
the previous signed Debian/APT cohort; do not overwrite individual files on a
customer system. Per-unit identity and calibration live under
`/var/lib/nuvo-depth` and are preserved by package removal.
