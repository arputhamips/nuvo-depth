# Production deployment

Depth Daemon is a native, systemd-managed hardware service. The ROS container
does not install, update, or own its binaries, camera calibration, Hailo HEFs,
labels, configuration, or runtime state. ROS receives only the read-only mmap
products in `/run/depthd`.

## Canonical paths

- Executable support files and native libraries: `/usr/lib/nuvo-depthd`
- Asset management command: `/usr/sbin/depthd-assets`
- Package-owned provisioning seed: `/usr/share/nuvo-depthd/runtime-seed`
- Immutable data releases: `/var/lib/nuvo-depthd/releases/<release-id>`
- Active/rollback pointers: `/var/lib/nuvo-depthd/current` and `previous`
- Daemon-owned generated output: `/var/lib/nuvo-depthd/output`
- Ephemeral ROS boundary rings: `/run/depthd`

The Git repository deliberately ignores `*.hef`. Model binaries are release
artifacts, not source files and never belong in NuvoBase or its ROS image.

## Build and install the host package

Build on a compatible arm64 Pi image with the HailoRT development package,
OpenCV, `libegl-dev`, and `libgles-dev` installed:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DDEPTHD_WITH_HAILORT_SEMANTICS=ON \
  -DDEPTHD_WITH_EGL_GL_DMABUF_RECTIFICATION=ON \
  -DDEPTHD_PACKAGE_REVISION=$(git rev-parse --short=12 HEAD) \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j
ctest --test-dir build --output-on-failure
cpack --config build/CPackConfig.cmake -G DEB
sudo apt install ./build/nuvo-depthd_*.deb
sudo systemd-tmpfiles --create nuvo-depthd.tmpfiles.conf
```

The Debian package declares the system Python camera/image dependencies plus
EGL/GLES, HailoRT 4.23, and its PCIe driver. A revisioned production package
fails configuration if the EGL/GLES backend is omitted. Build release packages
from a clean, tagged Depth Daemon checkout so the source revision remains
reproducible.

GPU rectification is the selected production default. CPU rollback is one
systemd environment override and does not require rebuilding or removing the
GPU library:

```bash
sudo systemctl edit nuvo-depthd.service
```

```ini
[Service]
Environment=DEPTHD_RECTIFICATION_BACKEND=cpu_opencv
```

Then run `sudo systemctl daemon-reload && sudo systemctl restart
nuvo-depthd.service`. Remove the drop-in to return to `egl_gl_dmabuf`. Runtime
GPU initialization/import failures also fail over permanently to the same CPU
backend for that daemon process.

The package does not contain HEFs. Installing or upgrading it cannot silently
replace the active model/calibration release.

## Provision a new Pi

Obtain the external model bundle from the controlled Depth Daemon release or,
for the current migration only, copy the checksum-verified workstation seed to
the Pi. Its directory must contain `models/hailo/front_semantic.hef`,
`segformer_b0_bn.hef`, and `coco80.labels`.

Verify all package and external inputs before changing `/var/lib`:

```bash
sudo depthd-assets verify-source --artifact-source /path/to/seed
```

Install the immutable release and atomically activate it:

```bash
sudo depthd-assets install --artifact-source /path/to/seed
sudo depthd-assets preflight
sudo systemctl enable --now nuvo-depthd.service
```

`install` first verifies every byte and SHA-256, copies into a temporary
release, verifies the copy, renames it atomically, and only then updates
`current`. An existing release is never overwritten. Re-running the command is
safe when the existing release is intact and fails closed if it differs.

After the daemon creates both rings and the ROS bridge validates its topics,
the old `/home/cam/nuvo-base/data` copies may be archived manually. The
installer never removes them.

## Upgrade and rollback

Install a new signed/versioned host package and a new asset manifest, then run
the same `depthd-assets install` command with that release's external artifact
bundle. Activation preserves the former target in `previous`; all releases are
retained.

To roll back the complete model/calibration/profile set:

```bash
sudo systemctl stop nuvo-depthd.service
sudo depthd-assets rollback
sudo depthd-assets preflight
sudo systemctl start nuvo-depthd.service
```

Rollback swaps `current` and `previous`; it does not copy, edit, or delete any
payload. Roll back the Debian package separately with `apt install` if native
code compatibility also changed.

The production unit keeps the frozen CPU SGBM and exposure arguments explicit.
It verifies the active data release before opening cameras or Hailo and refuses
startup on missing/tampered data.
