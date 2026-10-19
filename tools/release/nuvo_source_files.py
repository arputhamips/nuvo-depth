"""Inventory tracked source, or an explicitly selected clean staging tree.

Never walk a development checkout to guess which files belong in an export.
Git's index defines the source inputs, including their executable/symlink modes.
"""
from __future__ import annotations

import os
from pathlib import Path
import subprocess


def _read(root: Path, name: str, mode: int | None = None):
    path = root / name
    if not path.exists() and not path.is_symlink():
        return None  # A tracked file deleted in this working snapshot.
    if not path.parent.resolve().is_relative_to(root):
        raise ValueError(f"source parent escapes the selected root: {name}")
    if path.is_symlink():
        if mode is not None and mode != 0o120000:
            raise ValueError(f"tracked regular file replaced by a symlink: {name}")
        return name, os.fsencode(os.readlink(path)), 0o120777
    if mode == 0o120000 or not path.is_file():
        raise ValueError(f"unsupported source file type: {name}")
    return name, path.read_bytes(), mode or (0o100755 if path.stat().st_mode & 0o111 else 0o100644)


def source_files(root: Path, *, staged_tree: bool = False):
    root = root.resolve()
    if staged_tree:
        # This opt-in is for an intentionally prepared package tree, not source
        # discovery. Preserve symlinks as links and never read their targets.
        names = []
        for directory, dirs, files in os.walk(root, followlinks=False):
            dirs[:] = [name for name in dirs if name != '.git']
            files = [name for name in files if name != '.git']
            names.extend((Path(directory) / name).relative_to(root).as_posix()
                         for name in files + [name for name in dirs if (Path(directory) / name).is_symlink()])
        for name in sorted(names):
            if item := _read(root, name):
                yield item
        return
    try:
        top = subprocess.check_output(['git', '-C', str(root), 'rev-parse', '--show-toplevel'],
                                      stderr=subprocess.PIPE, text=True).strip()
        if Path(top).resolve() != root:
            raise ValueError('select the Git repository root')
        index = subprocess.check_output(['git', '-C', str(root), 'ls-files', '--stage', '-z'])
    except (subprocess.CalledProcessError, FileNotFoundError) as error:
        raise ValueError('source export needs a Git checkout; use an explicit clean staging tree for an SPDX inventory') from error
    for entry in sorted(index.split(b'\0'), key=lambda item: item.split(b'\t', 1)[-1]):
        if not entry:
            continue
        metadata, raw_name = entry.split(b'\t', 1)
        mode_text, _, stage = metadata.split()
        name = os.fsdecode(raw_name)
        if stage != b'0':
            raise ValueError(f'resolve the Git index conflict before exporting: {name}')
        mode = int(mode_text, 8)
        if mode == 0o160000:
            continue  # Nested repository checkouts are outside this source tree.
        if mode not in {0o100644, 0o100755, 0o120000}:
            raise ValueError(f'unsupported Git file mode: {name}')
        if item := _read(root, name, mode):
            yield item
