# Signed APT publication

The release repository is staged with `reprepro`; the staging command refuses
to overwrite an existing directory and requires the full fingerprint of a
locally available secret signing key. Publish the completed directory with an
atomic server-side directory or object-prefix switch.

Example after arm64 qualification:

```text
python3 tools/release/stage_apt_repository.py \
  --output ../apt-beta-0.1.0 \
  --suite beta \
  --key-fingerprint FULL_40_HEX_RELEASE_KEY_FINGERPRINT \
  ../packages/*.deb
```

Clients receive a dedicated keyring and a source entry using `signed-by=`.
Release automation must verify `InRelease`, package hashes, expiry/freshness
policy, and an install from a clean qualified Raspberry Pi OS image before the
staged tree is published. Signing keys are never stored in this repository.

An offline bundle is the same signed repository tree plus the frozen dependency
pool; normal camera operation has no Internet dependency.
