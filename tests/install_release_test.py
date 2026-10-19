"""Real Ed25519 signatures and Debian fixtures; no package installation/network."""
import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build_install_release as builder
import nuvo_release as release
import nuvo_install as installer
import nuvo_setup as setup


@unittest.skipUnless(shutil.which("dpkg-deb") and shutil.which("openssl"), "requires Linux Debian fixture tools")
class ReleaseTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temp.name)
        cls.addClassCleanup(cls.temp.cleanup)
        packages = cls.directory / "packages"
        packages.mkdir()
        cls.key = cls.directory / "private.key"
        subprocess.run(["openssl","genpkey","-algorithm","ED25519","-out",str(cls.key)], check=True, capture_output=True)
        for package in release.RUNTIME_PACKAGES:
            tree = cls.directory / package / "DEBIAN"
            tree.mkdir(parents=True)
            (tree / "control").write_text(f"Package: {package}\nVersion: 1.2.3\nArchitecture: arm64\nMaintainer: Test <test@example.com>\nDescription: Installer fixture\n")
            subprocess.run(["dpkg-deb","--build","--root-owner-group",str(tree.parent),str(packages / (package+"_1.2.3_arm64.deb"))], check=True, capture_output=True)
        cls.platform = {"architecture":"arm64","codename":"trixie","kernels":["test-kernel"],"base_image":{"url":"https://downloads.raspberrypi.com/fixture.img.xz","sha256":"a"*64}}
        cls.assets = cls.directory / "release"
        cls.manifest = builder.build(ROOT, packages, cls.assets, "test-owner/nuvo", "v1.2.3", "1.2.3", cls.platform, cls.key)

    def test_real_signed_release_and_dpkg_metadata(self):
        manifest, packages = release.acquire_release(self.directory, self.assets/"release.pub", bundle=self.assets)
        self.assertEqual(len(packages), 8)
        self.assertEqual(manifest["tag"], "v1.2.3")
        with zipfile.ZipFile(self.assets/"nuvo-install.pyz") as archive:
            defaults = archive.read("nuvo_release.py").decode()
            self.assertIn("DEFAULT_REPOSITORY = 'test-owner/nuvo'", defaults)
            self.assertIn("BEGIN PUBLIC KEY", defaults)
            self.assertEqual(archive.read("LICENSE"), (ROOT/"LICENSE").read_bytes())
            self.assertEqual(archive.read("NOTICE"), (ROOT/"NOTICE").read_bytes())
        for name in ("LICENSE","NOTICE"):
            self.assertEqual((self.assets/name).read_bytes(),(ROOT/name).read_bytes())
            release.check_asset(self.assets/name,next(f for f in manifest["files"] if f["name"]==name))
        help_text = subprocess.check_output([sys.executable, str(self.assets/"nuvo-install.pyz"), "--help"], text=True)
        self.assertIn("--factory", help_text)
        self.assertNotIn(self.key.read_text(), (self.assets/"install-nuvo-depth.sh").read_text())
        self.assertIn('DEFAULT_REPOSITORY="test-owner/nuvo"', (self.assets/"install-nuvo-depth.sh").read_text())
        # A fork's signing key must replace the already-pinned upstream key.
        self.assertIn((self.assets/"release.pub").read_text().strip(), (self.assets/"install-nuvo-depth.sh").read_text())

    def test_supplementary_source_and_build_records_are_signed(self):
        extra = self.directory / "nuvo-depth-source.tar.gz"
        extra.write_bytes(b"test source archive fixture")
        assets = self.directory / "release-with-source"
        manifest = builder.build(ROOT, self.directory/"packages", assets, "test-owner/nuvo", "v1.2.3", "1.2.3", self.platform, self.key, [extra])
        release.verify_signature(assets/"manifest.json", assets/"manifest.sig", assets/"release.pub")
        item = next(f for f in manifest["files"] if f["name"] == extra.name)
        release.check_asset(assets/extra.name, item)
        (assets/extra.name).write_bytes(b"tampered source")
        with self.assertRaises(ValueError): release.check_asset(assets/extra.name, item)
        for extras in ([extra, extra], [self.key], [self.assets/"manifest.json"]):
            with self.assertRaises(ValueError):
                builder.build(ROOT, self.directory/"packages", self.directory/"rejected-output", "test-owner/nuvo", "v1.2.3", "1.2.3", self.platform, self.key, extras)

    def test_source_trust_defaults_match_published_public_key(self):
        import re
        public_key = (ROOT/"deployment/release-signing.pub").read_text()
        self.assertEqual(release.DEFAULT_PUBLIC_KEY, public_key)
        match = re.search(r"^RELEASE_PUBLIC_KEY_PEM='([^']*)'", (ROOT/"install-nuvo-depth.sh").read_text(), re.M)
        self.assertEqual(match.group(1)+"\n", public_key)

    def test_tampering_wrong_key_and_asset_hash_fail(self):
        bad = self.directory / "bad.json"
        bad.write_bytes((self.assets/"manifest.json").read_bytes()+b" ")
        with self.assertRaises(ValueError): release.verify_signature(bad, self.assets/"manifest.sig", self.assets/"release.pub")
        other = self.directory / "other.key"
        subprocess.run(["openssl","genpkey","-algorithm","ED25519","-out",str(other)],check=True,capture_output=True)
        public = self.directory / "other.pub"
        public.write_bytes(subprocess.check_output(["openssl","pkey","-in",str(other),"-pubout"]))
        with self.assertRaises(ValueError): release.verify_signature(self.assets/"manifest.json", self.assets/"manifest.sig", public)
        asset = self.directory / "tampered.deb"
        item = self.manifest["files"][0]
        asset.write_bytes(bytes(item["size"]))
        with self.assertRaises(ValueError): release.check_asset(asset, item)

    def test_incomplete_mixed_unsafe_and_mutable_releases_fail_closed(self):
        for change in (lambda m: m["files"].pop(0), lambda m: m["files"][0].update(version="old"),
                       lambda m: m["files"][0].update(name="../evil.deb"), lambda m: m.update(tag="main"),
                       lambda m: m["platform"].update(kernels=[]), lambda m: m["platform"]["base_image"].update(sha256="")):
            manifest = copy.deepcopy(self.manifest)
            change(manifest)
            with self.assertRaises(ValueError): release.validate_manifest(manifest)
        with self.assertRaises(ValueError): release.validate_manifest(self.manifest, "wrong/repository", "v1.2.3")

    def test_duplicate_json_and_http_redirect_rejected(self):
        path = self.directory / "duplicate.json"
        path.write_text('{"schema_version":1,"schema_version":2}')
        with self.assertRaises(ValueError): release.load_json(path)
        with self.assertRaises(ValueError): release.HTTPSRedirect().redirect_request(None,None,302,"",{},"http://insecure.test")

    def test_standalone_shell_verifies_before_executing_archive(self):
        directory=self.directory/"bootstrap"
        directory.mkdir()
        assets=directory/"assets"
        shutil.copytree(self.assets,assets)
        with zipfile.ZipFile(assets/"nuvo-install.pyz","w") as archive:
            archive.writestr("__main__.py","print('verified fixture installer executed')\n")
        import hashlib
        manifest=json.loads((assets/"manifest.json").read_text())
        item=next(f for f in manifest["files"] if f["name"]=="nuvo-install.pyz")
        item.update(size=(assets/item["name"]).stat().st_size,sha256=hashlib.sha256((assets/item["name"]).read_bytes()).hexdigest())
        (assets/"manifest.json").write_text(json.dumps(manifest))
        subprocess.run(["openssl","pkeyutl","-sign","-inkey",str(self.key),"-rawin","-in",str(assets/"manifest.json"),"-out",str(assets/"manifest.sig")],check=True)
        shell=directory/"install-nuvo-depth.sh"
        shutil.copyfile(ROOT/"install-nuvo-depth.sh",shell)
        commands=directory/"bin"
        commands.mkdir()
        curl=commands/"curl"
        curl.write_text('#!/usr/bin/python3\nimport os,pathlib,shutil,sys\na=sys.argv\nurl=next(v for v in a if v.startswith("https://"))\nassert url.startswith("https://github.com/test-owner/nuvo/releases/download/v1.2.3/")\nshutil.copyfile(pathlib.Path(os.environ["FIXTURE_ASSETS"])/url.rsplit("/",1)[1],a[a.index("-o")+1])\n')
        curl.chmod(0o700)
        env=os.environ|{"PATH":str(commands)+os.pathsep+os.environ["PATH"],"FIXTURE_ASSETS":str(assets)}
        args=["bash",str(shell),"--repository","test-owner/nuvo","--release","v1.2.3","--public-key",str(assets/"release.pub"),"--factory"]
        result=subprocess.run(args,env=env,capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn("verified fixture installer executed",result.stdout)
        with (assets/"nuvo-install.pyz").open("ab") as output: output.write(b"tamper")
        result=subprocess.run(args,env=env,capture_output=True,text=True)
        self.assertNotEqual(result.returncode,0)
        self.assertNotIn("verified fixture installer executed",result.stdout)
        self.assertIn("checksum mismatch",result.stderr)

    @unittest.skipUnless(os.name == "posix", "worker flock requires Linux")
    def test_pending_handoff_survives_a_rejected_installer_job(self):
        root=self.directory/"pending-root"
        root.mkdir()
        state={"schema_version":1,"phase":"installed","access_stage":"connecting","network_deadline":12345}
        setup.save(root,setup.STATE,state)
        job={"plan":{"factory":True},"bundle":str(self.assets),"public_key":str(self.assets/"release.pub"),"repository":"test-owner/nuvo","tag":"v1.2.3"}
        with mock.patch.object(installer,"preflight"),mock.patch.object(installer,"check_ap"):
            with self.assertRaises(ValueError): installer.install_worker(root,job)
        self.assertEqual(setup.read(root,setup.STATE),state)

    @unittest.skipUnless(os.name == "posix", "worker flock requires Linux")
    def test_apt_failure_cannot_activate_hotspot_or_change_identity(self):
        root = self.directory / "worker-root"
        root.mkdir(exist_ok=True)
        job = {"plan":{"factory":True,"access_policy":"setup_ap"},"bundle":str(self.assets),"public_key":str(self.assets/"release.pub"),"repository":"test-owner/nuvo","tag":"v1.2.3"}
        calls = []
        def run(args, **kwargs):
            calls.append(args)
            if args[:2] == ["apt-get","update"]: raise RuntimeError("apt failed")
            return subprocess.CompletedProcess(args,1,"","")
        with mock.patch.object(installer, "preflight"), mock.patch.object(installer,"check_ap"), mock.patch.object(setup,"apply_configuration") as apply:
            with self.assertRaises(RuntimeError): installer.install_worker(root, job, run)
            apply.assert_not_called()
        self.assertFalse(any(c[0]=="nmcli" for c in calls))
        self.assertFalse((root/"var/lib/nuvo-depth/identity/device.uuid").exists())
        self.assertEqual(setup.read(root, installer.RESULT)["status"], "failed")
        self.assertEqual(setup.read(root, setup.STATE)["phase"], "failed")


if __name__ == "__main__": unittest.main()
