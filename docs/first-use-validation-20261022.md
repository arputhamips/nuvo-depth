# First-use journeys and rolling installer validation — 2026-10-22

The README and user wiki now offer three complete entry routes: preinstalled
Vision One, a user-supplied supported Pi, and application integration. A separate
minimal route finishes with local depth, validity and metadata over SSH.

The maintained source bootstraps resolve the most recently published public
release containing the requested complete bundle/signature pair, including
public previews and excluding drafts. Resolution binds one exact tag before
bundle and manifest verification. Package hashes, platform qualification,
profile separation and downgrade checks retain their existing authority.
Release-specific artifacts still default to their exact tag; release notes
remain version-pinned. No release or tag was created for this work.

## Observable completion

- Current-source Live view can select both rectified images, depth and validity
  at 1 Hz best effort. It permits a raw NUV1 sample download only after two
  advancing captures with useful associated depth/validity. Stale data,
  disconnects, restarts and all-invalid output do not complete this check.
- The browser download preserves the received bytes, including calibration
  authority and hash. Validity code 1 is displayed white; other codes are black.
- The Python helper works as a module or a standalone script using existing SDK
  interfaces. It privately prompts for the administrative token, verifies TLS,
  rejects duplicate/reversed captures and invalid distances, and saves NVR1.
  Offline replay/inspection explicitly does not claim live freshness.
- The public beta 6 console does not include the new controls. Its documented
  route uses manual Outputs selection and the standalone Python helper.

## Verification

- Full Linux/WSL development build: **58/58 CTest checks passed**. The existing
  Boost 1.83 environment was reused under
  `/var/tmp/nuvo-depth-cleanup-development-full183`.
- After the native gateway asset allowlist change, the four focused first-use,
  latest-selection, UI and gateway-integration checks passed. The gateway test
  checks that `first-use.js` is actually served.
- Browser tests render the real console against deterministic API fixtures:
  **7 checks passed** for image-only waiting, advancing stereo/depth products,
  raw download metadata, all-invalid output, disconnect, reconnect and pending
  calibration. No browser page errors occurred.
- Anonymous live release discovery selected `v0.1.1-beta.6` for both profiles.
  Download-only smoke tests verified the actual signed full and minimal bundles,
  manifests and all **8 full / 1 minimal** package hashes and control metadata.
  No packages were installed on a Pi or host by this smoke test.
- The SDK extracted from the actual published beta 6 package successfully ran
  the standalone helper against the browser-produced sample. Provisional
  authority and the calibration hash were preserved.
- Wiki: **3 first-use tests / 4 release-catalog tests**, production VitePress
  build, **30 HTML pages / 159 built files** and reference/hash validation pass.
  Automated WCAG 2.2 AA, overflow, navigation/search and theme checks cover
  **10 page/viewport combinations**, including mobile setup/SDK pages.

Reproduce the browser fixture with `node tests/first_use_browser_test.cjs` and a
local `playwright-core` installation. `NUVO_PLAYWRIGHT_MODULE` can point to that
module; `BROWSER_EXECUTABLE` selects a Chromium executable. The generated
`output/first-use-browser/` files are local test evidence, not shipped data.
The wiki screenshot is explicitly labelled as synthetic fixture output.

## Calibration and hardware boundary

The beta 5 geometry/provenance seed, its provisional authority, active-bundle
selection, validation/activation code, and runtime calibration defaults were
not changed by this work. Validated per-unit bundles remain authoritative;
software version changes alone do not require recalibration.

These checks do not establish physical fresh-SD installation, reboot, power
cycle, external Wi-Fi handoff, camera electrical timing or metric accuracy.
Those require the supported rig and their own retained hardware acceptance.
