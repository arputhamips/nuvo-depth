# Changelog

## [Unreleased]

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
