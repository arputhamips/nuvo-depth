#!/usr/bin/env python3
"""Create a new signed reprepro repository; never mutates an existing one."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import subprocess


def main() -> None:
    parser=argparse.ArgumentParser(description="Stage a signed Nuvo APT repository")
    parser.add_argument("--output",required=True); parser.add_argument("--key-fingerprint",required=True)
    parser.add_argument("--suite",choices=["beta","stable"],required=True); parser.add_argument("packages",nargs="+")
    args=parser.parse_args(); output=Path(args.output).resolve()
    if output.exists(): raise FileExistsError("repository output must not already exist")
    if not re.fullmatch(r"[0-9A-Fa-f]{40}",args.key_fingerprint): raise ValueError("full 40-hex signing fingerprint required")
    if not shutil.which("reprepro") or not shutil.which("gpg"): raise RuntimeError("reprepro and gpg are required")
    subprocess.run(["gpg","--batch","--list-secret-keys",args.key_fingerprint],check=True,stdout=subprocess.DEVNULL)
    (output/"conf").mkdir(parents=True)
    (output/"conf/distributions").write_text(
        f"Origin: Nuvo Depth\nLabel: Nuvo Depth {args.suite}\nCodename: {args.suite}\n"
        "Architectures: arm64 all\nComponents: main\nDescription: Signed Nuvo Depth appliance repository\n"
        f"SignWith: {args.key_fingerprint.upper()}\n",encoding="utf-8")
    for package in args.packages:
        subprocess.run(["reprepro","--basedir",str(output),"includedeb",args.suite,str(Path(package).resolve())],check=True)
    print(output)


if __name__=="__main__": main()
