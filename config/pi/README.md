# Raspberry Pi camera trigger setup

The two InnoMaker IMX296 cameras share BCM GPIO17 (physical header pin 11).
Their isolated external-trigger inputs idle high; a falling edge begins exposure,
and exposure lasts for the low pulse width plus approximately 14.26 us.

Install `depthd-imx296-trigger.conf` as root:

```bash
sudo install -m 0644 config/pi/depthd-imx296-trigger.conf \
  /etc/modprobe.d/depthd-imx296-trigger.conf
```

Disable camera auto-detection and configure both overlays in
`/boot/firmware/config.txt`:

```ini
camera_auto_detect=0
dtoverlay=imx296,always-on,cam0
dtoverlay=imx296,always-on,cam1
```

After reboot, `/sys/module/imx296/parameters/trigger_mode` must contain `1`.
Run `tools/pi/audit_imx296_trigger.sh` to inspect the configuration, then run
`tools/pi/verify_external_trigger.py` as root. The verifier first confirms that
no frames arrive without a pulse, sends five startup pulses for libcamera's
settling frames, and then requires one additional pulse to produce exactly one
new frame on each camera.

Do not restore the ROS workspace's former active-high 1 ms waveform. It never
enabled the driver's external-trigger mode and its 50 ms timestamp tolerance
masked free-running cameras as synchronized.
