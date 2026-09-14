#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

EXCLUDED_PARTS={".git",".pytest_cache","__pycache__","deliverables","oracle-bundles","third_party","models"}


def included(path: Path) -> bool:
    if any(part in EXCLUDED_PARTS or part.startswith("build") for part in path.parts): return False
    return path.suffix.lower() not in {".zip",".deb",".hef",".onnx",".pyc"}


def digest(data: bytes) -> str: return hashlib.sha256(data).hexdigest()


def archive_mode(name: str) -> int:
    path=Path(name)
    if name=="debian/rules" or (path.parts and path.parts[0] in {"tools","deployment"} and path.suffix==".py"):
        return 0o100755
    return 0o100644


def main() -> None:
    parser=argparse.ArgumentParser(description="Create a deterministic Nuvo source handoff")
    parser.add_argument("--source",default="."); parser.add_argument("--output",required=True)
    args=parser.parse_args(); root=Path(args.source).resolve(); output=Path(args.output).resolve()
    files=[]
    for path in sorted(root.rglob("*")):
        relative=path.relative_to(root)
        # Check the relative name first. Windows cannot stat WSL-created ELF
        # symlinks, and build/deliverable trees are outside this source handoff
        # in every case.
        if included(relative) and path.is_file():
            data=path.read_bytes(); files.append((relative.as_posix(),data,digest(data)))
    try: revision=subprocess.check_output(["git","-C",str(root),"rev-parse","HEAD"],text=True).strip()
    except (subprocess.CalledProcessError,FileNotFoundError): revision=None
    manifest={"schema_version":1,"format":"nuvo-depth-source-bundle",
              "base_git_revision":revision,"dirty_snapshot":True,
              "excluded":["build outputs",".git","research models/third_party","prior oracle bundles"],
              "files":[{"path":name,"sha256":checksum,"bytes":len(data),
                         "mode":oct(archive_mode(name)&0o777)} for name,data,checksum in files]}
    manifest_bytes=(json.dumps(manifest,sort_keys=True,indent=2)+"\n").encode()
    output.parent.mkdir(parents=True,exist_ok=True)
    if output.exists(): raise FileExistsError(output)
    timestamp=(2026,9,9,12,0,0)
    with zipfile.ZipFile(output,"x",compression=zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
        for name,data,_ in files:
            info=zipfile.ZipInfo("nuvo-depth/"+name,timestamp); info.create_system=3; info.compress_type=zipfile.ZIP_DEFLATED; info.external_attr=archive_mode(name)<<16
            archive.writestr(info,data)
        info=zipfile.ZipInfo("nuvo-depth/SOURCE-MANIFEST.json",timestamp); info.create_system=3; info.compress_type=zipfile.ZIP_DEFLATED; info.external_attr=0o100644<<16
        archive.writestr(info,manifest_bytes)
    with zipfile.ZipFile(output) as archive:
        for name,data,checksum in files:
            archived=archive.read("nuvo-depth/"+name)
            if len(archived)!=len(data) or digest(archived)!=checksum:
                raise RuntimeError("source bundle verification failed: "+name)
        if archive.read("nuvo-depth/SOURCE-MANIFEST.json")!=manifest_bytes:
            raise RuntimeError("source bundle manifest verification failed")
    print(json.dumps({"path":str(output),"bytes":output.stat().st_size,"sha256":digest(output.read_bytes()),"files":len(files)},sort_keys=True))


if __name__=="__main__": main()
