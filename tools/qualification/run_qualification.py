#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time

import yaml


def hash_file(path: Path) -> str:
    digest=hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda:source.read(1024*1024),b""): digest.update(chunk)
    return digest.hexdigest()


def atomic_json(path: Path, value: object) -> None:
    temporary=path.with_name(path.name+".tmp")
    with temporary.open("x",encoding="utf-8") as output:
        json.dump(value,output,sort_keys=True,indent=2); output.write("\n"); output.flush(); os.fsync(output.fileno())
    os.replace(temporary,path)


def system_manifest() -> dict:
    result={"kernel":platform.release(),"machine":platform.machine(),"python":platform.python_version()}
    camera_version = next(
        (name for name in ("rpicam-hello", "libcamera-hello") if shutil.which(name)),
        "rpicam-hello",
    )
    for name, command in {
        "os_release":["cat","/etc/os-release"],
        "packages":["dpkg-query","-W","-f=${binary:Package}\t${Version}\n"],
        "libcamera":[camera_version,"--version"],
        "throttled":["vcgencmd","get_throttled"],
    }.items():
        try:
            completed=subprocess.run(command,capture_output=True,text=True,timeout=30,check=False)
            result[name]={"exit_code":completed.returncode,"stdout":completed.stdout[-262144:],"stderr":completed.stderr[-16384:]}
        except (FileNotFoundError,subprocess.TimeoutExpired) as error:
            result[name]={"unavailable":str(error)}
    return result


def run_gate(gate: dict, directory: Path) -> dict:
    started=time.time_ns(); command=[str(value) for value in gate["command"]]
    try:
        completed=subprocess.run(command,capture_output=True,text=True,
                                 timeout=int(gate["timeout_seconds"]),check=False)
        result={"id":gate["id"],"status":"pass" if completed.returncode==0 else "fail",
                "command":command,"exit_code":completed.returncode,
                "started_unix_ns":started,"finished_unix_ns":time.time_ns(),
                "stdout":completed.stdout[-1024*1024:],"stderr":completed.stderr[-1024*1024:]}
    except (FileNotFoundError,subprocess.TimeoutExpired) as error:
        result={"id":gate["id"],"status":"fail","command":command,
                "started_unix_ns":started,"finished_unix_ns":time.time_ns(),"error":str(error)}
    path=directory/f"{gate['id']}.json"; atomic_json(path,result)
    return {"id":gate["id"],"status":result["status"],"evidence":path.name,"sha256":hash_file(path)}


def existing_gate(gate: dict, directory: Path) -> dict:
    path=directory/f"{gate['id']}.json"
    try:
        result=json.loads(path.read_text(encoding="utf-8"))
        if result.get("id") != gate["id"] or result.get("status") not in {"pass","fail"}:
            raise ValueError("stored gate identity or status is invalid")
        return {"id":gate["id"],"status":result["status"],"evidence":path.name,"sha256":hash_file(path)}
    except (OSError,json.JSONDecodeError,ValueError) as error:
        return {"id":gate["id"],"status":"fail","reason":f"cannot reuse {path.name}: {error}"}


def main() -> None:
    parser=argparse.ArgumentParser(description="Run the bounded Nuvo Pi 5 qualification ladder")
    parser.add_argument("--plan",default="/usr/share/nuvo-depth/qualification/pi5-beta.yaml")
    parser.add_argument("--output",required=True); parser.add_argument("--plan-only",action="store_true")
    parser.add_argument("--resume",action="store_true",help="reuse an existing evidence directory")
    parser.add_argument("--skip-automated",action="store_true",help="verify and reuse existing automated gate evidence")
    args=parser.parse_args(); plan_path=Path(args.plan); plan=yaml.safe_load(plan_path.read_text())
    if plan.get("schema_version")!=1: raise ValueError("unsupported qualification plan")
    if args.skip_automated and not args.resume:
        parser.error("--skip-automated requires --resume")
    output=Path(args.output)
    if output.exists():
        if not args.resume or not output.is_dir():
            raise FileExistsError(f"qualification output already exists: {output}")
    else:
        output.mkdir(parents=True)
    manifest=system_manifest() if not args.plan_only else {"plan_only":True}
    atomic_json(output/"system.json",manifest)
    gates=[]
    for gate in plan["automated_gates"]:
        if args.plan_only:
            result={"id":gate["id"],"status":"pending","reason":"plan-only run"}
        elif args.skip_automated:
            result=existing_gate(gate,output)
        else:
            result=run_gate(gate,output)
        gates.append(result)
        if gates[-1]["status"]=="fail": break
    completed={gate["id"] for gate in gates}
    for gate in plan["automated_gates"]:
        if gate["id"] not in completed: gates.append({"id":gate["id"],"status":"blocked","reason":"earlier automated gate failed"})
    for gate in plan["manual_evidence_gates"]:
        artifact=output/gate["artifact"]
        if artifact.exists():
            try:
                evidence=json.loads(artifact.read_text()); status="pass" if evidence.get("status")=="pass" else "fail"
                gates.append({"id":gate["id"],"status":status,"evidence":artifact.name,"sha256":hash_file(artifact)})
            except (json.JSONDecodeError,OSError) as error:
                gates.append({"id":gate["id"],"status":"fail","reason":str(error)})
        else: gates.append({"id":gate["id"],"status":"pending","required_artifact":gate["artifact"]})
    overall="pass" if gates and all(gate["status"]=="pass" for gate in gates) else "pending" if not any(gate["status"]=="fail" for gate in gates) else "fail"
    report={"schema_version":1,"qualification_id":plan["qualification_id"],
            "generated_unix_ns":time.time_ns(),"plan_sha256":hash_file(plan_path),
            "status":overall,"gates":gates,"release_authorized":overall=="pass"}
    atomic_json(output/"qualification.json",report); print(json.dumps(report,indent=2,sort_keys=True))


if __name__=="__main__": main()
