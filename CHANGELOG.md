# Changelog

## [Unreleased]

### Changed

- Process independent image and depth subscriptions on separate bounded workers.
- Build test packages on request and prepare releases as drafts for review.

### Fixed

- Accept encoded frames with the device's canonical UUID format during startup.
- Keep image delivery current when an older depth result finishes.

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
