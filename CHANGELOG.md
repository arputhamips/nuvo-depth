# Changelog

## [Unreleased]

## [v0.1.1-beta.8] - 2026-10-22

### Fixed

- Verify supplied calibration through the shared activation transaction and restore the previous selection on startup failure.
- Report malformed calibration/status state with an actionable unavailable result.
- Preserve minimal preview and validated calibration policies during upgrades, including stopped services.
- Keep deliberately disabled capture disabled when an old validated status file remains, and separate stopped-service state from activation verification.
- Bound public-release discovery and handle unusable IPv6 paths without changing device network settings.
- Preserve committed configuration on storage errors and allow retries after a process crash.
- Release finished SDK sessions without interrupting other clients and report session-capacity failures accurately.
- Keep the two-thread CPU baseline, preserve validated per-unit bundles and reuse unchanged beta 5 shared geometry with provisional authority.

### Added

- Guide the first fresh depth sample in the console and Python quickstart, retaining original frame identity, validity and calibration metadata.
- Show consistent calibration readiness and next actions in installation, console and CLI.
- Import compatible existing per-unit calibration on full and minimal with dry-run validation, transactional activation and rollback.
- Follow one centrally approved public release in rolling installers, keeping future upgrades independent of README command edits.
- Export allowlisted read-only diagnostics from the console and terminal, with installed-version and supported upgrade guidance.
- Run Python depth, pixel, display, stream, recording and replay examples and a public C decoder example.
- Reproduce host dependencies and exercise native, SDK and rendered-browser checks continuously.

## [v0.1.1-beta.7] - 2026-10-22

### Added

- Guide the first fresh depth sample in the console and Python quickstart, with original calibration and frame metadata retained in saved output.
- Show the same calibration readiness, explanation and next action in installer completion, console and terminal status.
- Import compatible existing calibration bundles in both full and minimal installations with dry-run validation, verified activation and rollback.
- Require an explicit provisional-preview choice on fresh installations and keep advanced calibration parameters expandable.

### Fixed

- Resolve rolling installers through one explicitly approved public release, keeping unreleased candidates separate from installation recommendations.
- Align supported-platform prerequisites, automatic HTTPS connection instructions and the Balanced starting recommendation.
- Preserve validated per-unit calibration and the unchanged beta 5 geometry through reinstalls and upgrades; keep shared geometry provisional.
- Restore the previous bundle and capture policy after failed or interrupted calibration activation.

## [v0.1.1-beta.6] - 2026-10-21

### Added

- Offer a minimal Raspberry Pi 5 package using the existing native depth pipeline with raw depth output.
- Configure the minimal installation over SSH with an interactive terminal wizard and `nuvoctl` commands.
- Capture float depth, validity masks and metadata, or stream raw frames from the terminal.
- Provide separate signed bundles and version-pinned install scripts for the full appliance and minimal raw depth profiles.

### Changed

- Keep the full appliance installation available with its existing browser interface and setup workflow.
- Omit the browser interface, network setup, SDK, calibration tools and encoder from the minimal installation.

## [v0.1.1-beta.5] - 2026-10-21

### Changed

- Open the HTTPS console directly without a browser password, setup code, or owner-password prompt.
- Install and reinstall without creating browser credentials; preserve existing device identity, calibration and network settings.
- Process independent image and depth subscriptions on separate bounded workers.
- Reuse bounded frame buffers and draw only visible live canvases.

### Fixed

- Restore browser sessions automatically after expiry or a gateway restart while retaining origin and CSRF checks.
- Reject stale encoded captures after startup or reconnect and wait for fresh output.
- Accept encoded frames with the device's canonical UUID format during startup.
- Keep image delivery current when an older depth result finishes.
- Report completed depth output separately from image-only publications.

### Removed

- Retire the parked pipeline editor, parallel legacy Python processor and its private output libraries.
- Replace photographic demo placeholders with waiting and disconnected live-view states.

## [v0.1.1-beta.4] - 2026-10-19

### Changed

- Combine installation downloads into one signed package bundle.
- Support terminal questions and masked input with the curl installer.

## [v0.1.1-beta.3] - 2026-10-19

### Fixed

- Accept browser-normalized hostnames when confirming a Wi-Fi connection.
- Save and verify the Wi-Fi country before starting the setup hotspot.
- Display hotspot recovery errors and check the boot configuration earlier.

## [v0.1.1-beta.2] - 2026-10-19

### Fixed

- Normalize release asset filenames before signing the installation manifest.
- Pin the tested camera libraries and preserve setup state during reinstall.
