# Release discipline

- Routine fixes, benchmarks and testing use Actions artifacts; do not create tags
  or GitHub releases for each change.
- Build ARM64 test packages with the manual package-build workflow when needed.
- Create a versioned release draft only when the user explicitly requests a
  release candidate. Do not publish it or mark it latest without explicit approval.
- Beta candidates stay draft prereleases for internal testing. A public preview
  is a deliberate exception approved by the user, currently `v0.1.1-beta.5`.
- Public release notes start with a bullet-point changelog, then Pi prerequisites
  and the pinned curl command. No prose follows the command.
- Keep manufacturing provisioning and repository-access instructions out of
  customer release notes and installation pages.
- Documentation-only changes must not start ARM64 package builds or signing jobs.
