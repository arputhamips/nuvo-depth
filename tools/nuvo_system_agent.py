#!/usr/bin/env python3
"""Root, socket-activated executor for a deliberately tiny control allowlist."""
from __future__ import annotations

import json
import re
import socket
import struct
import subprocess
import uuid

MAXIMUM_REQUEST=8192
SESSION_ID=re.compile(r"^[a-f0-9]{32}$")
CALIBRATION_TOOL=["/usr/lib/nuvo-depth/nuvo-calibration"]


def calibration_session(request: dict) -> str:
    value=request.get("session_id")
    if not isinstance(value,str) or not SESSION_ID.fullmatch(value):
        raise ValueError("invalid calibration session")
    return value


def execute(request: dict) -> dict:
    operation=request.get("operation")
    timeout=30
    if operation=="transport.stage":
        mode=request.get("preference")
        if not isinstance(mode,str) or mode not in {"auto","wifi","ethernet","usb","both","off"}: raise ValueError("invalid transport preference")
        command=["/usr/bin/nuvoctl","transport","stage",mode]
        if request.get("physical_confirmation") is True: command.append("--physical-confirmation")
    elif operation=="transport.confirm": command=["/usr/bin/nuvoctl","transport","confirm"]
    elif operation=="transport.rollback": command=["/usr/bin/nuvoctl","transport","rollback"]
    elif operation=="encoding.update":
        value=request.get("configuration")
        if not isinstance(value,dict): raise ValueError("encoding configuration must be an object")
        allowed={"schema_version","mode","codec","source","width","height","cadence_hz","bitrate_bps","jpeg_quality","keyframe_interval","threads","low_latency","preset"}
        if set(value)!=allowed: raise ValueError("encoding configuration fields are incomplete or unknown")
        if value.get("schema_version")!=1 or value.get("mode") not in {"raw","encoded","both"} or value.get("codec") not in {"h264","jpeg"} or value.get("source") not in {"left_rectified","right_rectified","left_color","right_color","disparity","depth","left_mono","right_mono","left_rgb","right_rgb","left_rectified_rgb","right_rectified_rgb","rectified_stereo","rectified_stereo_rgb"} or value.get("preset") not in {"ultrafast","superfast","veryfast","faster","fast"}:
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
    elif operation=="exposure.update":
        value=request.get("configuration")
        if not isinstance(value,dict) or len(json.dumps(value))>4096:
            raise ValueError("invalid exposure configuration")
        command=["/usr/bin/nuvoctl","exposure","set","--json",
                 json.dumps(value,separators=(",",":"))]
    elif operation=="calibration.create":
        if set(request)!={"operation","configuration"}:
            raise ValueError("calibration create fields are incomplete or unknown")
        value=request.get("configuration")
        if not isinstance(value,dict) or len(json.dumps(value))>6144:
            raise ValueError("invalid calibration session configuration")
        command=CALIBRATION_TOOL+["web-create","--state-root","/var/lib/nuvo-depth",
                                  "--configuration-json",json.dumps(value,separators=(",",":"))]
    elif operation=="calibration.status":
        if set(request)!={"operation","session_id"}:
            raise ValueError("calibration status fields are incomplete or unknown")
        session=calibration_session(request)
        command=CALIBRATION_TOOL+["web-status","--state-root","/var/lib/nuvo-depth",
                                  "--session",session]
    elif operation=="calibration.capture":
        if set(request)!={"operation","session_id"}:
            raise ValueError("calibration capture fields are incomplete or unknown")
        session=calibration_session(request)
        capture=uuid.uuid4().hex[:16]
        command=CALIBRATION_TOOL+["web-capture","--state-root","/var/lib/nuvo-depth",
                                  "--session",session,"--capture",capture,
                                  "--request-root","/run/nuvo-depth/calibration/requests",
                                  "--wait-seconds","8"]
        timeout=15
    elif operation=="calibration.solve":
        if set(request)!={"operation","session_id"}:
            raise ValueError("calibration solve fields are incomplete or unknown")
        session=calibration_session(request)
        command=CALIBRATION_TOOL+["web-solve","--state-root","/var/lib/nuvo-depth",
                                  "--session",session]
        timeout=300
    elif operation=="calibration.activate":
        if set(request)!={"operation","session_id"}:
            raise ValueError("calibration activate fields are incomplete or unknown")
        session=calibration_session(request)
        command=CALIBRATION_TOOL+["web-activate","--state-root","/var/lib/nuvo-depth",
                                  "--session",session,"--restart-depth-service"]
        timeout=165
    elif operation=="calibration.rollback":
        if set(request)!={"operation"}:
            raise ValueError("calibration rollback fields are unknown")
        command=CALIBRATION_TOOL+["web-rollback","--state-root","/var/lib/nuvo-depth",
                                  "--restart-depth-service"]
        timeout=165
    else: raise ValueError("operation is not in the system-agent allowlist")
    if operation in {"encoding.update","stereo.update","exposure.update"}:
        expected=request.get("expected_revision")
        if not isinstance(expected,str) or not re.fullmatch(r'"[a-f0-9]{64}"',expected):
            raise ValueError("configuration update requires a strong expected revision")
        command.extend(["--if-match",expected])
    completed=subprocess.run(command,capture_output=True,text=True,timeout=timeout,check=False)
    if completed.returncode==3 and operation in {"encoding.update","stereo.update","exposure.update"}:
        return {"status":"error","error_code":"precondition_failed",
                "error":"configuration changed; reload before saving"}
    if completed.returncode!=0: raise RuntimeError(completed.stderr.strip() or "control command failed")
    result=json.loads(completed.stdout)
    return {"status":"ok","result":result}


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
