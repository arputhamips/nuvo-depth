# Retired editor, legacy pipeline and demo assets — 2026-10-21

This cleanup removes the parked browser editor, the parallel Python processing
stack and its private output libraries, and photographic demo substitutes.
The native capture/rectification/matcher workers, raw and encoded delivery,
shared browser settings/codec, calibration, administration, setup, SDKs and
recording/replay remain supported.

The starting revision is `54d67dba12b7a1f48ff4ede8b6fe6db7020679b9`.
The changes were made in the requested order:

1. Removed five parked editor files, seven dedicated UI workspace/build files
   and its model test. Current browser tests now exercise shared settings,
   selection admission, rendering and matcher-rate reporting directly. Old
   prototype routes remain rejected even when stale files survive an upgrade.
2. Removed the inactive Python service/tmpfiles rule, five processing/wrapper
   scripts, their private C/C++ wrappers/result-ring/obstacle-cloud code, two
   standalone tools and eight dedicated tests. The native EGL engine, native
   EGL backend and CPU fallback tests remain. The historical native executable
   at `/usr/lib/nuvo-depthd/depthd` remains installed alongside `/usr/bin/depthd`.
3. Removed retired ROS/ring/cloud storage, normalization and diagnostics from
   the effective configuration. Old schema-v1 seed bytes still load unchanged:
   known retired output keys are optional and ignored at import. They no longer
   affect the native configuration digest. Unknown/duplicate keys and active
   calibration, geometry, trigger, capture and matcher checks still reject bad
   configurations. The live performance collector now uses `matched_frames`
   and unique nested `matcher_statistics.capture_sequence`, rather than treating
   every mono publication as new depth. Quality/grid fields are retained only
   when `processing_statistics_capture_sequence` identifies that matcher result.
4. Removed four DIODE JPEGs, gallery metadata, UI handlers/styles, install rules
   and static routes. Waiting, disabled-output and disconnected states replace
   photographs. The live canvas layout and controls remain. Stale demo URLs
   are covered by explicit 404 integration checks.

**Inventory.** Source counts use the same audited extensions before and after:
`.c`, `.h`, `.cc`, `.cpp`, `.hpp`, `.py`, `.js`, `.mjs`, `.jsx`, `.ts`, `.tsx`,
`.css`, `.html`, `.sh`, `.proto`. Physical lines include blank lines; nonblank
lines are counted separately. CMake is counted separately. Counts include the
new collector regression test and exclude generated/ignored build outputs.

| Measure | Before | After | Reduction |
|---|---:|---:|---:|
| Source files | 207 | 172 | 35 |
| Physical source lines | 55,242 | 48,016 | 7,226 |
| Nonblank source lines | 51,290 | 44,608 | 6,682 |
| CMake physical lines | 1,271 | 1,091 | 180 |

There are 47 deleted tracked files, including 36 source files containing 7,162
physical / 6,606 nonblank lines. The net totals also include edits to surviving
files and the new test. The four JPEGs account for 1,064,818 removed bytes;
the editor's lockfile and other non-source metadata are not included in source
line totals. This is a maintenance/build/asset reduction, with no new FPS claim.

**Compatibility and protected boundaries.** All 43 installer, release, setup,
administration, calibration/transport/asset rollback and seed-input files checked
against the starting revision remain byte-identical. The three seed files still
match their existing manifest sizes and SHA256 hashes. The removed service was
not part of the native appliance target; no actual installer, transport,
calibration or asset rollback path invokes it. Its old backend-switch comment
did not make the retired processing stack a current rollback dependency.

Current camera diagnostic helpers remain installed in the native libexec
directory. Historical Python pipeline/ring/cloud benchmark documents are
retained and labelled historical. `protocol/nuvo_stream_v1.proto` remains an
explicitly unused reference: the shipping wire metadata is JSON, and no build
target consumes that schema. Frozen YAML fixtures and the protected Debian
exclusion list retain legacy field/path names for compatibility; they do not
create runtime processing dependencies.

**Validation.** Final validation passed on Ubuntu 26.04.1 with GCC 15.2,
OpenCV 4.10, FFmpeg 8 and real Boost 1.83 packages. The full native build includes
the retained EGL engine/backend, CPU fallback, encoder and gateway. Its 52/52
CTest tests passed in 47.68 seconds; the minimal build passed 39/39 in 8.11
seconds. These include config/identity, capture/mapping ownership, codecs,
SDK recording/replay, calibration, asset rollback, system-agent, installation
contracts and frontend tests. The gateway integration suite passed in 43.09
seconds. These are host tests; no new Pi hardware performance measurement was
performed for this cleanup.

The architecture identity test still asserts the exact effective configuration
digest. Its expected value was regenerated for the same immutable seed after
removing retired fields from canonical serialization:
`610b539a0c6fd4bb9484c9878f2424811bda66f880e0eef78cccacea19bb1e08`.
Five collector regressions cover independent mono/matcher counters, repeated
samples, processing ownership of quality/grid fields, CLI sampling and shared
encoded-packet bounds. A final Windows rerun also passed all five current Node
suites and the 10-page/239-ID UI contract.

The restored Ubuntu host ships Boost 1.90, whose changed APIs are incompatible
with existing gateway code. Signed Ubuntu Boost 1.83 development/runtime packages
were extracted only under `/var/tmp/nuvo-depth-boost183`; the final build uses
their actual headers, CMake packages and system library. Repository dependency
rules and production source were not changed for that environment. No synthetic
Boost target or test-source adaptation was used in the final Ubuntu validation.

Commands, run from the repository root in Ubuntu:

```sh
cmake --preset minimal-host -B /var/tmp/nuvo-depth-cleanup-minimal
cmake --build /var/tmp/nuvo-depth-cleanup-minimal -j4
ctest --test-dir /var/tmp/nuvo-depth-cleanup-minimal --output-on-failure -j2

cmake --preset development-full -B /var/tmp/nuvo-depth-cleanup-development-full183 \
  -DDEPTHD_WITH_EGL_GL_DMABUF_RECTIFICATION=ON \
  -DBoost_DIR=/var/tmp/nuvo-depth-boost183/usr/lib/x86_64-linux-gnu/cmake/Boost-1.83.0
cmake --build /var/tmp/nuvo-depth-cleanup-development-full183 -j4
ctest --test-dir /var/tmp/nuvo-depth-cleanup-development-full183 --output-on-failure -j2
cmake --install /var/tmp/nuvo-depth-cleanup-development-full183 \
  --prefix /var/tmp/nuvo-depth-cleanup-stage
```

The staged install contains 91 file entries (89 regular files and two SDK
library symlinks). Every absolute HTML asset reference resolves. Retired
editor/demo/legacy-processing artifacts are absent, all four retained camera
diagnostic helpers are installed, and the two native executable paths contain
identical bytes. All three seed manifest hashes/sizes and staged
`depthd --check-config` pass. Imports of the staged Python client, raw/encoded
decoders and recording APIs pass; the staged C SDK loads with `ctypes.CDLL`.
The HTTP asset integration test also passed against the installed gateway and
web root, including eight stale prototype routes and four stale demo routes
returning 404. This was a staged CMake install, without running the installer or
creating a release package.

Host CTest logs are in each build directory's `Testing/Temporary/LastTest.log`.
Staged checks are recorded locally at
`/var/tmp/nuvo-depth-cleanup-stage-checks.json`; real Boost package hashes and
provenance are at `/var/tmp/nuvo-depth-boost183-provenance.json`.
Final protected-file comparisons, seed verification, UTF-8 and whitespace
checks pass. No Pi services, configuration, installer/release logic, tags or
remote branches were changed.
