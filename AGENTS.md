# Release discipline

- Routine fixes, benchmarks and testing use Actions artifacts; do not create tags
  or GitHub releases for each change.
- Build ARM64 test packages with the manual package-build workflow when needed.
- Create a versioned release draft only when the user explicitly requests a
  release candidate. Do not publish it or mark it latest without explicit approval.
- Beta candidates stay draft prereleases for internal testing. The user explicitly
  approved `v0.1.1-beta.9` as the next public preview; advance the public installer
  pointer only after its signed release is published. Until then it selects
  `v0.1.1-beta.8`.
- Public release notes start with a bullet-point changelog, then Pi prerequisites
  and the pinned curl command. No prose follows the command.
- Keep manufacturing provisioning and repository-access instructions out of
  customer release notes and installation pages.
- Documentation-only changes must not start ARM64 package builds or signing jobs.

# Calibration default

- Reuse the geometry used on the unchanged hardware with beta 5, source bundle
  `8adf4c8103f1fb7f2811a7d483669cc99c8c7132902700ebb3fd464d19158cf2`.
  Its checked-in seed and provenance are in `config/calibration/nuvo_depth_100mm/`.
- Keep shared seed authority provisional; preserve existing validated per-unit
  bundles. Do not require recalibration solely because the software version changes.
