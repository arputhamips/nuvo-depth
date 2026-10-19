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
| `models/`, `scripts/` | 9 | Optional export/accelerator research source and manifests, not model binaries. Retained outside the product build. |
| `third_party/LiteAnyStereo` | One Git link | Optional research checkout pinned to a commit. Not needed by the product build; the checkout is not embedded in source exports. |

Filename-reference searches were checked against CMake, Debian conventions,
Python/module imports, dynamic asset loading and manual diagnostic entry points.
A file with no literal filename reference was not automatically treated as dead.
For example, Debian discovers `.install` files by package name and Python imports
modules without their `.py` suffix.

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

The legacy Python comparison path and optional semantic/accelerator sources are
not the normal native camera runtime. They retain explicit build gates,
diagnostic uses, tests or historical evidence dependencies. Removing those
features would require a separate decommissioning change rather than deleting
them based only on their absence from a normal Pi service invocation.

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
