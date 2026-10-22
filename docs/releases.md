# Release operations

## Channels

- **Test build:** manual Actions artifact, identified by its source commit. No tag
  or release is created for routine fixes or benchmarks.
- **Internal beta:** `vX.Y.Z-beta.N`, built and signed as a draft prerelease.
- **Public preview:** an explicitly approved beta. The current preview is
  `v0.1.1-beta.6`; earlier approved previews remain available and experimental betas remain drafts.
- **Stable:** `vX.Y.Z`, published only after approval and marked latest.
- Stable releases appear first on the website, followed by approved public
  previews; each group is sorted by semantic version, newest first.

## Build packages for testing

- `ARM64 Debian packages` runs only by manual dispatch or from the reusable
  release workflow. Pushes, pull requests and README edits do not start it.
- `Installer checks` runs only for installer, packaging, policy or related test
  changes. Documentation-only edits do not start it.
- The ARM64 runner builds inside the pinned Debian Trixie container using
  `tools/ci/prepare-trixie-builder.sh` and `dpkg-buildpackage`.
- The `nuvo-debian-arm64` artifact contains packages, matching source, dependency
  versions and test/build records. It expires after 30 days.
- Select `package_profile=minimal` for the independent `nuvo-minimal-arm64`
  artifact and clean runtime installation checks.

```bash
gh workflow run package-build.yml --ref main -f source_ref=COMMIT
gh workflow run package-build.yml --ref main -f source_ref=COMMIT -f package_profile=minimal
```

## Prepare a release candidate

- Create a candidate only when a release version has been explicitly requested.
- Update `debian/changelog` and add the same version to [CHANGELOG.md](../CHANGELOG.md).
- Write customer-facing bullet points, optionally grouped under Added, Changed,
  Fixed, Removed, Security or Known issues.
- Test the exact package cohort on the Pi. Record installation, upgrade,
  service, network and camera results without replacing unit credentials.
- Complete `deployment/release-platform.json` with the supported OS image,
  SHA-256, tested kernels and camera-library versions.
- Tag the reviewed commit. Never move or reuse a published version tag.
- Run **Prepare signed release draft** from main with the existing tag and
  matching `beta` or `stable` channel. There is no automatic publication option.

```bash
gh workflow run install-release.yml --ref main \
  -f tag=vX.Y.Z-beta.N -f channel=beta
```

- `0.1.1~beta.6` in Debian metadata maps to `v0.1.1-beta.6`.
- Beta tags cannot be built as stable candidates, or stable tags as betas.
- A missing, empty or unreviewed changelog entry blocks release preparation.
- The workflow verifies the tag/version and platform record, builds/tests the
  full and minimal packages from the same commit, checks both build records,
  signs each exact cohort separately and creates one draft with six assets attached.
- Existing releases and assets are never overwritten.

## Review and publish

- Review and test the draft. Publishing requires an explicit human decision.
- Keep internal betas as drafts. A prerelease flag alone does not hide a public
  experiment from GitHub's release feed.
- Publish an approved preview as a prerelease and leave **Set as latest** off.
- Publish a stable version as a stable release and explicitly mark it latest.
- Promote the reviewed draft; do not rebuild or silently replace its assets.
- Update the website's `Wiki/release-catalog.json` only for an approved public
  release. Set `public: true` and choose `stable` or `preview`.
- After public approval and publication, update `deployment/public-release.json`
  with the approved tag/channel and exact approved-tag platform/profile SHA-256
  values. Development versions and draft candidates never update this pointer.
- Run `python3 tools/check_setup_consistency.py --write --wiki "../Nuvo Depth Web/Wiki"`
  and the wiki's `npm run sync:setup` / `npm run test:setup`. The rolling commands
  remain unchanged; release-specific commands remain pinned for reproducibility.
- Retain historical engineering files for evidence links, outside the customer
  release list and search content.

## Release notes

- Source: the exact version's entry in `CHANGELOG.md`.
- Order: **Changelog**, optional categories with bullets, **Install** with Pi OS
  prerequisites, then the release-pinned curl commands for the two installation choices.
- End at the command; no installer tutorial, asset-purpose table, manufacturing
  flags or repository-access explanations.
- `tools/release_notes.py` generates the same format for drafts and public releases.

## Signing and assets

- The pinned Ed25519 public key is [deployment/release-signing.pub](../deployment/release-signing.pub).
- SHA-256 of its DER public key: `6cd440dd00741e095a4dc436001463506eab7e295dcf608a156311e46344ee15`.
- The private key is the `NUVO_RELEASE_ED25519_PEM` secret in the
  `nuvo-install-release` environment, restricted to main.
- Package builds have read-only repository access and no signing secret.
- Signing verifies the committed public key, source commit and package cohort.
- All action dependencies are pinned. Private keys are never artifacts.
- Use **Check release signing** to verify the configured key without a release.
- Full assets: `install-nuvo-depth.sh`, `nuvo-depth-pi5-arm64.tar.gz`, and its `.sig`.
- Minimal assets: `install-nuvo-depth-minimal.sh`, `nuvo-depth-minimal-pi5-arm64.tar.gz`,
  and its `.sig`. GitHub also provides its standard source archives.
- The full bundle retains eight runtime packages; the minimal bundle retains
  only `nuvo-depth-minimal`. Each includes its own installer, matching GPL
  source, licenses and build records. Neither includes the factory package.
- Package filenames are normalized before signing; Debian control metadata keeps
  its exact version. The installer verifies the bundle, manifest and package hashes.
- A build/test result is not hardware or calibration acceptance. Keep those
  records with the exact tested cohort.

## Source and local builds

- See [build and test](build-and-test.md) for dependencies and native builds.
- `tools/release/create_source_bundle.py` exports tracked source and file modes;
  ignored/untracked files and replaced source symlinks are excluded.
- `tools/release/generate_spdx.py` uses the same tracked-source boundary.
- Manufacturing instructions live in [factory provisioning](factory-provisioning.md),
  separate from customer downloads and install pages.
