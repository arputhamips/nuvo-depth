#!/usr/bin/env python3
"""Render reviewed changelog bullets and the version-pinned install command."""
from __future__ import annotations

import argparse
from pathlib import Path
import re

import nuvo_release

TAG = re.compile(r"v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:-beta\.[1-9][0-9]*)?\Z")
CATEGORIES = {"Added", "Changed", "Fixed", "Removed", "Security", "Known issues"}


def validate_channel(tag: str, channel: str):
    if not TAG.fullmatch(tag):
        raise ValueError("use vX.Y.Z or vX.Y.Z-beta.N")
    if channel not in {"beta", "stable"} or ("-beta." in tag) != (channel == "beta"):
        raise ValueError("release channel must match the version tag")


def changelog_entry(document: str, tag: str) -> str:
    entries = list(re.finditer(r"^## \[([^\]]+)\].*$", document, re.M))
    matches = [i for i, entry in enumerate(entries) if entry.group(1) == tag]
    if len(matches) != 1:
        raise ValueError("add exactly one reviewed changelog entry for this version")
    index = matches[0]
    end = entries[index + 1].start() if index + 1 < len(entries) else len(document)
    body = document[entries[index].end():end].strip()
    if any(value in body.casefold() for value in ("--factory", "factory provisioning", "github account", "ssh key")):
        raise ValueError("keep release notes focused on customer-facing changes")
    bullets = 0
    for line in body.splitlines():
        if not line.strip():
            continue
        if line.startswith("### ") and line[4:] in CATEGORIES:
            continue
        if line.startswith("- ") and line[2:].strip():
            bullets += 1
            continue
        raise ValueError("release changelogs use bullet points and optional category headings")
    if not bullets:
        raise ValueError("release changelog must contain changes")
    return body


def render(source: Path, repository: str, tag: str, channel: str) -> str:
    validate_channel(tag, channel)
    if not nuvo_release.REPOSITORY.fullmatch(repository):
        raise ValueError("invalid owner/repository")
    changes = changelog_entry((source / "CHANGELOG.md").read_text(encoding="utf-8"), tag)
    platform = nuvo_release.validate_platform(nuvo_release.load_json(source / "deployment/release-platform.json"))
    kernels = ", ".join(f"`{value}`" for value in platform["kernels"])
    return (f"## Changelog\n\n{changes}\n\n"
            "## Install\n\n"
            "- Raspberry Pi 5.\n"
            "- Raspberry Pi OS Lite 64-bit, Debian Trixie (`arm64`).\n"
            f"- Supported kernel: {kernels}.\n"
            "- Internet connection and administrator access on the Pi.\n\n"
            "```bash\n"
            f"curl -fsSL https://github.com/{repository}/releases/download/{tag}/install-nuvo-depth.sh | sudo bash\n"
            "```\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--repository", default=nuvo_release.DEFAULT_REPOSITORY)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--channel", choices=("beta", "stable"), default="beta")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(render(args.source, args.repository, args.tag, args.channel), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
