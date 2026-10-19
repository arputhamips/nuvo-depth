#!/usr/bin/env python3
"""Installed, root-only provisioning/authentication and bounded network bootstrap."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import hmac
import html
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import socket
import ssl
import subprocess
import sys
import threading
import time
import uuid

if Path(__file__).parent == Path("/usr/bin"):
    sys.path.insert(0, "/usr/lib/nuvo-depth")
import nuvoctl as admin

DIRECTORY = "var/lib/nuvo-depth-setup"
STATE = DIRECTORY + "/state.json"
PRIVATE = DIRECTORY + "/private.json"
AUTH = DIRECTORY + "/auth.json"
PROFILE = "etc/NetworkManager/system-connections/nuvo-setup-ap.nmconnection"
STATION = "etc/NetworkManager/system-connections/nuvo-setup-station.nmconnection"
AP_ENABLED = "etc/nuvo-depth/setup-ap-enabled"
ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"


def read(root: Path, name: str, default=None):
    path = root / name
    if not path.exists():
        return default
    if path.is_symlink() or path.parent.is_symlink() or path.stat().st_size > 128 * 1024:
        raise RuntimeError("invalid setup state file")
    if root == Path("/") and (path.stat().st_uid != 0 or path.parent.stat().st_uid != 0):
        raise RuntimeError("setup state must be root-owned")
    return json.loads(path.read_text())


def save(root: Path, name: str, value):
    directory = root / DIRECTORY
    if directory.is_symlink():
        raise RuntimeError("setup directory cannot be a symlink")
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    if root == Path("/") and directory.stat().st_uid != 0:
        raise RuntimeError("setup directory must be owned by root")
    os.chmod(directory, 0o700)
    admin.atomic_write(root / name, json.dumps(value, sort_keys=True) + "\n", 0o600)


@contextmanager
def setup_lock(root):
    # The capture daemon owns /var/lib/nuvo-depth. Root job/auth records must
    # live outside that writable tree, including their persistent lock inode.
    directory = root / DIRECTORY
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    if directory.is_symlink() or root == Path("/") and directory.stat().st_uid != 0:
        raise RuntimeError("setup directory must be owned by root and cannot be a symlink")
    if os.name != "posix":
        with admin.configuration_lock(root):
            yield
        return
    import fcntl
    with (directory / "setup.lock").open("a") as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        try:
            with admin.configuration_lock(root):
                yield
        finally:
            fcntl.flock(handle, fcntl.LOCK_UN)


def command(args, *, check=True, timeout=90):
    result = subprocess.run(args, capture_output=True, text=True, timeout=timeout, check=False)
    if check and result.returncode:
        # Never echo subprocess stderr that might contain credentials.
        raise RuntimeError(f"{Path(args[0]).name} operation failed (exit {result.returncode})")
    return result


def password_record(password):
    if not isinstance(password, str) or not 12 <= len(password) <= 256:
        raise ValueError("Nuvo password must contain 12 through 256 characters")
    salt = secrets.token_hex(16)
    digest = hashlib.scrypt(password.encode(), salt=bytes.fromhex(salt), n=16384, r=8, p=1, dklen=32, maxmem=64*1024*1024)
    return {"salt": salt, "digest": digest.hex()}


def verify_password(password, record):
    digest = hashlib.scrypt(password.encode(), salt=bytes.fromhex(record["salt"]), n=16384, r=8, p=1, dklen=32, maxmem=64*1024*1024)
    return hmac.compare_digest(digest.hex(), record["digest"])


def pairing_digest(code, salt):
    return hashlib.sha256(bytes.fromhex(salt) + code.replace("-", "").upper().encode()).hexdigest()


def issue_pairing(root):
    with setup_lock(root):
        credentials = read(root, AUTH, {"schema_version": 1, "claimed": False})
        private = read(root, PRIVATE, {})
        code = "".join(secrets.choice(ALPHABET) for _ in range(8))
        salt = secrets.token_hex(16)
        credentials.update(pairing={"salt": salt, "digest": pairing_digest(code, salt), "expires_at": None}, failures=0, locked_until=0)
        private["pairing_code"] = code[:4] + "-" + code[4:]
        save(root, AUTH, credentials)
        save(root, PRIVATE, private)
        return private["pairing_code"]


def authenticate(root, secret, now=None):
    if not isinstance(secret, str) or not 1 <= len(secret) <= 256:
        return {"authenticated": False}
    now = time.time() if now is None else now
    with setup_lock(root):
        value = read(root, AUTH)
        if not value or value.get("locked_until", 0) > now:
            return {"authenticated": False}
        if value.get("locked_until", 0):
            value.update(locked_until=0, failures=0)
        accepted = bool(value.get("password") and verify_password(secret, value["password"]))
        pairing = value.get("pairing")
        if not accepted and pairing:
            if pairing.get("expires_at") is None:
                pairing["expires_at"] = now + 600
            if now <= pairing["expires_at"] and hmac.compare_digest(pairing_digest(secret, pairing["salt"]), pairing["digest"]):
                accepted = True
        if accepted:
            value.update(claimed=True, failures=0, locked_until=0)
            value.pop("pairing", None)
            private = read(root, PRIVATE, {})
            private.pop("pairing_code", None)
            save(root, PRIVATE, private)
        else:
            value["failures"] = value.get("failures", 0) + 1
            if value["failures"] >= 5:
                value["locked_until"] = now + 300
        save(root, AUTH, value)
        return {"authenticated": accepted, "needs_owner_password": accepted and not value.get("password")}


def set_owner_password(root, password):
    record = password_record(password)
    with setup_lock(root):
        value = read(root, AUTH)
        if not value or not value.get("claimed"):
            raise ValueError("claim the device before setting its owner password")
        value.update(password=record, failures=0, locked_until=0)
        value.pop("pairing", None)
        save(root, AUTH, value)
        private = read(root, PRIVATE, {})
        private.pop("pairing_code", None)
        save(root, PRIVATE, private)
    return {"owner_configured": True}


def boot_id(root):
    path = root / "proc/sys/kernel/random/boot_id"
    return path.read_text().strip() if path.exists() else "unknown"


def status(root):
    value = read(root, STATE, {})
    auth = read(root, AUTH, {})
    return {**{key: value.get(key) for key in (
        "schema_version", "origin", "access_policy", "access_stage", "display_name", "hostname",
        "ap_ssid", "ap_address", "phase", "reboot_required", "engineering", "calibration_state", "capture_error", "error")},
        "reboot_required": bool(value.get("reboot_required") and value.get("prepared_boot_id") == boot_id(root)),
        "claimed": bool(auth.get("claimed")),
        "needs_owner_password": bool(auth.get("claimed") and not auth.get("password")),
        "shipping_ready": False}


def nm_escape(value):
    if any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise ValueError("network values cannot contain control characters")
    return value.replace("\\", "\\\\").replace(" ", "\\s")


def check_interface(value):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,15}", value):
        raise ValueError("invalid Wi-Fi interface")
    return value


def ap_profile(state, private):
    address = ipaddress.IPv4Interface(state["ap_address"])
    if not address.ip.is_private or address.network.prefixlen != 24:
        raise ValueError("setup AP requires a private /24")
    return (f"[connection]\nid=Nuvo setup\nuuid={state['ap_uuid']}\ntype=wifi\n"
            f"interface-name={check_interface(state['wifi_interface'])}\nautoconnect=false\n\n"
            f"[wifi]\nmode=ap\nband=bg\nchannel=6\nssid={nm_escape(state['ap_ssid'])}\n\n"
            f"[wifi-security]\nkey-mgmt=wpa-psk\nproto=rsn;\npairwise=ccmp;\ngroup=ccmp;\npsk={private['ap_password']}\n\n"
            f"[ipv4]\nmethod=shared\naddress1={address}\nnever-default=true\n\n[ipv6]\nmethod=disabled\n")


def forwarding_rule(state, action, run=command):
    rule = ["FORWARD", "-i", state["wifi_interface"], "-m", "comment", "--comment", "nuvo-setup", "-j", "DROP"]
    if action == "add":
        if run(["iptables", "-C", *rule], check=False).returncode:
            run(["iptables", "-I", "FORWARD", "1", *rule[1:]])
    else:
        run(["iptables", "-D", *rule], check=False)


def apply_configuration(root, plan, run=command):
    """One path for interactive selections and factory defaults; packages exist."""
    with setup_lock(root):
        old = read(root, STATE, {})
        auth = read(root, AUTH, {})
        if plan["factory"] and (auth.get("claimed") or old and old.get("origin") != "factory"):
            raise ValueError("--factory cannot reset an existing customer installation")
        if plan["access_policy"] == "keep_current" and old.get("access_stage") in {"active", "connecting"}:
            raise ValueError("finish the active hotspot handoff before selecting the current network")
        if old.get("access_stage") in {"active", "connecting"} and any(old.get(k) != plan[k] for k in ("ap_address", "wifi_interface")):
            raise ValueError("complete the active network transaction before changing its interface/subnet")
        hostname = (root / "etc/hostname").read_text().strip()
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]{0,62}", hostname):
            raise ValueError("configure a valid OS hostname before installation")
        identity = admin.provision_identity(root, hostname + ".local")
        state = dict(old)
        state.update(schema_version=1, origin=old.get("origin", "factory" if plan["factory"] else "terminal"),
                     access_policy=plan["access_policy"], display_name=plan["display_name"], hostname=hostname,
                     hardware=plan["hardware"], factory_preset_version=plan.get("factory_preset_version"),
                     startup=plan["startup"], engineering=plan["engineering"], phase="configuring",
                     calibration_state="pending", capture_error=None, error=None)
        state.setdefault("ap_uuid", str(uuid.uuid4()))
        state.setdefault("station_uuid", str(uuid.uuid4()))
        state.setdefault("ap_ssid", "Nuvo-Setup-" + identity["device_uuid"][-6:].upper())
        state["wifi_interface"] = check_interface(plan["wifi_interface"])
        state["wifi_country"] = plan["wifi_country"]
        state["ap_address"] = plan["ap_address"]
        private = read(root, PRIVATE, {})
        private.setdefault("ap_password", secrets.token_urlsafe(18))
        if not auth:
            auth = {"schema_version": 1, "claimed": False}
        if plan.get("owner_password"):
            if auth.get("claimed"):
                raise ValueError("use explicit credential recovery to change an existing owner")
            auth.update(password=plan["owner_password"], claimed=True)
            auth.pop("pairing", None)
        if auth.get("claimed"):
            private.pop("pairing_code", None)
        save(root, AUTH, auth)
        save(root, PRIVATE, private)
        if plan["hardware"] == "nuvo":
            changes = admin.platform_prepare(root, True)
            state["reboot_required"] = bool(changes["reboot_required"] or old.get("reboot_required") and old.get("prepared_boot_id") == boot_id(root))
            if changes["reboot_required"]:
                state["prepared_boot_id"] = boot_id(root)
        else:
            state["reboot_required"] = False
        # Gateway setup must not start unconfigured capture through its Wants.
        admin.atomic_write(root / "etc/systemd/system/nuvo-gateway.service.d/20-setup.conf",
                           "[Unit]\nWants=\nWants=nuvo-system-agent.socket\nAfter=\nAfter=network.target nuvo-system-agent.socket\n", 0o644)
        admin.atomic_write(root / "etc/systemd/system/nuvo-depthd-native.service.d/20-setup.conf",
                           "[Unit]\nConditionPathExists=/etc/nuvo-depth/capture-enabled\n", 0o644)
        if plan["access_policy"] == "setup_ap":
            # Current raspi-config persists the country in the boot command
            # line. Do this in the installer, which can write the boot volume,
            # before arming the sandboxed first-boot network service.
            run(["/usr/bin/raspi-config", "nonint", "do_wifi_country", state["wifi_country"]])
            cmdline = admin.boot_config(root).with_name("cmdline.txt")
            configured = re.findall(r"(?:^|\s)cfg80211\.ieee80211_regdom=(\S+)", cmdline.read_text())
            if configured != [state["wifi_country"]]:
                raise RuntimeError("Wi-Fi country was not persisted in the boot command line; check boot volume permissions")
        if plan["access_policy"] == "setup_ap" and (not old or old.get("access_policy") != "setup_ap" or old.get("access_stage") not in {"confirmed", "active"}):
            state.update(access_stage="armed", activate_after_boot_id=boot_id(root), activate_now=False)
            admin.atomic_write(root / PROFILE, ap_profile(state, private), 0o600)
        elif plan["access_policy"] == "keep_current":
            state["access_stage"] = "confirmed"
        save(root, STATE, state)
    if not auth.get("claimed") and not auth.get("pairing"):
        issue_pairing(root)
    # Bundle activation validates signatures/bindings; do not rewrite unit IDs.
    if plan.get("calibration_bundle"):
        run(["/usr/lib/nuvo-depth/nuvo-calibration", "activate", plan["calibration_bundle"], "--namespace", plan["calibration_namespace"]])
    run(["depthd", "--check-config", "/etc/nuvo-depth/daemon.yaml"])
    calibration = json.loads(run(["/usr/lib/nuvo-depth/nuvo-calibration", "provisioning-status"]).stdout)
    capture = plan["hardware"] == "nuvo" and (plan["engineering"] or calibration["accepted"])
    marker = root / "etc/nuvo-depth/capture-enabled"
    if capture:
        admin.atomic_write(marker, "engineering-preview\n" if plan["engineering"] else "accepted-calibration\n", 0o600)
    else:
        marker.unlink(missing_ok=True)
        run(["systemctl", "stop", "nuvo-depthd-native.service"])
    # Provisional beta output requires an explicit terminal choice.
    command_line = "/usr/bin/depthd --run /etc/nuvo-depth/daemon.yaml --status /run/nuvo-depth/status.json --encoding-config /etc/nuvo-depth/encoding.json --stereo-control /etc/nuvo-depth/stereo.json --exposure-control /etc/nuvo-depth/exposure.json --device-identity /var/lib/nuvo-depth/identity/device.uuid"
    admin.atomic_write(root / "etc/systemd/system/nuvo-depthd-native.service.d/30-authority.conf",
                       "[Service]\nExecStart=\nExecStart=" + command_line + (" --allow-provisional-depth" if plan["engineering"] else "") + "\n", 0o644)
    if plan["access_policy"] == "setup_ap":
        admin.atomic_write(root / AP_ENABLED, "enabled\n", 0o600)
    else:
        (root / AP_ENABLED).unlink(missing_ok=True)
        run(["systemctl", "stop", "nuvo-setup.service"])
    run(["systemctl", "daemon-reload"])
    services = ["nuvo-gateway.service", "nuvo-system-agent.socket"]
    if plan["access_policy"] == "setup_ap":
        services.append("nuvo-setup.service")
    if capture:
        services.append("nuvo-depthd-native.service")
    if plan["startup"]:
        run(["systemctl", "enable", "nuvo-depth.target", *services])
    else:
        run(["systemctl", "disable", "nuvo-depth.target", "nuvo-gateway.service", "nuvo-depthd-native.service", "nuvo-setup.service", "nuvo-system-agent.socket"])
    with setup_lock(root):
        state = read(root, STATE)
        state.update(phase="installed", calibration_state="accepted" if calibration["accepted"] else "pending")
        save(root, STATE, state)
    run(["systemctl", "start", "nuvo-system-agent.socket"])
    # An upgrade must replace running binaries and apply the new unit overrides.
    # `start` alone leaves an already-active daemon/gateway on the old code.
    run(["systemctl", "restart", *[s for s in services if s not in {"nuvo-depthd-native.service", "nuvo-system-agent.socket"}]])
    if capture and not state["reboot_required"]:
        started = run(["systemctl", "restart", "nuvo-depthd-native.service"], check=False)
        if started.returncode:
            with setup_lock(root):
                state = read(root, STATE)
                state["capture_error"] = "Capture did not start; inspect nuvo-depthd-native.service. Console/network setup remains available."
                save(root, STATE, state)
    return status(root)


def activate_ap(root, run=command):
    with setup_lock(root):
        state = read(root, STATE)
        if not state or state.get("access_policy") != "setup_ap" or state.get("phase") != "installed":
            raise ValueError("no completed installation has armed a setup AP")
        if state.get("access_stage") == "confirmed":
            return
        old = run(["nmcli", "-g", "GENERAL.CON-UUID", "device", "show", state["wifi_interface"]], check=False).stdout.strip()
        state["previous_wifi_uuid"] = old if re.fullmatch(r"[a-fA-F0-9-]{36}", old) and old != state["ap_uuid"] else state.get("previous_wifi_uuid")
        save(root, STATE, state)
        # Hold the configuration lock through the access switch so an installer
        # cannot start package work between admission and a disruptive NM call.
        run(["iw", "reg", "set", state["wifi_country"]])
        run(["nmcli", "radio", "wifi", "on"])
        forwarding_rule(state, "add", run)
        try:
            run(["nmcli", "connection", "load", "/" + PROFILE])
            run(["nmcli", "--wait", "30", "connection", "up", "uuid", state["ap_uuid"]])
        except Exception:
            forwarding_rule(state, "remove", run)
            if state.get("previous_wifi_uuid"):
                run(["nmcli", "connection", "up", "uuid", state["previous_wifi_uuid"]], check=False)
            raise
        state.update(access_stage="active", activate_now=False, active_boot_id=boot_id(root))
        save(root, STATE, state)


def stage_network(root, ssid, password, run=command):
    if not isinstance(ssid, str) or not 1 <= len(ssid.encode()) <= 32 or not isinstance(password, str) or not (8 <= len(password.encode()) <= 63 or re.fullmatch(r"[a-fA-F0-9]{64}", password)):
        raise ValueError("enter an SSID and WPA2/WPA3 personal password (8–63 bytes or 64 hex digits)")
    ssid, password = nm_escape(ssid), nm_escape(password)
    with setup_lock(root):
        state = read(root, STATE)
        if state.get("phase") != "installed" or state.get("access_stage") != "active":
            raise ValueError("network setup is not active or another transaction is pending")
        private = read(root, PRIVATE)
        ticket = secrets.token_urlsafe(32)
        private["network_ticket"] = ticket
        text = (f"[connection]\nid=Nuvo configured Wi-Fi\nuuid={state['station_uuid']}\ntype=wifi\n"
                f"interface-name={state['wifi_interface']}\nautoconnect=false\n\n"
                f"[wifi]\nmode=infrastructure\nssid={ssid}\n\n[wifi-security]\nkey-mgmt=wpa-psk\npsk={password}\n\n"
                "[ipv4]\nmethod=auto\n\n[ipv6]\nmethod=auto\n")
        admin.atomic_write(root / STATION, text, 0o600)
        state.update(access_stage="connecting", error=None, network_deadline=time.time()+180,
                     network_start_after=time.time()+4, transaction_boot_id=boot_id(root), network_started=False)
        save(root, PRIVATE, private)
        save(root, STATE, state)
        return {"confirm_url": f"http://{state['hostname']}.local/#finish={ticket}", "console_url": f"https://{state['hostname']}.local:7443/"}


def confirm_network(root, ticket, run=command, *, local_address):
    if not isinstance(ticket, str) or not re.fullmatch(r"[A-Za-z0-9_-]{43}", ticket):
        raise ValueError("invalid network confirmation ticket")
    with setup_lock(root):
        state, private = read(root, STATE, {}), read(root, PRIVATE, {})
        expected = private.get("network_ticket")
        if state.get("phase") != "installed" or state.get("access_stage") != "connecting" or not state.get("network_started") or not expected or not hmac.compare_digest(ticket, expected) or time.time() > state["network_deadline"]:
            raise ValueError("network handoff is invalid or expired")
        active = run(["nmcli", "-g", "GENERAL.CON-UUID,GENERAL.STATE,IP4.ADDRESS", "device", "show", state["wifi_interface"]]).stdout.strip().splitlines()
        addresses = {str(ipaddress.IPv4Interface(value).ip) for value in active[2:] if value}
        if len(active) < 2 or active[0] != state["station_uuid"] or active[1].split()[0] != "100" or local_address not in addresses:
            raise ValueError("confirmation must arrive through the active candidate Wi-Fi address")
        run(["nmcli", "connection", "modify", "uuid", state["station_uuid"], "connection.autoconnect", "yes"])
        state.update(access_stage="confirmed", error=None)
        private.pop("network_ticket", None)
        save(root, STATE, state)
        save(root, PRIVATE, private)
    forwarding_rule(state, "remove", run)
    return {"console_url": f"https://{state['hostname']}.local:7443/"}


def reconcile(root, run=command):
    state = read(root, STATE, {})
    if state.get("phase") != "installed":
        return
    current_boot = boot_id(root)
    if state.get("reboot_required") and state.get("prepared_boot_id") != current_boot:
        with setup_lock(root):
            state = read(root, STATE)
            state["reboot_required"] = False
            save(root, STATE, state)
    stage = state.get("access_stage")
    if stage == "connecting":
        if state.get("transaction_boot_id") != current_boot or time.time() > state["network_deadline"]:
            forwarding_rule(state, "add", run)
            run(["nmcli", "connection", "load", "/" + PROFILE])
            run(["nmcli", "connection", "up", "uuid", state["ap_uuid"]])
            with setup_lock(root):
                state, private = read(root, STATE), read(root, PRIVATE)
                state.update(access_stage="active", active_boot_id=current_boot, error="Wi-Fi handoff failed or was not confirmed; previous setup access restored")
                private.pop("network_ticket", None)
                save(root, STATE, state)
                save(root, PRIVATE, private)
        elif not state.get("network_started") and time.time() >= state["network_start_after"]:
            # Persist before a disruptive call, so a service restart cannot
            # turn an interrupted transition into an uncontrolled retry loop.
            with setup_lock(root):
                state = read(root, STATE)
                state["network_started"] = True
                save(root, STATE, state)
            run(["nmcli", "connection", "load", "/" + STATION])
            run(["nmcli", "--wait", "45", "connection", "up", "uuid", state["station_uuid"]], check=False)
    elif stage == "armed" and (state.get("activate_now") or state.get("activate_after_boot_id") != current_boot):
        activate_ap(root, run)
    elif stage == "active" and state.get("active_boot_id") != current_boot:
        activate_ap(root, run)


PAGE = r'''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Nuvo network setup</title>
<style>body{font:17px system-ui;max-width:34rem;margin:8vh auto;padding:1.5rem;background:#101b27;color:#edf6ff}input,button{box-sizing:border-box;width:100%;padding:.8rem;margin:.6rem 0 1rem;font:inherit;border-radius:.5rem}button{background:#68dec5;border:0}label{display:block}#message{white-space:pre-wrap}a{color:#68dec5}</style>
<h1>Connect your Nuvo</h1><p>This private setup network has no internet. Keep this page open while Nuvo joins your Wi-Fi.</p>
<form id="network"><label>Network name (SSID)<input name="ssid" required maxlength="32" autocomplete="off"></label><label>Wi-Fi password<input name="password" type="password" required minlength="8" maxlength="64" autocomplete="off"></label><button>Connect to Wi-Fi</button></form><p id="message" role="status">__MESSAGE__</p>
<script>const nonce=__NONCE__;const form=document.getElementById('network'),message=document.getElementById('message');
async function send(path,value){const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json','X-Nuvo-Setup':nonce},body:JSON.stringify(value)});const d=await r.json();if(!r.ok)throw new Error(d.error||'Setup failed');return d;}
const finish=location.hash.startsWith('#finish=')?location.hash.slice(8):'';if(finish){history.replaceState(null,'',location.pathname);form.hidden=true;send('/confirm',{ticket:finish}).then(d=>{message.textContent='Wi-Fi confirmed. ';const a=document.createElement('a');a.href=d.console_url;a.textContent='Open Nuvo console';message.append(a);}).catch(e=>message.textContent=e.message);}
form.onsubmit=async e=>{e.preventDefault();const values=Object.fromEntries(new FormData(form));try{const d=await send('/network',values);form.reset();form.hidden=true;message.textContent='Reconnect this phone or computer to your home Wi-Fi, then use this link to confirm within three minutes. If it fails, reconnect to the same Nuvo hotspot and retry. ';const a=document.createElement('a');a.href=d.confirm_url;a.textContent='Confirm on the new network';message.append(a);}catch(e){message.textContent=e.message;}};
</script></html>'''


class SetupServer(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 8

    def __init__(self, *args):
        self.slots = threading.BoundedSemaphore(8)
        super().__init__(*args)

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, address)
        except Exception:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()


def make_handler(root):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass  # Network passwords and handoff capabilities never enter logs.

        def handle(self):
            self.connection.settimeout(10)
            super().handle()

        def respond(self, code, data, content_type="application/json"):
            body = data.encode() if isinstance(data, str) else json.dumps(data).encode()
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Referrer-Policy", "no-referrer")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'")
            self.end_headers()
            self.wfile.write(body)

        def context(self):
            state = read(root, STATE, {})
            # DNS names are case-insensitive. Browsers lowercase the hostname
            # in the generated handoff URL even if /etc/hostname uses capitals.
            host = self.headers.get("Host", "").lower()
            if host.endswith(":80"):
                host = host[:-3]
            allowed = {str(ipaddress.IPv4Interface(state.get("ap_address", "192.168.4.1/24")).ip), state.get("hostname", "").lower() + ".local"}
            # Request must target this actual local address/name, not a rebound
            # remote domain. Origin checks are required for every mutation.
            allowed.add(self.connection.getsockname()[0])
            origin = self.headers.get("Origin", "http://" + host).lower()
            if origin.endswith(":80"):
                origin = origin[:-3]
            if host not in allowed or origin != "http://" + host:
                raise ValueError("setup host/origin rejected")
            return state

        def do_GET(self):
            try:
                state = self.context()
                if self.path != "/" or state.get("phase") != "installed" or state.get("access_stage") not in {"active", "connecting"}:
                    self.respond(404, {"error": "Network setup is not active"}); return
                local = self.connection.getsockname()[0]
                ap_ip = str(ipaddress.IPv4Interface(state["ap_address"]).ip)
                if state["access_stage"] == "active" and local != ap_ip:
                    self.respond(403, {"error": "Join the private Nuvo setup network"}); return
                page = PAGE.replace("__NONCE__", json.dumps(self.server.nonce)).replace("__MESSAGE__", html.escape(state.get("error") or ""))
                self.respond(200, page, "text/html; charset=utf-8")
            except ValueError as error:
                self.respond(403, {"error": str(error)})

        def do_POST(self):
            try:
                state = self.context()
                if self.headers.get("X-Nuvo-Setup") != self.server.nonce or self.headers.get("Content-Type") != "application/json":
                    raise ValueError("setup request token rejected")
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 2048:
                    raise ValueError("invalid setup body length")
                value = json.loads(self.rfile.read(length))
                if not isinstance(value, dict):
                    raise ValueError("setup request must be an object")
                local = self.connection.getsockname()[0]
                ap_ip = str(ipaddress.IPv4Interface(state["ap_address"]).ip)
                if self.path == "/network" and local == ap_ip and set(value) == {"ssid", "password"}:
                    result = stage_network(root, value["ssid"], value["password"])
                elif self.path == "/confirm" and local != ap_ip and set(value) == {"ticket"}:
                    result = confirm_network(root, value["ticket"], local_address=local)
                else:
                    raise ValueError("operation is not allowed on this interface")
                self.respond(200, result)
            except (ValueError, RuntimeError, json.JSONDecodeError) as error:
                self.respond(400, {"error": str(error)})
    return Handler


def serve(root):
    server = SetupServer(("0.0.0.0", 80), make_handler(root))
    server.nonce = secrets.token_urlsafe(32)
    server.timeout = 1
    def worker():
        while True:
            try:
                reconcile(root)
            except Exception:
                # Keep serving recovery/status; avoid credential-bearing stderr.
                with setup_lock(root):
                    state = read(root, STATE, {})
                    state["error"] = "Network activation failed; use terminal status/recovery"
                    save(root, STATE, state)
            time.sleep(3)
    threading.Thread(target=worker, daemon=True).start()
    server.serve_forever(poll_interval=1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=["status", "serve", "activate-ap", "pair", "private", "auth"])
    args = parser.parse_args()
    if os.name != "posix" or os.geteuid() != 0:
        parser.error("run this installed administrative helper with sudo on the Pi")
    root = Path("/")
    if args.operation == "serve":
        serve(root); return
    if args.operation == "auth":
        request = json.loads(sys.stdin.read(8193))
        if not isinstance(request, dict):
            raise ValueError("setup request must be an object")
        if request.get("operation") == "setup.authenticate" and set(request) == {"operation", "secret"}:
            result = authenticate(root, request["secret"])
        elif request.get("operation") == "setup.owner" and set(request) == {"operation", "password"}:
            result = set_owner_password(root, request["password"])
        elif request == {"operation": "setup.status"}:
            result = status(root)
        else:
            raise ValueError("invalid setup operation")
    elif args.operation == "pair":
        if not sys.stdout.isatty():
            raise ValueError("pairing secrets require a private interactive terminal")
        result = {"pairing_code": issue_pairing(root)}
    elif args.operation == "private":
        if not sys.stdout.isatty():
            raise ValueError("setup secrets require a private interactive terminal")
        result = read(root, PRIVATE, {})
    elif args.operation == "activate-ap":
        with setup_lock(root):
            state = read(root, STATE)
            if state.get("access_stage") == "confirmed":
                raise ValueError("use an explicit network recovery plan after confirmed setup")
            state["activate_now"] = True
            save(root, STATE, state)
        result = {"activation": "requested", "notice": "Wi-Fi SSH may disconnect after the setup worker activates the AP"}
    else:
        result = status(root)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, RuntimeError, OSError) as error:
        print(json.dumps({"error": str(error)}), file=sys.stderr)
        raise SystemExit(1)
