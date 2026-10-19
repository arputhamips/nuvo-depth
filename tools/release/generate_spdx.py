#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import uuid

EXCLUDED_PARTS={".git",".pytest_cache","__pycache__","deliverables","oracle-bundles","third_party","models"}


def included(path: Path) -> bool:
    if any(part in EXCLUDED_PARTS or part.startswith("build") for part in path.parts):
        return False
    return path.suffix.lower() not in {".zip",".deb",".hef",".onnx",".pyc"}


def main() -> None:
    parser=argparse.ArgumentParser(description="Generate a file-level SPDX 2.3 build artifact")
    parser.add_argument("root"); parser.add_argument("output"); parser.add_argument("--version",required=True)
    parser.add_argument("--created",default="2026-09-08T17:00:00Z")
    args=parser.parse_args(); root=Path(args.root).resolve(); output=Path(args.output).resolve()
    files=[]
    for path in sorted(root.rglob("*")):
        relative_path=path.relative_to(root)
        if included(relative_path) and path.is_file() and path.resolve()!=output:
            relative="./"+relative_path.as_posix(); checksum=hashlib.sha256(path.read_bytes()).hexdigest()
            files.append({"SPDXID":"SPDXRef-File-"+hashlib.sha256(relative.encode()).hexdigest()[:16],
                          "fileName":relative,"checksums":[{"algorithm":"SHA256","checksumValue":checksum}],
                          "licenseConcluded":"NOASSERTION","copyrightText":"NOASSERTION"})
    package_digest=hashlib.sha256("".join(item["checksums"][0]["checksumValue"] for item in files).encode()).hexdigest()
    namespace=uuid.uuid5(uuid.NAMESPACE_URL,f"nuvo-depth:{args.version}:{package_digest}")
    document={"spdxVersion":"SPDX-2.3","dataLicense":"CC0-1.0","SPDXID":"SPDXRef-DOCUMENT",
              "name":f"nuvo-depth-{args.version}","documentNamespace":f"urn:uuid:{namespace}",
              "creationInfo":{"created":args.created,"creators":["Tool: nuvo-generate-spdx/1"]},
              "documentDescribes":["SPDXRef-Package"],
              "packages":[{"name":"nuvo-depth","SPDXID":"SPDXRef-Package","versionInfo":args.version,
                           "downloadLocation":"NOASSERTION","filesAnalyzed":True,
                           "licenseConcluded":"NOASSERTION","licenseDeclared":"GPL-3.0-or-later",
                           "copyrightText":"NOASSERTION"}],"files":files,
              "relationships":[{"spdxElementId":"SPDXRef-Package","relationshipType":"CONTAINS","relatedSpdxElement":item["SPDXID"]} for item in files]}
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(json.dumps(document,sort_keys=True,indent=2)+"\n",encoding="utf-8")


if __name__=="__main__": main()
