# Debian archive and APT publication roadmap

`apt install nuvo-depth` requires the package to exist in an APT source known
to the machine. There are two different publication targets:

1. a signed Nuvo-owned APT repository for immediate product releases; and
2. the official Debian archive, which requires Debian-compatible licensing,
   source publication, review, sponsorship, and ongoing maintenance.

## Current official-Debian blockers

The repository is not ready for an official Debian upload:

- Nuvo-authored software is now GPL-3.0-or-later; the recorded third-party
  notices still need a complete binary/source distribution audit;
- `debian/control` uses a placeholder `.example` maintainer address;
- versioned upstream releases and their matching source artifacts still need
  to be published from the public GitHub repository;
- a complete third-party license/source audit and Debian Free Software
  Guidelines classification have not been recorded;
- the source package has not yet passed clean `sbuild`, `lintian`, `autopkgtest`,
  `piuparts`, reproducibility, and arm64 buildd checks in current Debian
  unstable;
- no WNPP Intent To Package (ITP), Debian packaging repository, or sponsor has
  been established.

The project license permits public source publication. An official Debian upload
still requires the complete packaging and third-party review listed above.

## Route into the official Debian archive

1. Choose and publish redistribution terms for all shipped source/assets.
   Put the complete machine-readable result in `debian/copyright`. Decide with
   a Debian reviewer whether the package belongs in `main`, another archive
   component, or cannot be accepted.
2. Publish a stable upstream source tarball and replace the placeholder
   maintainer identity with a monitored address. Add normal upstream metadata,
   a watch file, and Debian Salsa packaging repository.
3. Check the WNPP list and file an `ITP: nuvo-depth -- ...` bug against the
   `wnpp` pseudo-package. Reference that bug from the first Debian changelog
   entry.
4. Build a source-only upload against current Debian unstable in a clean
   environment. Run `lintian`, package build-time tests, installed-package
   `autopkgtest`, install/upgrade/remove/purge testing, and reproducible-build
   comparison. Hardware-dependent tests must skip clearly on Debian buildds;
   they cannot require a camera or privileged Pi configuration.
5. Upload the signed source package to `mentors.debian.net`, file a Request For
   Sponsorship, and iterate with a Debian Developer sponsor. A sponsor reviews
   and uploads it for a maintainer without archive upload rights.
6. Pass the Debian NEW queue for the new binary package names. Maintain release
   critical bugs and future upstream updates after acceptance. Packages normally
   enter unstable first; migration and stable availability follow Debian's
   archive and release processes.

After acceptance and after users have a Debian suite containing the package,
installation is the ordinary:

```bash
sudo apt update
sudo apt install nuvo-depth
```

## Immediate product route

An official Debian upload is not a substitute for a product release channel.
For controlled Pi 5 deployments, publish the existing `.deb` cohort in a
Nuvo-owned, HTTPS, signed APT repository while the Debian review proceeds. The
repository already includes `tools/release/stage_apt_repository.py` to stage a
`reprepro` repository; production still needs an offline signing key, signed
`InRelease`, documented key rotation/revocation, immutable versioned artifacts,
arm64 CI, and upgrade/rollback qualification.

Keep the repository opt-in. Package installation must not silently enable the
hardware service or alter Raspberry Pi boot configuration; provisioning remains
an explicit administrator action.

## Debian references

- New packages and ITP process:
  <https://www.debian.org/doc/manuals/developers-reference/pkgs.en.html#new-packages>
- Guide for Debian Maintainers:
  <https://www.debian.org/doc/manuals/debmake-doc/>
- Debian Mentors and sponsorship:
  <https://wiki.debian.org/DebianMentors>
- Debian Policy:
  <https://www.debian.org/doc/debian-policy/>
