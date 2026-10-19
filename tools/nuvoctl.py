#!/usr/bin/env python3
"""Administrative control surface for the Nuvo Depth appliance."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
import re
from pathlib import Path
import secrets
import shutil
import subprocess
import time
import uuid


def fsync_dir(path: Path) -> None:
    # Windows does not permit opening a directory through os.open(). The
    # durable rename path is used on the Linux appliance; host-side tests and
    # planning commands still need atomic replacement semantics on Windows.
    if os.name == "nt":
        return
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try: os.fsync(descriptor)
    finally: os.close(descriptor)


def atomic_write(path: Path, data: str, mode: int = 0o640) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, mode)
    try:
        # Service UMask is intentionally restrictive. Set the requested final
        # mode on the still-private temporary inode before writing or renaming.
        if os.name != "nt":
            os.fchmod(descriptor, mode)
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as output:
            output.write(data); output.flush(); os.fsync(output.fileno())
        os.replace(temporary, path); fsync_dir(path.parent)
    except Exception:
        try: temporary.unlink()
        except FileNotFoundError: pass
        raise


class ConfigurationConflict(RuntimeError):
    pass


@contextmanager
def configuration_lock(root: Path):
    # Every privileged configuration writer uses one persistent lock inode;
    # atomic replacement must not replace the inode providing the lock.
    path = root / "var/lib/nuvo-depth/transactions/configuration.lock"
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as lock:
        if os.name == "nt":
            import msvcrt
            if lock.seek(0, os.SEEK_END) == 0:
                lock.write(b"\0"); lock.flush()
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl
            fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            yield
        finally:
            if os.name == "nt":
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(lock, fcntl.LOCK_UN)


def write_configuration(root: Path, name: str, value: dict,
                        expected_revision: str | None = None) -> dict:
    if expected_revision is not None and not re.fullmatch(r'"[a-f0-9]{64}"', expected_revision):
        raise ValueError("If-Match must be one strong configuration ETag")
    path = root / "etc/nuvo-depth" / name
    data = json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n"
    with configuration_lock(root):
        if expected_revision is not None:
            current = hashlib.sha256(path.read_bytes() if path.exists() else b"").hexdigest()
            if expected_revision != '"' + current + '"':
                raise ConfigurationConflict("configuration changed; reload before saving")
        atomic_write(path, data, 0o644)
        if expected_revision is not None:
            return {"configuration":value,"configuration_json":data,"requested_revision":hashlib.sha256(data.encode()).hexdigest(),
                    "state":"pending"}
    return value


def device_model(root: Path) -> str:
    path = root / "proc/device-tree/model"
    return path.read_bytes().rstrip(b"\x00").decode(errors="replace") if path.exists() else "unknown"


def boot_config(root: Path) -> Path:
    # Current Raspberry Pi OS keeps the effective configuration under
    # /boot/firmware while retaining /boot/config.txt as a regular-file notice.
    # Prefer the current location and use the legacy location only as a fallback.
    firmware = root / "boot/firmware/config.txt"
    legacy = root / "boot/config.txt"
    if firmware.exists():
        return firmware
    if legacy.exists():
        return legacy
    raise RuntimeError("Raspberry Pi boot config is missing")


def platform_preflight(root: Path) -> dict:
    model = device_model(root)
    result = {"schema_version":1, "platform":"pi5", "model":model,
              "checks":{}, "ready":False, "reboot_required":False}
    try:
        config = boot_config(root); content = config.read_text(encoding="utf-8")
        result["boot_config"] = str(config)
        result["checks"]["camera_auto_detect_disabled"] = "camera_auto_detect=0" in content
        result["checks"]["cam0_overlay"] = "dtoverlay=imx296,always-on,cam0" in content
        result["checks"]["cam1_overlay"] = "dtoverlay=imx296,always-on,cam1" in content
    except RuntimeError as error:
        result["checks"]["boot_config"] = str(error)
    trigger = root / "sys/module/imx296/parameters/trigger_mode"
    result["checks"]["trigger_mode"] = trigger.read_text().strip() == "1" if trigger.exists() else False
    result["checks"]["gpiochip0"] = (root / "dev/gpiochip0").exists()
    result["checks"]["video_devices"] = len(list((root / "dev").glob("video*"))) >= 2
    result["ready"] = model.startswith("Raspberry Pi 5") and all(value is True for value in result["checks"].values())
    return result


def platform_prepare(root: Path, apply: bool) -> dict:
    model = device_model(root)
    if not model.startswith("Raspberry Pi 5"):
        raise RuntimeError(f"refusing Pi 5 changes on detected model: {model}")
    config = boot_config(root)
    original = config.read_text(encoding="utf-8")
    begin, end = "# BEGIN NUVO DEPTH (managed)\n", "# END NUVO DEPTH (managed)\n"
    block = begin + "[all]\ncamera_auto_detect=0\ndtoverlay=imx296,always-on,cam0\ndtoverlay=imx296,always-on,cam1\n" + end
    if original.count(begin) != original.count(end) or original.count(begin) > 1:
        raise RuntimeError("repair malformed Nuvo boot configuration markers before proceeding")
    outside = original
    if begin in original:
        start = original.index(begin); finish = original.index(end, start) + len(end)
        outside = original[:start] + original[finish:]
        updated = original[:start] + block + original[finish:]
    else:
        updated = original + ("" if original.endswith("\n") else "\n") + "\n" + block
    for line in outside.splitlines():
        active = line.split("#", 1)[0].strip()
        if re.match(r"include\s", active) or re.match(r"dtoverlay\s*=\s*(imx\d|ov\d|arducam)", active, re.I):
            raise RuntimeError("resolve existing camera overlays/includes before Nuvo platform preparation")
    modprobe = root / "etc/modprobe.d/nuvo-depth-imx296.conf"
    changes = {"boot_config":str(config), "boot_config_changed":updated != original,
               "modprobe_config":str(modprobe), "modprobe_changed":not modprobe.exists() or modprobe.read_text() != "options imx296 trigger_mode=1\n",
               "reboot_required":updated != original or not modprobe.exists() or modprobe.read_text() != "options imx296 trigger_mode=1\n"}
    if apply:
        backup = config.with_name(config.name + ".nuvo-backup")
        if not backup.exists(): atomic_write(backup, original, 0o600)
        if updated != original: atomic_write(config, updated, 0o644)
        if changes["modprobe_changed"]: atomic_write(modprobe, "options imx296 trigger_mode=1\n", 0o644)
    return changes


def provision_identity(root: Path, hostname: str) -> dict:
    identity = root / "var/lib/nuvo-depth/identity"
    identity.mkdir(parents=True, exist_ok=True)
    device_uuid = identity / "device.uuid"; token = identity / "claim.token"
    key = identity / "device.key"; certificate = identity / "device.crt"
    if not device_uuid.exists(): atomic_write(device_uuid, str(uuid.uuid4()) + "\n", 0o640)
    if not token.exists(): atomic_write(token, secrets.token_hex(32) + "\n", 0o640)
    if not key.exists() or not certificate.exists():
        openssl = shutil.which("openssl")
        if not openssl: raise RuntimeError("openssl is required to provision the device certificate")
        subprocess.run([openssl,"req","-x509","-newkey","rsa:3072","-nodes","-days","825",
                        "-subj",f"/CN={hostname}","-addext",f"subjectAltName=DNS:{hostname}",
                        "-keyout",str(key),"-out",str(certificate)],check=True,
                       stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        os.chmod(key,0o640); os.chmod(certificate,0o644); fsync_dir(identity)
    if root == Path("/"):
        try:
            import grp
            gateway_group = grp.getgrnam("nuvo-gateway").gr_gid
            video_group = grp.getgrnam("nuvo-video").gr_gid
            os.chown(device_uuid, 0, video_group)
            for path in (token, key, certificate):
                os.chown(path, 0, gateway_group)
            os.chmod(device_uuid, 0o640)
            os.chmod(token, 0o640)
            os.chmod(key, 0o640)
            os.chmod(certificate, 0o644)
        except KeyError:
            raise RuntimeError(
                "nuvo-gateway and nuvo-video groups must exist before identity provisioning"
            )
    fingerprint = hashlib.sha256(certificate.read_bytes()).hexdigest()
    return {"device_uuid":device_uuid.read_text().strip(),
            "certificate_sha256":fingerprint,
            "claim_token_path":str(token), "created_or_verified":True}


def stage_transport(root: Path, mode: str, physical_confirmation: bool) -> dict:
    if mode not in {"auto","wifi","ethernet","usb","both","off"}:
        raise ValueError("invalid transport preference")
    if mode == "off" and not physical_confirmation:
        raise RuntimeError("transport off requires --physical-confirmation")
    config = root / "etc/nuvo-depth/transport.json"
    previous = json.loads(config.read_text()) if config.exists() else {"preference":"auto"}
    transaction = root / "var/lib/nuvo-depth/transactions/network-pending.json"
    now = time.time_ns()
    staged = {"schema_version":1,"preference":mode,"previous":previous.get("preference","auto"),
              "staged_unix_ns":now,"expires_unix_ns":now+60_000_000_000,"confirmed":False}
    atomic_write(
        config,
        json.dumps({"schema_version":1,"preference":mode},sort_keys=True)+"\n",
        0o644,
    )
    atomic_write(transaction,json.dumps(staged,sort_keys=True)+"\n",0o600)
    return staged


def confirm_transport(root: Path) -> dict:
    transaction = root / "var/lib/nuvo-depth/transactions/network-pending.json"
    value = json.loads(transaction.read_text())
    if time.time_ns() > value["expires_unix_ns"]: raise RuntimeError("confirmation window expired; run transport rollback")
    value["confirmed"] = True; value["confirmed_unix_ns"] = time.time_ns()
    atomic_write(transaction,json.dumps(value,sort_keys=True)+"\n",0o600)
    return value


def rollback_transport(root: Path) -> dict:
    transaction = root / "var/lib/nuvo-depth/transactions/network-pending.json"
    value = json.loads(transaction.read_text())
    restored = {"schema_version":1,"preference":value["previous"]}
    atomic_write(
        root/"etc/nuvo-depth/transport.json",
        json.dumps(restored,sort_keys=True)+"\n",
        0o644,
    )
    value["rolled_back_unix_ns"] = time.time_ns(); atomic_write(transaction,json.dumps(value,sort_keys=True)+"\n",0o600)
    return restored


def reconcile_transport(root: Path) -> dict:
    transaction = root / "var/lib/nuvo-depth/transactions/network-pending.json"
    config = root / "etc/nuvo-depth/transport.json"
    if not transaction.exists():
        current = json.loads(config.read_text()) if config.exists() else {"schema_version":1,"preference":"auto"}
        return {"status":"no_pending_change","preference":current.get("preference","auto")}
    value = json.loads(transaction.read_text())
    if value.get("confirmed") is True:
        return {"status":"confirmed","preference":value["preference"]}
    if value.get("rolled_back_unix_ns") is not None:
        return {"status":"rolled_back","preference":value["previous"]}
    if time.time_ns() <= value["expires_unix_ns"]:
        return {"status":"pending","preference":value["preference"],"expires_unix_ns":value["expires_unix_ns"]}
    restored = rollback_transport(root)
    return {"status":"rolled_back","preference":restored["preference"]}


def validate_encoding(value: dict) -> dict:
    if not isinstance(value,dict): raise ValueError("encoding configuration must be an object")
    expected={"schema_version","mode","codec","source","width","height","cadence_hz",
              "bitrate_bps","jpeg_quality","keyframe_interval","threads","low_latency","preset"}
    if set(value)!=expected: raise ValueError("encoding configuration fields are incomplete or unknown")
    if value["schema_version"]!=1: raise ValueError("encoding schema_version must be 1")
    if value["mode"] not in {"raw","encoded","both"}: raise ValueError("invalid encoding mode")
    if value["codec"] not in {"h264","jpeg"}: raise ValueError("invalid encoding codec")
    if value["source"] not in {"left_rectified","right_rectified","left_color","right_color","disparity","depth","left_mono","right_mono","left_rgb","right_rgb","left_rectified_rgb","right_rectified_rgb","rectified_stereo","rectified_stereo_rgb"}: raise ValueError("invalid encoding source")
    for name in ("width","height","cadence_hz","bitrate_bps","jpeg_quality","keyframe_interval","threads"):
        if isinstance(value[name],bool) or not isinstance(value[name],int): raise ValueError(f"encoding {name} must be an integer")
    width,height=value["width"],value["height"]
    if (width==0)!=(height==0) or width<0 or height<0 or width>1920 or height>1080 or (width and (width%2 or height%2)):
        raise ValueError("encoding dimensions must be native or even and at most 1920x1080")
    if not 1<=value["cadence_hz"]<=60: raise ValueError("encoding cadence_hz must be in 1..60")
    if not 100_000<=value["bitrate_bps"]<=50_000_000: raise ValueError("encoding bitrate is out of range")
    if not 1<=value["jpeg_quality"]<=100: raise ValueError("encoding JPEG quality is out of range")
    if not 1<=value["keyframe_interval"]<=600: raise ValueError("encoding keyframe interval is out of range")
    if value["mode"]!="raw" and value["codec"]=="h264" and value["keyframe_interval"]!=1:
        raise ValueError("latest-frame H.264 delivery requires keyframe_interval=1")
    if not 1<=value["threads"]<=4: raise ValueError("encoding thread count is out of range")
    if not isinstance(value["low_latency"],bool): raise ValueError("encoding low_latency must be boolean")
    if value["preset"] not in {"ultrafast","superfast","veryfast","faster","fast"}: raise ValueError("invalid encoding preset")
    return value


def set_encoding(root: Path, value: dict, expected_revision: str | None = None) -> dict:
    validated=validate_encoding(value)
    return write_configuration(root,"encoding.json",validated,expected_revision)

STEREO_FIELDS={"schema_version","algorithm","sgbm_mode","min_disparity","search_range",
               "max_disparity","block_size","uniqueness_ratio",
               "speckle_window_size","speckle_range","disparity_12_max_diff",
               "pre_filter_cap","bm_pre_filter_size","bm_texture_threshold",
               "specular_threshold","median_filter_size","opencv_threads"}

def validate_stereo(value: dict) -> dict:
    if not isinstance(value,dict) or set(value)!=STEREO_FIELDS:
        raise ValueError("stereo configuration fields are incomplete or unknown")
    if type(value["schema_version"]) is not int or value["schema_version"]!=1:
        raise ValueError("unsupported stereo schema")
    if value["algorithm"] not in {"sgbm","bm"}:
        raise ValueError("algorithm must be sgbm or bm")
    if value["sgbm_mode"] not in {"3way","standard","hh4","hh"}:
        raise ValueError("sgbm_mode must be 3way, standard, hh4, or hh")
    for name in STEREO_FIELDS-{"schema_version","algorithm","sgbm_mode"}:
        if type(value[name]) is not int: raise ValueError(f"{name} must be an integer")
    def bounded(name: str, minimum: int, maximum: int) -> None:
        if not minimum<=value[name]<=maximum: raise ValueError(f"{name} must be in {minimum}..{maximum}")
    bounded("min_disparity",0,512)
    bounded("search_range",16,512)
    if value["search_range"]%16: raise ValueError("search_range must be a multiple of 16")
    if value["min_disparity"]+value["search_range"]>=728:
        raise ValueError("search interval leaves no image region")
    bounded("max_disparity",1,727)
    if not value["min_disparity"]<value["max_disparity"]<=value["min_disparity"]+value["search_range"]:
        raise ValueError("max_disparity must lie inside the search range")
    bounded("block_size",5,31)
    if value["block_size"]%2==0: raise ValueError("block_size must be odd")
    bounded("uniqueness_ratio",0,100)
    bounded("speckle_window_size",0,10000)
    bounded("speckle_range",0,256)
    bounded("disparity_12_max_diff",-1,256)
    bounded("pre_filter_cap",1,63)
    bounded("bm_pre_filter_size",5,255)
    if value["bm_pre_filter_size"]%2==0: raise ValueError("bm_pre_filter_size must be odd")
    bounded("bm_texture_threshold",0,255)
    bounded("specular_threshold",1,256)
    if value["median_filter_size"] not in {0,3,5}:
        raise ValueError("median_filter_size must be 0, 3, or 5")
    bounded("opencv_threads",1,4)
    return value


def set_stereo(root: Path, value: dict, expected_revision: str | None = None) -> dict:
    validated=validate_stereo(value)
    return write_configuration(root,"stereo.json",validated,expected_revision)

EXPOSURE_FIELDS={"schema_version","mode","manual_exposure_us",
                 "manual_analogue_gain","red_gain","blue_gain",
                 "auto_target_median","auto_min_median","auto_max_median",
                 "auto_min_exposure_us","auto_max_exposure_us",
                 "auto_min_analogue_gain","auto_max_analogue_gain",
                 "auto_update_interval_frames","auto_settle_frames_after_update"}

def validate_exposure(value: dict) -> dict:
    if not isinstance(value,dict) or set(value)!=EXPOSURE_FIELDS:
        raise ValueError("exposure configuration fields are incomplete or unknown")
    if type(value["schema_version"]) is not int or value["schema_version"]!=1:
        raise ValueError("unsupported exposure schema")
    if value["mode"] not in {"manual","auto"}:
        raise ValueError("exposure mode must be manual or auto")
    integer_fields={"manual_exposure_us","auto_min_exposure_us","auto_max_exposure_us",
                    "auto_update_interval_frames","auto_settle_frames_after_update"}
    number_fields=EXPOSURE_FIELDS-{"schema_version","mode"}-integer_fields
    if any(type(value[name]) is not int for name in integer_fields):
        raise ValueError("exposure times and cadences must be integers")
    if any(isinstance(value[name],bool) or not isinstance(value[name],(int,float))
           for name in number_fields):
        raise ValueError("exposure gains and targets must be numbers")
    if not 250<=value["manual_exposure_us"]<=8000:
        raise ValueError("manual exposure must be in 250..8000 microseconds")
    if not 1.0<=value["manual_analogue_gain"]<=3.0:
        raise ValueError("manual analogue gain must be in 1..3")
    if not 0.1<=value["red_gain"]<=16.0 or not 0.1<=value["blue_gain"]<=16.0:
        raise ValueError("colour gains must be in 0.1..16")
    if not 1<=value["auto_min_median"]<=value["auto_target_median"]<=value["auto_max_median"]<=254:
        raise ValueError("auto median target/deadband is invalid")
    if not 250<=value["auto_min_exposure_us"]<=value["auto_max_exposure_us"]<=8000:
        raise ValueError("auto exposure range is invalid")
    if not 1.0<=value["auto_min_analogue_gain"]<=value["auto_max_analogue_gain"]<=3.0:
        raise ValueError("auto gain range is invalid")
    if not 1<=value["auto_update_interval_frames"]<=120:
        raise ValueError("auto update interval must be in 1..120")
    if not 0<=value["auto_settle_frames_after_update"]<=120:
        raise ValueError("auto settle frames must be in 0..120")
    return value


def set_exposure(root: Path, value: dict, expected_revision: str | None = None) -> dict:
    validated=validate_exposure(value)
    return write_configuration(root,"exposure.json",validated,expected_revision)

def usb_identity(root: Path) -> tuple[str, str, str]:
    serial_path=root/"var/lib/nuvo-depth/identity/device.uuid"
    if not serial_path.exists(): raise RuntimeError("provision device identity before USB configuration")
    serial=serial_path.read_text().strip()
    def mac(role: str) -> str:
        value=bytearray(hashlib.sha256((serial+role).encode()).digest()[:6])
        value[0]=(value[0]|2)&0xfe
        return ":".join(f"{byte:02x}" for byte in value)
    return serial,mac("usb-device"),mac("usb-host")


def usb_plan(root: Path) -> dict:
    udc_root=root/"sys/class/udc"
    udcs=sorted(path.name for path in udc_root.iterdir()) if udc_root.exists() else []
    config_root=root/"sys/kernel/config/usb_gadget"
    active=[]
    if config_root.exists():
        for gadget in config_root.iterdir():
            assignment=gadget/"UDC"
            if assignment.exists() and assignment.read_text().strip(): active.append(gadget.name)
    return {"schema_version":1,"udcs":udcs,"active_gadgets":active,
            "configfs_available":config_root.is_dir(),
            "ready":len(udcs)==1 and not active}


def _attribute(path: Path, value: str) -> None:
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(value+"\n",encoding="ascii")


def usb_configure(root: Path, vid: str, pid: str, electrical_approval: bool,
                  apply: bool) -> dict:
    import re
    if not electrical_approval: raise RuntimeError("USB gadget mode requires --electrical-approval")
    if not re.fullmatch(r"0x[0-9a-fA-F]{4}",vid) or not re.fullmatch(r"0x[0-9a-fA-F]{4}",pid):
        raise ValueError("VID and PID must be explicit four-digit hexadecimal values")
    plan=usb_plan(root)
    if len(plan["udcs"])!=1 or not plan["configfs_available"]:
        raise RuntimeError("exactly one UDC and mounted configfs are required")
    if any(name!="nuvo_depth" for name in plan["active_gadgets"]):
        raise RuntimeError("another USB gadget manager is active; refusing a conflict")
    serial,device_mac,host_mac=usb_identity(root)
    result={"schema_version":1,"vid":vid.lower(),"pid":pid.lower(),"serial":serial,
            "device_mac":device_mac,"host_mac":host_mac,"function":"ecm",
            "udc":plan["udcs"][0],"electrical_approval":True,"applied":False}
    if not apply: return result
    gadget=root/"sys/kernel/config/usb_gadget/nuvo_depth"
    gadget.mkdir(parents=True,exist_ok=True)
    udc=gadget/"UDC"
    if udc.exists() and udc.read_text().strip(): _attribute(udc,"")
    _attribute(gadget/"idVendor",vid); _attribute(gadget/"idProduct",pid)
    _attribute(gadget/"bcdUSB","0x0200"); _attribute(gadget/"bcdDevice","0x0100")
    strings=gadget/"strings/0x409"; strings.mkdir(parents=True,exist_ok=True)
    _attribute(strings/"serialnumber",serial); _attribute(strings/"manufacturer","NuvoHardware")
    _attribute(strings/"product","Nuvo Depth")
    function=gadget/"functions/ecm.usb0"; function.mkdir(parents=True,exist_ok=True)
    _attribute(function/"dev_addr",device_mac); _attribute(function/"host_addr",host_mac)
    config=gadget/"configs/c.1"; config.mkdir(parents=True,exist_ok=True)
    config_strings=config/"strings/0x409"; config_strings.mkdir(parents=True,exist_ok=True)
    _attribute(config_strings/"configuration","Nuvo Depth ECM")
    link=config/"ecm.usb0"
    if not link.exists(): os.symlink("../../functions/ecm.usb0",link)
    _attribute(udc,result["udc"])
    saved=root/"etc/nuvo-depth/usb-gadget.json"
    atomic_write(saved,json.dumps(result,sort_keys=True)+"\n",0o600)
    result["applied"]=True
    return result


def usb_disable(root: Path, apply: bool) -> dict:
    udc=root/"sys/kernel/config/usb_gadget/nuvo_depth/UDC"
    active=udc.exists() and bool(udc.read_text().strip())
    if apply and active: _attribute(udc,"")
    return {"schema_version":1,"was_active":active,"applied":apply}


def usb_apply_saved(root: Path) -> dict:
    saved=json.loads((root/"etc/nuvo-depth/usb-gadget.json").read_text())
    if saved.get("electrical_approval") is not True:
        raise RuntimeError("saved USB configuration has no electrical approval")
    return usb_configure(root,str(saved["vid"]),str(saved["pid"]),True,True)


def parse() -> argparse.Namespace:
    parser=argparse.ArgumentParser(prog="nuvoctl",description="Manage a Nuvo Depth appliance")
    parser.add_argument("--root",default="/",help=argparse.SUPPRESS)
    commands=parser.add_subparsers(dest="command",required=True)
    platform=commands.add_parser("platform").add_subparsers(dest="operation",required=True)
    platform.add_parser("preflight"); prepare=platform.add_parser("prepare"); prepare.add_argument("--platform",choices=["pi5"],required=True); prepare.add_argument("--apply",action="store_true")
    identity=commands.add_parser("identity").add_subparsers(dest="operation",required=True)
    provision=identity.add_parser("provision"); provision.add_argument("--hostname",default="nuvo-depth.local")
    transport=commands.add_parser("transport").add_subparsers(dest="operation",required=True)
    stage=transport.add_parser("stage"); stage.add_argument("mode",choices=["auto","wifi","ethernet","usb","both","off"]); stage.add_argument("--physical-confirmation",action="store_true")
    transport.add_parser("confirm"); transport.add_parser("rollback"); transport.add_parser("reconcile")
    encoding=commands.add_parser("encoding").add_subparsers(dest="operation",required=True)
    configure_encoding=encoding.add_parser("set")
    configure_encoding.add_argument("--if-match")
    configure_encoding.add_argument("--mode",choices=["raw","encoded","both"],required=True)
    configure_encoding.add_argument("--codec",choices=["h264","jpeg"],required=True)
    configure_encoding.add_argument("--source",choices=["left_rectified","right_rectified","left_color","right_color","disparity","depth","left_mono","right_mono","left_rgb","right_rgb","left_rectified_rgb","right_rectified_rgb","rectified_stereo","rectified_stereo_rgb"],required=True)
    configure_encoding.add_argument("--width",type=int,required=True)
    configure_encoding.add_argument("--height",type=int,required=True)
    configure_encoding.add_argument("--cadence-hz",type=int,required=True)
    configure_encoding.add_argument("--bitrate-bps",type=int,required=True)
    configure_encoding.add_argument("--jpeg-quality",type=int,required=True)
    configure_encoding.add_argument("--keyframe-interval",type=int,required=True)
    configure_encoding.add_argument("--threads",type=int,required=True)
    configure_encoding.add_argument("--low-latency",choices=["true","false"],required=True)
    configure_encoding.add_argument("--preset",choices=["ultrafast","superfast","veryfast","faster","fast"],required=True)
    stereo=commands.add_parser("stereo").add_subparsers(dest="operation",required=True)
    configure_stereo=stereo.add_parser("set")
    configure_stereo.add_argument("--if-match")
    configure_stereo.add_argument("--json",required=True)
    exposure=commands.add_parser("exposure").add_subparsers(dest="operation",required=True)
    configure_exposure=exposure.add_parser("set")
    configure_exposure.add_argument("--if-match")
    configure_exposure.add_argument("--json",required=True)
    usb=commands.add_parser("usb").add_subparsers(dest="operation",required=True)
    usb.add_parser("plan")
    usb.add_parser("apply-saved")
    configure=usb.add_parser("configure"); configure.add_argument("--vid",required=True); configure.add_argument("--pid",required=True); configure.add_argument("--electrical-approval",action="store_true"); configure.add_argument("--apply",action="store_true")
    disable=usb.add_parser("disable"); disable.add_argument("--apply",action="store_true")
    return parser.parse_args()


def main() -> None:
    args=parse(); root=Path(args.root)
    if args.command=="platform" and args.operation=="preflight": result=platform_preflight(root)
    elif args.command=="platform": result=platform_prepare(root,args.apply)
    elif args.command=="identity": result=provision_identity(root,args.hostname)
    elif args.command=="encoding": result=set_encoding(root,{"schema_version":1,"mode":args.mode,"codec":args.codec,"source":args.source,"width":args.width,"height":args.height,"cadence_hz":args.cadence_hz,"bitrate_bps":args.bitrate_bps,"jpeg_quality":args.jpeg_quality,"keyframe_interval":args.keyframe_interval,"threads":args.threads,"low_latency":args.low_latency=="true","preset":args.preset},args.if_match)
    elif args.command=="stereo": result=set_stereo(root,json.loads(args.json),args.if_match)
    elif args.command=="exposure": result=set_exposure(root,json.loads(args.json),args.if_match)
    elif args.command=="usb" and args.operation=="plan": result=usb_plan(root)
    elif args.command=="usb" and args.operation=="configure": result=usb_configure(root,args.vid,args.pid,args.electrical_approval,args.apply)
    elif args.command=="usb" and args.operation=="apply-saved": result=usb_apply_saved(root)
    elif args.command=="usb": result=usb_disable(root,args.apply)
    elif args.operation=="stage": result=stage_transport(root,args.mode,args.physical_confirmation)
    elif args.operation=="confirm": result=confirm_transport(root)
    elif args.operation=="rollback": result=rollback_transport(root)
    else: result=reconcile_transport(root)
    print(json.dumps(result,sort_keys=True,indent=2))


if __name__ == "__main__":
    try:
        main()
    except ConfigurationConflict as error:
        print(json.dumps({"error_code":"precondition_failed","error":str(error)}))
        raise SystemExit(3)
