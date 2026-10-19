# Build and test guide

`CMakeLists.txt`, `CMakePresets.json`, the source files under `cmake/`, and
`debian/` are the build definitions. Build directories are generated, ignored
by Git, and safe to recreate. Keep new CMake builds under `build/`; do not
create a new top-level `build-step*` directory for each experiment.

For device installation, use the [Pi installation guide](https://docs.nuvohardware.com/vision-one/install/byo-pi).
The commands below are for source development and package maintenance.

## Everyday development

Use a Linux or WSL environment with CMake 3.20+, Ninja, and a C++20 compiler.
The minimal preset excludes optional native dependencies. The full development
preset additionally needs yaml-cpp, OpenCV 4, FFmpeg development libraries,
Boost.System, and OpenSSL 3. `DEPTHD_WITH_PI5_CAPTURE` stays off for host builds.

```bash
cmake --preset minimal-host
cmake --build --preset minimal-host -j
ctest --preset minimal-host

cmake --preset development-full
cmake --build --preset development-full -j
ctest --preset development-full
```

For a focused test, run `ctest --test-dir build/development-full -R PATTERN
--output-on-failure`. Build outputs remain in the ignored `build/` tree.

## Pi 5 production and packages

Use the `pi5-release` preset on the intended 64-bit Pi/Linux build environment,
with the native libcamera, EGL/GLES, libsystemd, OpenCV, yaml-cpp, FFmpeg,
Boost.System, and OpenSSL development dependencies installed. The production
option deliberately rejects incomplete or semantic/Hailo configurations.

```bash
cmake --preset pi5-release
cmake --build --preset pi5-release -j
ctest --preset pi5-release
```

The historical `build-step3-missing-libcamera.log` showed CMake failing at
`pkg_check_modules(libcamera)` because the host lacked `libcamera`. This is a
missing build dependency, not a source file to copy into the repository. The
old `build-step3-libcamera-deps/` tree was an unpacked Ubuntu 0.2.0 package
snapshot used for a host API check; use the native distro dependency on the
target instead. A host build with capture disabled cannot validate camera or
hardware-in-the-loop behavior.

For Debian packages, use the declared package build dependencies and the
release procedure in [deployment/README.md](../deployment/README.md):

```bash
sudo apt build-dep ./
dpkg-buildpackage --no-sign --build=binary
```

Do not treat a successful compilation or test run as Pi hardware qualification;
use the qualification plan and hardware handoff documentation for that gate.

## What the removed top-level build folders represented

These were disposable snapshots from the September 7–8, 2026 development
sequence, not separate build definitions:

| Former folder(s) | Purpose; current replacement |
| --- | --- |
| `build-step3-minimal` | Dependency-light host build; use `minimal-host`. |
| `build-step3-debug`, `build-step3-release`, `build-step3-sanitize` | Host debug, release, and sanitizer experiments; use a preset or a new subdirectory of `build/` with explicit CMake flags. |
| `build-step3-missing-libcamera`, `build-step3-pi-api`, `build-step3-pi-api2`, `build-step3-libcamera-deps` | libcamera absence and temporary host/sysroot API checks; install the native dependency for a real Pi build. |
| `build-step3-install`, `build-step6-install-audit`, `build-final-install`, `build-release-staging-20260908` | Copies staged by `cmake --install` or packaging checks; regenerate from the source and packaging rules. |
| `build-step6-package-audit` | Generated Debian package and CPack staging; rebuild from `debian/`. |
| `build-gateway`, `build-remaining` | Intermediate host builds as gateway and runtime features were added; use `development-full`. |

The old binaries, object files, CMake caches, test logs, generated effective
configuration JSON, downloaded `.deb` files, and staged install trees are not
needed for future development. The canonical `build/` folder and source
`cmake/` folder are intentionally retained.
