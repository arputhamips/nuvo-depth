#!/usr/bin/env python3
"""Prompted/factory Nuvo installer; download signed, prebuilt GitHub packages."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import getpass
import ipaddress
import json
import os
from pathlib import Path
import platform
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import uuid
import zipfile

import nuvo_release as release
import nuvo_setup as setup

JOB = setup.DIRECTORY + "/install-job.json"
RESULT = setup.DIRECTORY + "/install-result.json"


def choose(prompt, choices, default, input_fn=input):
    print(prompt)
    for index, (value, label) in enumerate(choices, 1):
        print(f"  {index}. {label}" + (" [default]" if value == default else ""))
    while True:
        answer = input_fn("> ").strip()
        if not answer:
            return default
        if answer.isdigit() and 1 <= int(answer) <= len(choices):
            return choices[int(answer)-1][0]
        print("Select one of the numbered options.")


def yes(prompt, default=False, input_fn=input):
    while True:
        answer = input_fn(prompt + (" [Y/n] " if default else " [y/N] ")).strip().lower()
        if not answer:
            return default
        if answer in {"y", "yes"}:
            return True
        if answer in {"n", "no"}:
            return False


def collect_plan(root, args, input_fn=input, password_fn=getpass.getpass):
    old, auth = setup.read(root, setup.STATE, {}), setup.read(root, setup.AUTH, {})
    if old.get("access_stage") == "connecting":
        raise ValueError("finish the pending Wi-Fi confirmation or wait for hotspot recovery before installing")
    if args.factory and (auth.get("claimed") or old and old.get("origin") != "factory" or not old and (root / "var/lib/nuvo-depth/identity/device.uuid").exists()):
        raise ValueError("--factory is not a reset: an existing customer/legacy installation requires explicit recovery")
    plan = {"schema_version": 1, "factory": args.factory,
            "factory_preset_version": 1 if args.factory else old.get("factory_preset_version"),
            "access_policy": old.get("access_policy", "setup_ap" if args.factory else "keep_current"),
            "display_name": old.get("display_name", "Nuvo Depth"), "hardware": old.get("hardware", "nuvo"),
            "startup": old.get("startup", True), "engineering": old.get("engineering", False),
            "wifi_interface": args.wifi_interface or old.get("wifi_interface", "wlan0"),
            "ap_address": args.ap_address or old.get("ap_address", "192.168.4.1/24"),
            "calibration_bundle": args.calibration_bundle, "calibration_namespace": "factory" if args.factory else "user",
            "reboot": args.reboot, "activate_now": False}
    country = getattr(args, "wifi_country", None) or old.get("wifi_country", "")
    if not country and (root / "etc/default/crda").is_file():
        found = re.search(r'^REGDOMAIN=[\"\']?([A-Z]{2})', (root / "etc/default/crda").read_text(), re.M)
        country = found.group(1) if found else ""
    if not country and root == Path("/") and (root / "usr/bin/raspi-config").is_file():
        country = setup.command(["/usr/bin/raspi-config", "nonint", "get_wifi_country"], check=False).stdout.strip()
    if not args.factory:
        plan["access_policy"] = choose("Network access AFTER installation:", [
            ("keep_current", "Use the current network"),
            ("setup_ap", "Configure Wi-Fi afterward using a protected Nuvo setup hotspot")], plan["access_policy"], input_fn)
        if plan["access_policy"] == "setup_ap" and not getattr(args, "wifi_country", None):
            country = input_fn(f"Country where the Pi will be used (two letters, e.g. US/GB) [{country}]: ").strip() or country
        name = input_fn(f"Device display name [{plan['display_name']}]: ").strip()
        if name:
            plan["display_name"] = name
        plan["hardware"] = choose("Camera assembly:", [
            ("nuvo", "Supported Nuvo Pi 5 / dual-IMX296 head"),
            ("custom", "Other or incomplete assembly (install console; defer capture)")], plan["hardware"], input_fn)
        if not plan["calibration_bundle"]:
            plan["calibration_bundle"] = input_fn("Calibration bundle directory (blank: preserve/defer): ").strip() or None
        if plan["calibration_bundle"]:
            plan["calibration_namespace"] = choose("Calibration authority:", [("user", "User validated"), ("factory", "Signed factory bundle (requires provisioned trust keys)")], "user", input_fn)
        if plan["hardware"] == "nuvo":
            plan["engineering"] = yes("Allow explicitly provisional engineering preview when accepted calibration is absent?", plan["engineering"], input_fn)
        if not auth.get("claimed") and yes("Set the Nuvo owner password now? (Otherwise issue a one-time setup code)", True, input_fn):
            while True:
                password = password_fn("Nuvo password (12+ characters; not your OS password): ")
                again = password_fn("Confirm Nuvo password: ")
                try:
                    if password != again:
                        raise ValueError("passwords do not match")
                    plan["owner_password"] = setup.password_record(password)
                    break
                except ValueError as error:
                    print(error)
                finally:
                    password = again = None
        plan["startup"] = yes("Start Nuvo services at boot? (required for hotspot recovery)", plan["startup"], input_fn)
        if plan["access_policy"] == "setup_ap" and not plan["startup"]:
            raise ValueError("setup hotspot recovery requires boot startup; rerun and enable startup or keep the current network")
        if not args.reboot:
            plan["reboot"] = yes("Restart automatically if camera preparation requires it?", False, input_fn)
        if plan["access_policy"] == "setup_ap" and not plan["reboot"]:
            plan["activate_now"] = yes("Activate the setup hotspot after provisioning? Wi-Fi SSH WILL disconnect.", False, input_fn)
    plan["wifi_country"] = country.upper()
    if plan["access_policy"] == "setup_ap":
        countries = root / "usr/share/zoneinfo/iso3166.tab"
        if not re.fullmatch(r"[A-Z]{2}", plan["wifi_country"]) or countries.is_file() and not any(line.startswith(plan["wifi_country"] + "\t") for line in countries.read_text().splitlines()):
            raise ValueError("set the actual Wi-Fi operating country; factory mode accepts --wifi-country CC or a configured OS country")
    if not isinstance(plan["display_name"], str) or not 1 <= len(plan["display_name"]) <= 80 or any(ord(c) < 32 for c in plan["display_name"]):
        raise ValueError("device name must be 1–80 printable characters")
    setup.check_interface(plan["wifi_interface"])
    address = ipaddress.IPv4Interface(plan["ap_address"])
    if not address.ip.is_private or address.network.prefixlen != 24 or address.ip in {address.network.network_address, address.network.broadcast_address}:
        raise ValueError("AP address must be a usable private IPv4 /24 address")
    if plan["calibration_bundle"]:
        bundle = Path(plan["calibration_bundle"]).resolve()
        if not bundle.is_dir() or not (bundle / "manifest.json").is_file():
            raise ValueError("calibration bundle must be a directory containing manifest.json")
        plan["calibration_bundle"] = str(bundle)
    return plan


def summary(plan):
    return {key: plan[key] for key in ("factory", "access_policy", "display_name", "hardware", "startup", "engineering", "wifi_interface", "wifi_country", "ap_address", "reboot", "activate_now")} | {
        "owner": "password supplied" if plan.get("owner_password") else "preserve existing owner / bounded setup code",
        "calibration": "bundle supplied" if plan.get("calibration_bundle") else "preserve existing / pending"}


def preflight(root, manifest=None, *, architecture=None, kernel=None, run=setup.command):
    model = setup.admin.device_model(root)
    if not model.startswith("Raspberry Pi 5"):
        raise ValueError("run on a supported Raspberry Pi 5; detected: " + model)
    os_values = {}
    for line in (root / "etc/os-release").read_text().splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            os_values[key] = value.strip('"')
    if os_values.get("ID") not in {"debian", "raspbian"} or os_values.get("VERSION_CODENAME") != "trixie":
        raise ValueError("use the published qualified Raspberry Pi OS 64-bit Trixie image")
    architecture = architecture or run(["dpkg", "--print-architecture"]).stdout.strip()
    if architecture != "arm64":
        raise ValueError("64-bit arm64 userspace is required")
    kernel = kernel or platform.release()
    if manifest and kernel not in manifest["platform"]["kernels"]:
        raise ValueError("kernel is outside this release's qualified cohort; use its documented base OS image")
    if not setup.admin.boot_config(root).with_name("cmdline.txt").is_file():
        raise ValueError("Raspberry Pi boot command line is missing; use the qualified base OS image")
    if run(["dpkg", "--audit"]).stdout.strip():
        raise ValueError("repair the existing interrupted dpkg transaction before installing Nuvo")
    if run(["systemctl", "is-active", "NetworkManager.service"], check=False).returncode:
        raise ValueError("the supported image must have NetworkManager active")
    return {"model": model, "architecture": architecture, "kernel": kernel, "codename": "trixie"}


def check_ap(plan, run=setup.command):
    if plan["access_policy"] != "setup_ap":
        return
    capability = run(["nmcli", "-g", "WIFI-PROPERTIES.AP", "device", "show", plan["wifi_interface"]]).stdout.strip()
    if capability.lower() != "yes":
        raise ValueError("the selected Wi-Fi interface/driver cannot provide a setup AP")
    target = ipaddress.IPv4Interface(plan["ap_address"]).network
    routes = json.loads(run(["ip", "-j", "-4", "route", "show"]).stdout)
    for route in routes:
        if route.get("dst") not in {None, "default"} and target.overlaps(ipaddress.IPv4Network(route["dst"], strict=False)):
            if route.get("dev") != plan["wifi_interface"] or route.get("prefsrc") != str(ipaddress.IPv4Interface(plan["ap_address"]).ip):
                raise ValueError("setup AP subnet overlaps an existing route; choose a free --ap-address /24")
    if run(["systemctl", "is-active", "nuvo-setup.service"], check=False).returncode:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
                probe.bind(("0.0.0.0", 80))
        except OSError:
            raise ValueError("setup hotspot requires free TCP port 80; resolve the existing listener first") from None


def stage_installer(target):
    """Persist trusted code before package work, so loss of SSH is harmless."""
    source = Path(__file__)
    if source.parent.suffix == ".pyz":
        shutil.copyfile(source.parent, target)
    else:
        with zipfile.ZipFile(target, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for name in ("nuvo_install.py", "nuvo_setup.py", "nuvo_release.py", "nuvoctl.py"):
                archive.write(source.parent / name, name)
            archive.writestr("__main__.py", "from nuvo_install import main\nmain()\n")
            for name in ("LICENSE", "NOTICE"):
                archive.write(source.parent.parent / name, name)
    os.chmod(target, 0o600)


@contextmanager
def install_lock(root, wait=False):
    # A separate package-job lock avoids recursively acquiring the existing
    # configuration lock inside package/provisioning helpers.
    import fcntl
    path = root / setup.DIRECTORY / "install.lock"
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    with path.open("a") as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | (0 if wait else fcntl.LOCK_NB))
        except BlockingIOError:
            raise RuntimeError("another Nuvo install is already running") from None
        try:
            yield
        finally:
            fcntl.flock(handle, fcntl.LOCK_UN)


def install_worker(root, job, run=setup.command):
    plan = job["plan"]
    owns_phase = False
    with install_lock(root, wait=True):
        try:
            manifest, packages = release.acquire_release(Path(job["bundle"]), Path(job["public_key"]), job["repository"], job["tag"], bundle=job["bundle"])
            preflight(root, manifest, run=run)
            check_ap(plan, run)
            with setup.setup_lock(root):
                state = setup.read(root, setup.STATE, {})
                if state.get("access_stage") == "connecting":
                    raise ValueError("finish the pending Wi-Fi confirmation before installing")
                state.update(schema_version=1, phase="installing", origin=state.get("origin", "factory" if plan["factory"] else "terminal"))
                setup.save(root, setup.STATE, state)
                owns_phase = True
            for item in manifest["files"]:
                if not item["name"].endswith(".deb"):
                    continue
                installed = run(["dpkg-query", "-W", "-f=${Version}", item["package"]], check=False)
                if installed.returncode == 0 and run(["dpkg", "--compare-versions", installed.stdout.strip(), "gt", item["version"]], check=False).returncode == 0:
                    raise ValueError("refusing an implicit downgrade of an installed Nuvo package")
            run(["apt-get", "update"], timeout=600)
            package_args = [str(path) for path in packages]
            run(["apt-get", "--simulate", "--no-remove", "--reinstall", "install", *package_args], timeout=120)
            # Install dependencies using the supported OS sources; Nuvo's exact
            # packages are the hash-verified local files from GitHub Releases.
            os.environ["DEBIAN_FRONTEND"] = "noninteractive"
            run(["apt-get", "--yes", "--no-remove", "--reinstall", "-o", "Dpkg::Options::=--force-confdef",
                 "-o", "Dpkg::Options::=--force-confold", "install", *package_args], timeout=1800)
            result = setup.apply_configuration(root, plan, run)
            setup.save(root, RESULT, {"status": "complete", "result": result, "tag": manifest["tag"]})
        except Exception as error:
            # Error messages contain only operation names and validated public
            # metadata. No owner password/AP secret is serialized here.
            setup.save(root, RESULT, {"status": "failed", "error": str(error)})
            if owns_phase:
                with setup.setup_lock(root):
                    state = setup.read(root, setup.STATE, {})
                    state.update(phase="failed", error="Installation interrupted; inspect installer status and rerun")
                    setup.save(root, setup.STATE, state)
            raise


def print_access(root):
    state, private = setup.read(root, setup.STATE, {}), setup.read(root, setup.PRIVATE, {})
    print(f"Nuvo console: https://{state.get('hostname', 'HOSTNAME')}.local:7443/")
    certificate = root / "var/lib/nuvo-depth/identity/device.crt"
    if certificate.exists():
        der = setup.ssl.PEM_cert_to_DER_cert(certificate.read_text())
        print("Device certificate SHA-256: " + setup.hashlib.sha256(der).hexdigest())
    print("Private setup material: /" + setup.PRIVATE + " (root-only).")
    if sys.stdout.isatty():
        if state.get("access_policy") == "setup_ap":
            print(f"Setup Wi-Fi: {state.get('ap_ssid')}  Password: {private.get('ap_password')}")
            print("Network setup: http://" + str(ipaddress.IPv4Interface(state["ap_address"]).ip) + "/")
        if private.get("pairing_code"):
            print("One-time setup code: " + private["pairing_code"])
    print("Status: sudo nuvo-setup status")
    print("Reissue an expired setup code: sudo nuvo-setup pair")
    print("Self-signed device trust must be verified using the fingerprint before entering credentials.")


def parse():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--factory", action="store_true", help="manufacturing preset, no selection prompts; never reset an owned device")
    parser.add_argument("--repository", default=release.DEFAULT_REPOSITORY, help="GitHub release repository (default: arputhamips/nuvo-depth)")
    parser.add_argument("--release", help="exact GitHub release tag; no latest alias")
    parser.add_argument("--public-key", type=Path, help="independently trusted Ed25519 release public key")
    parser.add_argument("--bundle", type=Path, help="extracted signed release; OS dependencies still require APT/cache")
    parser.add_argument("--calibration-bundle", help="existing matching calibration bundle directory")
    parser.add_argument("--wifi-interface", help="Wi-Fi interface (preserve existing; default wlan0)")
    parser.add_argument("--wifi-country", help="actual two-letter operating country; required factory input if OS country is unset")
    parser.add_argument("--ap-address", help="AP address (preserve existing; default 192.168.4.1/24)")
    parser.add_argument("--reboot", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--status", action="store_true")
    parser.add_argument("--worker", type=Path, help=argparse.SUPPRESS)
    return parser.parse_args()


def main():
    args = parse()
    root = Path("/")
    try:
        if os.name != "posix" or os.geteuid() != 0:
            raise ValueError("run with sudo on the Raspberry Pi (use --help for instructions)")
        if args.status:
            print(json.dumps(setup.status(root), indent=2)); return
        if args.worker:
            if args.worker.resolve() != root / JOB:
                raise ValueError("worker accepts only its fixed root-owned job record")
            install_worker(root, setup.read(root, JOB)); return
        if not args.factory and not sys.stdin.isatty():
            raise ValueError("normal setup needs an interactive terminal; --factory is only for manufacturing")
        print(json.dumps(preflight(root), indent=2))
        plan = collect_plan(root, args)
        check_ap(plan)
        print(json.dumps(summary(plan), indent=2))
        if not args.factory and not yes("Apply this installation plan?", False):
            print("Cancelled; no device configuration changed."); return
        with tempfile.TemporaryDirectory(prefix="nuvo-release-") as temporary:
            temporary = Path(temporary)
            key = temporary / "release.pub"
            if args.public_key:
                shutil.copyfile(args.public_key, key)
            elif release.DEFAULT_PUBLIC_KEY:
                key.write_text(release.DEFAULT_PUBLIC_KEY)
            else:
                raise ValueError("TODO(RELEASE_TRUST): configure the pinned public key or pass --public-key")
            manifest, packages = release.acquire_release(temporary / "release", key, args.repository or None, args.release, args.bundle)
            print(json.dumps(preflight(root, manifest), indent=2))
            if args.dry_run:
                print("Dry run: signed release verified; no package/platform/service changes applied."); return
            if shutil.disk_usage("/").free < sum(p.stat().st_size for p in packages) * 5 + 512 * 1024**2:
                raise ValueError("insufficient free space for packages, unpacking and recovery")
            # Stage verified package bytes in a root-only directory, preventing
            # modification of user-supplied offline files before APT opens them.
            directory = root / setup.DIRECTORY
            directory.mkdir(parents=True, exist_ok=True, mode=0o700)
            os.chmod(directory, 0o700)
            cache = directory / ("release-" + str(uuid.uuid4()))
            cache.mkdir(mode=0o700)
            source = Path(args.bundle).resolve() if args.bundle else temporary / "release"
            for name in ("manifest.json", "manifest.sig"):
                shutil.copyfile(source / name, cache / name)
            for package in packages:
                shutil.copyfile(package, cache / package.name)
            shutil.copyfile(key, cache / "release.pub")
            if plan.get("calibration_bundle"):
                destination = cache / "calibration"
                # Validate/import uses a private copy; never execute bundle code.
                if any(p.is_symlink() for p in Path(plan["calibration_bundle"]).rglob("*")):
                    raise ValueError("calibration input cannot contain symlinks")
                shutil.copytree(plan["calibration_bundle"], destination)
                plan["calibration_bundle"] = str(destination)
            worker = cache / "installer.pyz"
            stage_installer(worker)
            with install_lock(root):
                active = setup.command(["systemctl", "is-active", "nuvo-install.service"], check=False).stdout.strip()
                if active in {"active", "activating", "reloading"}:
                    raise RuntimeError("another Nuvo installer service is active; inspect its status before retrying")
                setup.save(root, RESULT, {"status": "pending"})
                setup.save(root, JOB, {"plan": plan, "bundle": str(cache), "public_key": str(cache/"release.pub"), "repository": manifest["repository"], "tag": manifest["tag"]})
                setup.command(["systemd-run", "--unit=nuvo-install", "--collect", "--property=Type=exec",
                               "/usr/bin/python3", str(worker), "--worker", "/" + JOB])
            print("Installation runs in nuvo-install.service and survives SSH disconnect. Logs: journalctl -u nuvo-install")
            while True:
                receipt = setup.read(root, RESULT, {})
                if receipt.get("status") in {"complete", "failed"}:
                    break
                active = setup.command(["systemctl", "is-active", "nuvo-install.service"], check=False).stdout.strip()
                if active not in {"active", "activating", "reloading"}:
                    receipt = setup.read(root, RESULT, {})
                    if receipt.get("status") not in {"complete", "failed"}:
                        raise RuntimeError("installer stopped without a receipt; inspect journalctl -u nuvo-install and rerun")
                    break
                time.sleep(1)
            if receipt["status"] != "complete":
                raise RuntimeError(receipt.get("error", "installation failed"))
        print_access(root)
        state = setup.read(root, setup.STATE)
        if state.get("reboot_required") and plan["reboot"]:
            print("Provisioning complete. Restarting; the selected access policy applies after boot.")
            setup.command(["systemctl", "reboot"])
        elif state.get("reboot_required"):
            print("Camera changes need a reboot: sudo reboot")
        elif plan["activate_now"]:
            with setup.setup_lock(root):
                state["activate_now"] = True
                setup.save(root, setup.STATE, state)
            print("Provisioning complete. Setup hotspot activation requested; Wi-Fi SSH may disconnect.")
        elif state.get("access_stage") == "armed":
            print("AP armed for next boot; activate deliberately with: sudo nuvo-setup activate-ap")
        print("Installation does not certify electrical/metric calibration acceptance or shipping readiness.")
    except (ValueError, RuntimeError, OSError, subprocess.SubprocessError) as error:
        print("Nuvo install: " + str(error), file=sys.stderr)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
