# Build and test guide

`CMakeLists.txt`, `CMakePresets.json`, the source files under `cmake/`, and
`debian/` are the build definitions. Build directories are generated, ignored
by Git, and safe to recreate. Keep new CMake builds under `build/`; do not
create a new top-level `build-step*` directory for each experiment.

For device installation, use the [Pi installation guide](https://docs.nuvohardware.com/vision-one/install/byo-pi).
The commands below are for source development and package maintenance.

## Everyday development

Use Debian 13 Trixie or Ubuntu 24.04, including Ubuntu 24.04 under WSL2,
with CMake 3.25+, Ninja, and a C++20 compiler. Preset schema 6 requires 3.25.
Install host dependencies from the checkout before building:

```bash
bash tools/dev/bootstrap-host.sh --minimal
# Or, for development-full:
bash tools/dev/bootstrap-host.sh --full
```

This installs distribution packages including current security updates and saves
the resolved environment in `output/dev-environment/manifest.json`. It does not
install Pi camera backends or a Nuvo runtime package. For a disposable Debian
environment: `docker build -f tools/dev/Dockerfile -t nuvo-depth-dev .`, then
`docker run --rm -it -v "$PWD:/workspace" nuvo-depth-dev`. Run these commands in
Linux/WSL, not Windows PowerShell. Windows is also a supported Python SDK client;
host fixtures do not qualify physical Pi capture or networking.

Calibration solver/session fixtures require Python OpenCV and run in the required
development-full and ARM package suites. They are excluded from minimal-host,
which omits that capability; shared readiness/import/transaction tests still run.
This does not remove solver coverage from the required full CI job or add Python
OpenCV to the minimal runtime package.

Browser tools require Node 20+. When the distribution supplies an older Node,
the bootstrap installs official Node 22.22.1 using frozen x64/arm64 SHA-256
digests in `/opt/nuvo-host-node-22.22.1` and links its executables in
`/usr/local/bin`. Existing Node 20+ with npm is retained and its version recorded.
It avoids installing an obsolete distro Node/npm dependency tree first.

## Real gateway fixture and rendered tests

After the full build, launch the rootless fixture in one terminal:

```bash
python3 tools/dev/gateway_fixture.py --binary build/development-full/nuvo-gateway --manifest output/gateway-fixture/manifest.json --control output/gateway-fixture/control.json
```

Its manifest supplies a dynamically allocated HTTPS origin, public certificate,
fixture SPKI, and control-file path. Private test credentials stay in a temporary
private directory and are removed at exit. Output is labeled TEST FIXTURE;
synthetic identities and readiness are not camera/calibration evidence. Stop
with Ctrl-C. The control JSON selects readiness state, run and gateway restart.

In another terminal, use the pinned browser package:

```bash
npm --prefix tools/dev/browser ci
npm --prefix tools/dev/browser exec -- playwright install --with-deps chromium
export NUVO_PLAYWRIGHT_MODULE="$PWD/tools/dev/browser/node_modules/playwright"
node tests/first_use_browser_test.cjs
node tests/gateway_browser_test.cjs output/gateway-fixture/manifest.json
python3 tests/sdk_tls_test.py
```

The browser's certificate exception is scoped to the generated fixture SPKI.
SDK tests independently verify correct trust, unknown trust, hostname mismatch
and certificate replacement. Product examples always validate TLS. Screenshots,
traces and results are under `output/first-use-browser` and `output/gateway-browser`.
Fresh CI jobs run minimal/full builds and Linux/Windows Python 3.11/3.13 clients;
the `required` job rejects missing required jobs. Documentation changes route
to lightweight consistency checks. ARM64 packages/signing remain manual.

The minimal preset excludes optional native dependencies. The full development
preset additionally needs yaml-cpp, OpenCV 4, FFmpeg development libraries,
Boost headers (1.74+), and OpenSSL 3. Boost.System is used in its header-only form,
including distributions which no longer provide a separate Boost.System CMake
package. `DEPTHD_WITH_PI5_CAPTURE` stays off for host builds.

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

The stability regressions run through the existing CTest jobs. Exposure tests
simulate unchanged HDR scenes, clipping/quantization boundaries, lighting steps,
transients, gradual drift and delayed/missing applied controls. Runtime tests
cover independent versus associated images, pool limits and publication/cache
ownership. CPU rectification tests compare direct CPU and staged DMA pixels and
reject overlapping buffers. Encoder tests cover full-sensor downscaling,
direct-I420 color, demand changes, failed optional capture copies and recovery.

`depthd_web_ui_static_test` also runs frame-parser and stream-lifecycle JavaScript
regressions. `depthd_python_sdk_test` covers selected products, fragmented receives,
bounded immutable decoding and metadata compatibility. The separate rendered
gateway journey checks task presets, actual product grids, interrupted streams,
gateway restarts and desktop/mobile layout. Keep a regression at the relevant
boundary when changing one of these paths; the [source map](native-runtime-ipc-gateway.md#source-map)
identifies implementation and test entry points.

## Pi 5 production and packages

Use the `pi5-release` preset on the intended 64-bit Pi/Linux build environment,
with the native libcamera, EGL/GLES, libsystemd, OpenCV, yaml-cpp, FFmpeg,
Boost headers, and OpenSSL development dependencies installed. The production
option requires every backend used by the depth-only appliance.

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
