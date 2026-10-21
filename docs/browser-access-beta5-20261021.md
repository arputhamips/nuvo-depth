# Direct browser access and beta 5 preparation — 2026-10-21

The console now opens without a Nuvo password, setup code, claim form or
owner-password dialog. This is the explicitly selected browser access policy:
anyone who can reach the device's HTTPS console can use its administrative
controls. OS/SSH credentials and protected Wi-Fi setup are separate and remain
in place.

## Browser and API behavior

`GET /api/v1/session` runs before the ordinary API session gate. It reuses a
valid browser cookie or creates a session within the existing capacity and
twelve-hour expiry. The response supplies the CSRF token and a
Secure/HttpOnly/SameSite=Strict cookie. Origin mismatches and cross-site or
same-site browser Fetch Metadata are rejected; no cross-origin read access is
added. Automatic bootstrap does not authenticate against setup credentials,
consume pairing material or change the root-only ownership store.

Other API/WebSocket routes still require a session or compatible administrative
token. Mutations still require the session's CSRF token, and configuration
writes retain their strong edit-base revision check. The browser shares
concurrent bootstrap/recovery work, automatically recovers after a rejected
session, and retries the rejected request once with the new CSRF and original
configuration revision. A failed first connection can recover through periodic
settings polling; the first encoding configuration starts the preview streams.

The existing `POST /api/v1/session` token/password/code interface and explicit
legacy owner APIs remain compatible for existing integrations. SDK examples
continue to use `NuvoClient.claim()` with the administrative token. They are
separate from automatic browser access. Old setup status can still describe
legacy ownership, but the browser neither gates access on it nor requests a
password.

## Installation and release preparation

Interactive installation no longer asks for or creates a Nuvo owner password.
Factory and ordinary fresh installs no longer issue an enrollment code. The
console handoff prints the URL, certificate verification guidance and direct
access instructions. Protected hotspot details still appear only in the private
interactive terminal. Reruns preserve existing credential records, identity,
calibration, AP secrets and network selections; legacy explicit enrollment APIs
remain available. Actual network/transport/calibration/asset rollback logic and
the three seed inputs are unchanged.

`debian/changelog` prepares `0.1.1~beta.5`, with matching customer bullets in
`CHANGELOG.md`. Current repository and Wiki install commands use
`v0.1.1-beta.5`. The Wiki adds a preview entry while preserving the beta 4
record. Current guides describe direct console access; historical ownership
proposals and older physical qualification remain identified by scope. Generated
release notes start with changelog bullets, then Pi prerequisites, and end with
the exact release-pinned curl command. The release-policy record now includes
the user's explicit beta 5 public-preview authorization, while draft/review and
prerelease rules remain.

## Source validation

- Ubuntu development-full build with retained EGL backend, FFmpeg 8 and real
  Boost 1.83: passed. Full CTest: **53/53 passed in 44.27 seconds**, including
  31 gateway integration cases, 22 provisioning cases, signed bundle/PTY
  installer tests, legacy authentication compatibility and rollback suites.
- Six frontend Node suites and the 10-page/230-ID UI contract passed on Ubuntu
  and Windows. The automatic-session suite is also registered directly with
  CTest. It covers shared initial bootstrap, one retry after rejection, fresh
  CSRF with unchanged edit-base revision, bounded failures, malformed sessions
  and first-connection failure followed by periodic preview recovery.
- Gateway regressions cover fresh unclaimed and old claimed-without-password
  devices, unchanged credential material, no-claim raw streaming, valid CSRF
  mutations, origin/Fetch Metadata rejection, secure cookie attributes, reuse,
  stale-cookie replacement and session-cap exhaustion without losing existing
  access.
- Installer regressions cover fresh ordinary/factory plans without password
  callbacks or pairing creation, exact legacy credential preservation on rerun,
  direct console handoff, and continued protected-network behavior.
- Fresh staged CMake install passed at `/var/tmp/nuvo-depth-beta5-stage`.
  Installed web assets match source and have no login controls; all HTML asset
  paths, both native binary paths, three seed hashes, Python SDK imports and C
  SDK load pass. Three installed-gateway tests passed, including automatic
  sessions/CSRF and capacity. Local evidence:
  `/var/tmp/nuvo-depth-beta5-stage-checks.json`.
- Wiki release-catalog tests: **4/4 passed**. VitePress build and validation of
  **29 HTML pages / 154 built files** passed. Existing beta 4 catalog data is
  unchanged. No website deployment was performed during source preparation.
- Release-note rendering, UTF-8 and whitespace checks pass. Transport/calibration/
  asset rollback files and seed inputs remain byte-identical to `098910ed`.

The native build/test commands reuse the real Boost 1.83 environment recorded in
[the cleanup report](repository-cleanup-20261021.md). CTest logs are under
`/var/tmp/nuvo-depth-cleanup-development-full183/Testing/Temporary/LastTest.log`.
The ignored generated release notes are `.codex-tmp/beta5-release-notes.md`.

## Physical evidence boundary

The [cleaned-runtime Pi benchmark](performance-cleanup-pi-20261021.md) tests the
earlier frozen `098910ed` artifact, before these browser/installer changes. It
does not qualify the beta 5 cohort. The subsequent
[beta 5 signed-cohort qualification](release-qualification-beta5-20261021.md)
records the exact signed draft's successful interactive dry-run/upgrade,
automatic-session API checks and five Pi-local performance cases, with all
99 protected benchmark files restored. Visual browser inspection was blocked
by `ERR_CERT_AUTHORITY_INVALID`; no certificate bypass or rendered-video claim
was made. The same assets are now a public prerelease, with anonymous download
hashes verified. The actual public pinned curl Pi reinstall passed in 20.594
seconds, followed by a successful automatic-session/live-frame/CSRF check with
all eight beta 5 versions and 22 critical files preserved. Earlier UF-01/UF-02
records do not imply fresh-SD,
new-user, new hotspot or depth-accuracy acceptance of this cohort.
