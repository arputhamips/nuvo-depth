# Repository contents audit — 2026-10-19

The audit covers the tracked source tree, file references, CMake install rules,
Debian package manifests, source/SPDX exporters, generated files, and the main
user-facing documentation links. It does not remove local recordings, calibration
evidence, backups, or rewrite published release history.

## Inventory

The starting tree at `1797afc` contained **399 regular files, 5,366,408 bytes
(5.12 MiB), and one pinned research submodule**. Build directories, captures,
downloaded model binaries, `node_modules`, release downloads, private setup
material, and `.codex-tmp` were already ignored rather than published on GitHub.

| Group | Files before cleanup | Purpose and disposition |
| --- | ---: | --- |
| Root files and `cmake/` | 12 | Build/configuration metadata, installer, README, licenses. Moved the old root architecture review into `docs/history/`. |
| `.github/` | 4 | Package CI, installer checks, release signing and publication. Retained. |
| `apps/`, `src/`, `include/`, `platform/` | 84 | Runtime, gateway, interfaces, platform adapters and explicitly gated research/compatibility code. Retained. |
| `config/`, `presets/`, `qualification/` | 20 | Runtime seeds, calibration examples, candidate profiles and acceptance plan. Retained; package inputs now explicitly listed. |
| `sdk/`, `protocol/` | 11 | Python SDK, its packaging/license, and wire contract. Retained. |
| `tools/` | 53 | Provisioning, calibration, release operations, qualification, diagnostics and reproducible benchmarks. Retained; corrected unsafe source discovery. |
| `tests/` | 71 | Runtime, package, web, installer and hardware checks. Retained; two existing Python suites were missing from CTest and are now registered. |
| `web/` | 31 | Live console, shared modules, offline fonts, referenced benchmark photos and parked prototype source. Removed three generated prototype outputs. |
| `debian/`, `deployment/` | 43 | Package manifests, services, release platform/key, asset provisioning and deployment references. Retained. |
| `docs/` | 61 | Specifications, technical references, research and dated physical evidence. Retained, with old reviews identified as historical. |
| Former model/export research | 9 | Removed with the unused stereo and semantic/accelerator experiments. Model binaries were never included. |

The subsequent cleanup removed the external research checkout, its Git
submodule configuration, model export/compilation scripts, dedicated validation
tool and porting/feasibility notes. A dependency check then confirmed that the
semantic/Hailo libraries were separate opt-in research targets: the shipping
daemon, capture path, services and packages did not use them. Removed those
libraries, their headers, Python workers, semantic visualizer, dedicated tests,
model manifest and setup notes, plus their CMake options and package flags.
The current tree has no Git submodules; `models/`, `scripts/` and `third_party/`
no longer contain tracked files. The complete
current folder list is in the [README](../README.md#repository-layout).

Filename-reference searches were checked against CMake, Debian conventions,
Python/module imports, dynamic asset loading and manual diagnostic entry points.
A file with no literal filename reference was not automatically treated as dead.
For example, Debian discovers `.install` files by package name and Python imports
modules without their `.py` suffix.

## Depth-only scope and repository navigation

After retiring the unused semantic/accelerator branch, the tree contains
**359 tracked files in 17 top-level directories**. That change removes 29 files
and the associated CMake targets. Capture, CPU stereo matching, EGL/GLES
rectification, calibration, the gateway and client SDK remain in the tree.
Existing depth-only tests remain registered.

Removal verification: the reconfigured full host build passed 57/57 CTest
checks; the reconfigured dependency-light build passed 44/44. Both include
the source-export/package-hygiene checks. Local Markdown links resolve, and
the active CMake, headers, native source, Python tools and packaging have no
remaining semantic-inference/Hailo implementation references.

For context, the default branches inspected on 2026-10-19 contain:

| Project | Top-level directories, including hidden ones | Tracked files |
| --- | ---: | ---: |
| [RealSense SDK](https://github.com/realsenseai/librealsense/tree/c84fe6438bd14a4cc69f46deae92c993268232c6) | 14 | 4,048 |
| [DepthAI core](https://github.com/luxonis/depthai-core/tree/ed9961bf2c0adad744dbf042a2bfaf1fffde7780) | 19 | 1,751 |
| [Arducam ToF camera examples](https://github.com/ArduCAM/Arducam_tof_camera/tree/2a65e80d347947832326b312fe25e3ff0a44643e) | 4 | 35 |

These are different scopes: the small Arducam repository mainly contains
examples and wrappers, whereas this repository also owns the device daemon,
browser console, provisioning, SDK and signed package pipeline. Folder count
alone is not a useful measure of unnecessary code.

Keep the conventional `src/`, `include/`, `apps/`, `platform/`, `tests/`,
`cmake/` and `debian/` boundaries. The README now starts with functional areas
and puts the full folder map in a collapsible section. Current installation
and operation remain in the website wiki. Dated experiment reports should be
clearly marked as historical, and can be collected under `docs/history/` when
their references are updated together.

If a smaller root is still preferred, three small source-only moves can reduce
17 directories to 14: `presets/` into `config/presets/`, `protocol/` into
`sdk/protocol/`, and the qualification plan into `tools/qualification/plans/`.
Those are suggested follow-up layout changes, not part of this feature removal;
their installed paths and customer commands should remain stable.

## Removed generated output and reduced package inputs

- Removed `web/pipeline-canvas.js`, `web/pipeline-canvas.css`, and
  `web/pipeline-licenses.txt` from Git: **451,464 bytes (441 KiB)** of generated
  output for a parked, unserved prototype. Its source, lockfile, build script
  and notice generator remain available. Rebuilding produces ignored local
  files and the matching third-party notices.
- Moved the old top-level architecture review to
  [historical reviews](history/multi-platform-review.md) and marked its scope.
- Stopped installing AI review/handoff prompts in customer package documentation.
  The source references remain available for engineering history.
- Replaced recursive CMake installation of the web tree, Python SDK, presets
  and qualification directory with explicit file lists. Untracked secrets,
  experimental assets, caches or notes placed under those folders cannot enter
  the packages through those rules.
- Added ignore rules for Debian `obj-*` build trees, binary-package staging
  directories, build stamps, nested `node_modules` and generated prototype files.

The console's four benchmark JPEGs and three fonts are referenced by the actual
UI and its offline styling. Their notices remain included. The duplicate GPL
text at `sdk/python/LICENSE` is intentional: the Python distribution declares
it as its packaged license file. These are not unused copies.

The legacy Python depth comparison path retains diagnostic uses, tests and
historical evidence dependencies. It remains available separately from the
native camera runtime. Historical mixed-workload results may mention the
removed accelerator experiment; they describe the conditions of those runs,
not a feature available in the current product.

## Source exports

The older handoff and SPDX helpers walked the working directory directly. That
could include ignored local files and omit legitimate names beginning with
`build`, including release-builder source.

Both helpers now use the Git index for source membership. Exports retain local
edits to tracked source and Git executable/symlink modes, omit ignored and
untracked material, reject unresolved index conflicts, and do not dereference
symlinks into external files. New source must be staged before export. SPDX
inventory of a non-Git package tree requires an explicit `--staged-tree` choice.
The published release pipeline continues to use exact tagged `git archive`
source, alongside its signed package cohort.

## Website and installation links

The primary links are:

- [NuvoHardware website](https://nuvohardware.com/).
- [Vision One wiki](https://docs.nuvohardware.com/vision-one/).
- [Preinstalled/bundled unit setup](https://docs.nuvohardware.com/vision-one/install/bundled-pi).
- [Installation on your own Pi or card](https://docs.nuvohardware.com/vision-one/install/byo-pi).
- [First browser connection and owner login](https://docs.nuvohardware.com/vision-one/guide/getting-started).
- [Python SDK guide](https://docs.nuvohardware.com/vision-one/sdk/python).

README, documentation index, installer/deployment references, SDK metadata and
package homepage now lead to these destinations. Current user-facing examples
use HTTPS port `7443`; dated evidence retains the ports used during those tests.
The SDK's long administrative token is distinguished from the browser's short
first-use setup code.

## Verification

`tests/repository_hygiene_test.py` exercises actual source exports, SPDX output,
symlink handling and CMake package staging with unwanted local files present.
It also checks that the installed console still has its referenced assets.
The suite is registered with CTest. Existing gateway integration tests create
stale prototype fixtures and require their routes to remain inaccessible.

For this cleanup, run the hygiene, UI, installer and SDK tests, then the native
ARM64 package workflow and its clean Trixie installation check. The regular
[package checks](https://github.com/arputhamips/nuvo-depth/actions/workflows/package-build.yml)
and [installer checks](https://github.com/arputhamips/nuvo-depth/actions/workflows/installer-checks.yml)
record the results for the cleanup commit. Local Markdown file links and the
website/wiki installation URLs are checked as part of the audit.

### Completed checks

The tested implementation is commit
`f3af6e09733fca23153ede15ec0621d6d50b1a9f`.

| Check | Result |
| --- | --- |
| Development host build and CTest | 57/57 passed. |
| Final native ARM64 package build | [58/58 passed](https://github.com/arputhamips/nuvo-depth/actions/runs/39329398781). |
| Clean Trixie installation | All eight runtime packages installed; configuration, installed SDK import, setup/SPDX helpers and dpkg audit passed. |
| Installer checks | [Passed](https://github.com/arputhamips/nuvo-depth/actions/runs/39329399057). |
| Hygiene regressions | Seven checks passed from the checkout and an unpacked source archive containing an existing CMake build cache; isolated Git ownership was also exercised. |
| Downloaded package contents | All eight runtime payloads checked: no generated prototype assets, caches, `node_modules`, local environment files or review/handoff prompts. Homepage metadata is `https://nuvohardware.com/`. |
| Matching source | All 399 source files in the CI archive matched the tested Git tree byte-for-byte. |
| Parked frontend | Rebuilt successfully from source; its generated files remain ignored. |
| Documentation links | 141 local Markdown file links resolved; website, wiki, bundled/BYO installation, login and Python SDK URLs returned HTTP 200. |
| Website documentation | [Cloudflare deployment passed](https://github.com/arputhamips/nuvo-depth-web/actions/runs/39328117974); 28 built pages validated, and the updated SDK guide was verified live. |
