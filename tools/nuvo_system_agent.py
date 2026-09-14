#!/usr/bin/env python3
"""Root, socket-activated executor for a deliberately tiny control allowlist."""
from __future__ import annotations

import json
import socket
import struct
import subprocess

MAXIMUM_REQUEST=8192


def execute(request: dict) -> dict:
    operation=request.get("operation")
    if operation=="transport.stage":
        mode=request.get("preference")
        if not isinstance(mode,str) or mode not in {"ethernet","usb","both","off"}: raise ValueError("invalid transport preference")
        command=["/usr/bin/nuvoctl","transport","stage",mode]
        if request.get("physical_confirmation") is True: command.append("--physical-confirmation")
    elif operation=="transport.confirm": command=["/usr/bin/nuvoctl","transport","confirm"]
    elif operation=="transport.rollback": command=["/usr/bin/nuvoctl","transport","rollback"]
    elif operation=="encoding.update":
        value=request.get("configuration")
        if not isinstance(value,dict): raise ValueError("encoding configuration must be an object")
        allowed={"schema_version","mode","codec","source","width","height","cadence_hz","bitrate_bps","jpeg_quality","keyframe_interval","threads","low_latency","preset"}
        if set(value)!=allowed: raise ValueError("encoding configuration fields are incomplete or unknown")
        if value.get("schema_version")!=1 or value.get("mode") not in {"raw","encoded","both"} or value.get("codec") not in {"h264","jpeg"} or value.get("source") not in {"left_rectified","right_rectified","disparity","depth"} or value.get("preset") not in {"ultrafast","superfast","veryfast","faster","fast"}:
            raise ValueError("invalid encoding configuration")
        numbers=("width","height","cadence_hz","bitrate_bps","jpeg_quality","keyframe_interval","threads")
        if any(isinstance(value.get(name),bool) or not isinstance(value.get(name),int) for name in numbers) or not isinstance(value.get("low_latency"),bool):
            raise ValueError("invalid encoding configuration types")
        command=["/usr/bin/nuvoctl","encoding","set",
                 "--mode",value["mode"],"--codec",value["codec"],"--source",value["source"],
                 "--width",str(value["width"]),"--height",str(value["height"]),
                 "--cadence-hz",str(value["cadence_hz"]),"--bitrate-bps",str(value["bitrate_bps"]),
                 "--jpeg-quality",str(value["jpeg_quality"]),"--keyframe-interval",str(value["keyframe_interval"]),
                 "--threads",str(value["threads"]),"--low-latency",str(value["low_latency"]).lower(),
                 "--preset",value["preset"]]
    elif operation=="stereo.update":
        value=request.get("configuration")
        if not isinstance(value,dict) or len(json.dumps(value))>4096:
            raise ValueError("invalid stereo configuration")
        command=["/usr/bin/nuvoctl","stereo","set","--json",
                 json.dumps(value,separators=(",",":"))]
    else: raise ValueError("operation is not in the system-agent allowlist")
    completed=subprocess.run(command,capture_output=True,text=True,timeout=30,check=False)
    if completed.returncode!=0: raise RuntimeError(completed.stderr.strip() or "control command failed")
    return {"status":"ok","result":json.loads(completed.stdout)}


def main() -> None:
    connection=socket.fromfd(0,socket.AF_UNIX,socket.SOCK_SEQPACKET)
    credentials=connection.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,struct.calcsize("3i"))
    pid,uid,gid=struct.unpack("3i",credentials)
    data=connection.recv(MAXIMUM_REQUEST+1)
    try:
        if len(data)>MAXIMUM_REQUEST: raise ValueError("control request exceeds bound")
        request=json.loads(data)
        response=execute(request); response["peer"]={"pid":pid,"uid":uid,"gid":gid}
    except Exception as error:
        response={"status":"error","error":str(error)}
    connection.send(json.dumps(response,sort_keys=True,separators=(",",":")).encode())


if __name__=="__main__": main()
