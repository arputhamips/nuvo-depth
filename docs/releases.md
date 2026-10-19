# Release operations

Nuvo builds native ARM64 Debian packages on GitHub's `ubuntu-24.04-arm` runner
inside a digest-pinned **Debian Trixie** container. The build adds the signed
Raspberry Pi package repository, checks its archive key against a pinned hash,
and installs `debian/control` build dependencies. This uses Trixie/Pi userspace
libraries, independent of the runner's Ubuntu host. A hosted build does not
establish camera, kernel, calibration or power compatibility on real hardware.

## Signing trust

The Ed25519 public key is [deployment/release-signing.pub](../deployment/release-signing.pub).
The same key is pinned in the source bootstrap and Python installer. Its
**SHA-256 fingerprint of the DER SubjectPublicKeyInfo** is:

```text
6cd440dd00741e095a4dc436001463506eab7e295dcf608a156311e46344ee15
```

```bash
openssl pkey -pubin -in deployment/release-signing.pub -outform DER | sha256sum
```

The private key is stored as the environment secret `NUVO_RELEASE_ED25519_PEM`
in `nuvo-install-release`. That environment permits deployment from `main`
only. Package/PR builds have read-only repository access and no signing secret.
The repository's active `v*` tag rules prevent tag updates and deletion, with
no bypass actors; creating a new version tag remains allowed.
Signing runs on a separate fresh runner after the package build succeeds. It
checks the private key against the committed public key and the tagged source,
checks the build's source commit, signs the manifest, then verifies the signature.
All action dependencies are pinned to commit IDs. No private key is an artifact.

The original private key is retained on the provisioning workstation outside
the repository, in `.ssh/nuvo_depth_release_ed25519.key`, with access limited to
the current Windows user and SYSTEM. Back it up in the owner's encrypted offline
vault. This key is independent of GitHub SSH/deploy keys and factory calibration
keys. Losing it requires a deliberate trust-key rotation; replacing a key on a
release download page alone must not silently change installed clients' trust.

Use **Actions > Check release signing > Run workflow > main** to verify the
configured secret without creating a release. It signs a random challenge and
verifies it against the pinned public key.

## Build and qualification

`ARM64 Debian packages` runs on main pushes, pull requests, manual dispatch and
as a reusable release job. `dpkg-buildpackage` runs the CTest suite and builds
all packages. The `nuvo-debian-arm64` artifact contains the `.deb` files,
`.buildinfo`, `.changes`, test log, exact dependency versions, source commit/run
record and `nuvo-depth-source.tar.gz`. Actions artifacts expire after 30 days;
published release assets are the durable customer distribution.

Before tagging an install release:

1. Set `debian/changelog` to a new version. A beta such as `0.1.1~beta.2` maps
   to tag `v0.1.1-beta.2`; stable `0.1.1` maps to `v0.1.1`.
2. Install and exercise the exact built packages on the target Pi. Preserve its
   current configuration, ownership, identity and accepted calibration. Verify
   APT dependency resolution, daemon/gateway configuration, console access,
   terminal/factory provisioning contracts and package/service lifecycle.
3. Fill `deployment/release-platform.json` from that evidence: architecture,
   codename, exact tested `uname -r` values, and the upstream base image URL and
   SHA-256. Record updates applied after flashing; a base image does not freeze
   later APT dependencies. Keep hardware acceptance separate from build tests.
   The Pi platform package pins its libcamera/IPA and rpicam cohort exactly;
   update those pins and the ARM64 build dependency together only after testing.
   Raspberry Pi changed from its own Span to std::span in October 2026 without
   changing the libcamera 0.7 SONAME, so the upstream version alone is insufficient.
4. Commit matching instructions and qualification evidence, wait for successful
   checks, then create a new tag on main. Never move or reuse a published tag.

An incomplete platform manifest blocks release before accessing the signing
secret. It does not block ordinary package builds.

For a local build on an ARM64 Trixie/Pi machine with the Raspberry Pi APT source:

```bash
sudo apt update
sudo apt install build-essential debhelper nodejs
sudo apt-get build-dep --no-install-recommends .
dpkg-checkbuilddeps
DEB_BUILD_OPTIONS=parallel=2 dpkg-buildpackage -b -us -uc
```

The disposable container setup is in `tools/ci/prepare-trixie-builder.sh`.
Do not run that CI repository-configuration script on a customer's installed OS.
Third-party build/runtime dependencies come from Debian/Raspberry Pi APT and
are recorded in `.buildinfo`. No Git submodule initialization is required.

For an editable working-tree handoff, `tools/release/create_source_bundle.py`
exports tracked Git source with its current contents and index file modes.
Stage new source files first. Ignored/untracked files and submodule checkouts
are excluded, and a tracked file replaced by a symlink is rejected. This helper
does not replace the release workflow's exact tagged `git archive` source.

`tools/release/generate_spdx.py` uses the same tracked-source boundary by
default. To inventory a deliberately prepared package tree without Git, pass
`--staged-tree`. That explicit mode inventories the supplied distribution tree;
prepare it separately from development outputs and credentials.

## Publish

The **Signed install release** workflow runs from main and takes an existing
tag plus `draft`, `prerelease` or `publish` visibility. It requires the tag to
be on main's history, the version to match the changelog and the platform
manifest to be complete. For example:

```bash
git tag -a v0.1.1-beta.2 -m 'Nuvo Depth 0.1.1 beta 1'
git push origin refs/tags/v0.1.1-beta.2
gh workflow run install-release.yml --ref main \
  -f tag=v0.1.1-beta.2 -f publication=prerelease
```

Choose `draft` to stage a release for review; customers cannot download draft
assets anonymously. `prerelease` publishes an explicitly marked development
release without making it latest. `publish` publishes a stable latest release.
Run once per tag; existing release assets are never overwritten. To promote an
already reviewed draft, publish that draft in GitHub rather than rebuilding it.
The workflow first uploads all assets to a draft, then applies the selected
visibility, so users are not offered a partially uploaded release.

From beta 4 onward, each install release exposes three customer downloads:

| File | Purpose |
| --- | --- |
| `install-nuvo-depth.sh` | Release-pinned installer, used by the one-line terminal command. |
| `nuvo-depth-pi5-arm64.tar.gz` | All eight runtime packages, installer archive, manifest, public key, LICENSE/NOTICE, README, matching source and build evidence. |
| `nuvo-depth-pi5-arm64.tar.gz.sig` | Ed25519 signature of the complete bundle, checked before extraction. |

GitHub also generates its standard source-code ZIP/tar downloads. Customers do
not choose individual `.deb` files. Older published releases retain their
individual assets so their original installer URLs keep working.

The bundle retains eight runtime packages at one exact version. GitHub-safe
internal filenames use dots in place of Debian version tildes; package
control metadata retains the exact `~beta.N` version. This normalization happens
before signing, so downloaded filenames match the manifest. The signed
manifest binds every included file's name, size and SHA-256. The bootstrap
checks the bundle signature before unpacking, then checks the manifest and
Python archive. The installer checks package
hashes and control metadata before APT installs them. Customers need HTTPS
access to public GitHub releases and their OS APT sources, with no GitHub
account, token or SSH key. See [customer install commands](install.md).

Owners can install modified software using their own signing key and the
installer's explicit `--repository`, `--public-key` or `--bundle` options. The
release signature proves provenance; it grants no factory calibration authority.
