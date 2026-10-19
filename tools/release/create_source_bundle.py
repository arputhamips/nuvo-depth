#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile
from nuvo_source_files import source_files


def digest(data: bytes) -> str: return hashlib.sha256(data).hexdigest()


def main() -> None:
    parser=argparse.ArgumentParser(description="Create a deterministic Nuvo source handoff")
    parser.add_argument("--source",default="."); parser.add_argument("--output",required=True)
    args=parser.parse_args(); root=Path(args.source).resolve(); output=Path(args.output).resolve()
    files=[(name,data,digest(data),mode) for name,data,mode in source_files(root)]
    if output.is_relative_to(root) and output.relative_to(root).as_posix() in {name for name,_,_,_ in files}:
        raise ValueError('output must not replace a tracked source file')
    try: revision=subprocess.check_output(["git","-C",str(root),"rev-parse","HEAD"],text=True,stderr=subprocess.DEVNULL).strip()
    except (subprocess.CalledProcessError,FileNotFoundError): revision=None
    manifest={"schema_version":1,"format":"nuvo-depth-source-bundle",
              "base_git_revision":revision,"dirty_snapshot":True,
              "excluded":["Git metadata","ignored and untracked files","submodule checkouts"],
              "files":[{"path":name,"sha256":checksum,"bytes":len(data),
                         "mode":oct(mode&0o777)} for name,data,checksum,mode in files]}
    manifest_bytes=(json.dumps(manifest,sort_keys=True,indent=2)+"\n").encode()
    output.parent.mkdir(parents=True,exist_ok=True)
    if output.exists(): raise FileExistsError(output)
    timestamp=(2026,9,9,12,0,0)
    with zipfile.ZipFile(output,"x",compression=zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
        for name,data,_,mode in files:
            info=zipfile.ZipInfo("nuvo-depth/"+name,timestamp); info.create_system=3; info.compress_type=zipfile.ZIP_DEFLATED; info.external_attr=mode<<16
            archive.writestr(info,data)
        info=zipfile.ZipInfo("nuvo-depth/SOURCE-MANIFEST.json",timestamp); info.create_system=3; info.compress_type=zipfile.ZIP_DEFLATED; info.external_attr=0o100644<<16
        archive.writestr(info,manifest_bytes)
    with zipfile.ZipFile(output) as archive:
        for name,data,checksum,_ in files:
            archived=archive.read("nuvo-depth/"+name)
            if len(archived)!=len(data) or digest(archived)!=checksum:
                raise RuntimeError("source bundle verification failed: "+name)
        if archive.read("nuvo-depth/SOURCE-MANIFEST.json")!=manifest_bytes:
            raise RuntimeError("source bundle manifest verification failed")
    print(json.dumps({"path":str(output),"bytes":output.stat().st_size,"sha256":digest(output.read_bytes()),"files":len(files)},sort_keys=True))


if __name__=="__main__": main()
