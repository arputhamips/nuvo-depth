"""Real Ed25519 signatures and Debian fixtures; no package installation/network."""
import copy
import json
import os
import io
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import tarfile
import urllib.error
import unittest
import zipfile
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build_install_release as builder
import nuvo_release as release
import nuvo_install as installer
import nuvo_setup as setup


def run_pty(command, *, cwd=None, env=None, prompts=(), timeout=15):
    """Drain a PTY and reap its child under one deadline; EOF is not exit."""
    import errno, pty, select, signal, time
    deadline = time.monotonic() + timeout
    pid, master = pty.fork()
    if pid == 0:
        try:
            if cwd is not None:
                os.chdir(cwd)
            os.execvpe(command[0], command, os.environ if env is None else env)
        except BaseException:
            os._exit(127)
    transcript = bytearray()
    answered = set()
    status = None
    eof = False
    completed = False
    try:
        while True:
            if status is None:
                done, child_status = os.waitpid(pid, os.WNOHANG)
                if done:
                    status = child_status
            if eof and status is not None:
                completed = True
                return os.waitstatus_to_exitcode(status), bytes(transcript)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("PTY child did not finish before deadline: " + transcript.decode(errors='replace'))
            if eof:
                # EIO/EOF can precede process exit. Keep waiting, without
                # spinning on a permanently readable closed terminal.
                time.sleep(min(0.01, remaining))
                continue
            if not select.select([master], [], [], min(0.05, remaining))[0]:
                continue
            try:
                chunk = os.read(master, 65536)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                chunk = b''
            if not chunk:
                eof = True
                continue
            transcript.extend(chunk)
            for prompt, answer in prompts:
                if prompt in transcript and prompt not in answered:
                    os.write(master, answer)
                    answered.add(prompt)
    finally:
        try:
            if not completed:
                try:
                    os.killpg(pid, signal.SIGKILL)
                except ProcessLookupError:
                    # The fork can fail before creating its terminal session.
                    if status is None:
                        try:
                            os.kill(pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                if status is None:
                    os.waitpid(pid, 0)
        finally:
            os.close(master)


@unittest.skipUnless(os.name == 'posix', 'requires a controlling PTY')
class PtyRunnerTest(unittest.TestCase):
    def test_eof_before_delayed_child_exit_is_not_failure(self):
        import errno, time
        # The child cannot exit until the parent has observed terminal EOF.
        # This reproduces the race without depending on scheduler timing.
        real_read = os.read
        for eof_as_bytes in (False, True):
            release_read, release_write = os.pipe()
            os.set_inheritable(release_read, True)
            eof_seen = []
            def observe_eof(fd, size):
                try:
                    chunk = real_read(fd, size)
                except OSError as error:
                    if error.errno != errno.EIO:
                        raise
                    eof_seen.append(True)
                    os.write(release_write, b'x')
                    if eof_as_bytes:
                        return b''
                    raise
                if not chunk:
                    eof_seen.append(True)
                    os.write(release_write, b'x')
                return chunk
            program = ("import os,time\nprint('output drained',flush=True)\n"
                       "for fd in (0,1,2): os.close(fd)\n"
                       f"os.read({release_read},1)\ntime.sleep(.1)\n")
            try:
                started = time.monotonic()
                with self.subTest(eof_as_bytes=eof_as_bytes), mock.patch('os.read', side_effect=observe_eof):
                    code, transcript = run_pty([sys.executable, '-c', program], timeout=2)
                self.assertEqual(code, 0)
                self.assertIn(b'output drained', transcript)
                self.assertTrue(eof_seen)
                self.assertGreaterEqual(time.monotonic() - started, .1)
            finally:
                os.close(release_read)
                os.close(release_write)

    def test_nonzero_exit_and_all_output_are_retained(self):
        code, transcript = run_pty([sys.executable, '-c', "import sys;print('failure output');sys.exit(7)"])
        self.assertEqual(code, 7)
        self.assertIn(b'failure output', transcript)

    def test_open_or_closed_terminal_hang_uses_same_deadline_and_is_reaped(self):
        import signal, time
        for close in (False, True):
            program = "import os,time\nprint('ready',flush=True)\n"
            if close:
                program += "for fd in (0,1,2): os.close(fd)\n"
            program += 'time.sleep(60)\n'
            with self.subTest(closed=close), mock.patch('os.killpg', wraps=os.killpg) as kill:
                started = time.monotonic()
                with self.assertRaisesRegex(TimeoutError, 'deadline.*ready'):
                    run_pty([sys.executable, '-c', program], timeout=.3)
                self.assertLess(time.monotonic() - started, 2)
                pid = kill.call_args.args[0]
                kill.assert_called_once_with(pid, signal.SIGKILL)
                with self.assertRaises(ChildProcessError):
                    os.waitpid(pid, os.WNOHANG)

    def test_io_and_assertion_exceptions_kill_and_reap_the_owned_group(self):
        import signal
        program = "import time;print('answer: ',flush=True);time.sleep(60)"
        for operation, error in [('select.select', RuntimeError('select failed')),
                                 ('os.read', AssertionError('read failed')),
                                 ('os.write', OSError('write failed'))]:
            with self.subTest(operation=operation), mock.patch('os.killpg', wraps=os.killpg) as kill:
                with mock.patch(operation, side_effect=error), self.assertRaises(type(error)):
                    run_pty([sys.executable, '-c', program], prompts=[(b'answer: ', b'yes\n')], timeout=2)
                pid = kill.call_args.args[0]
                kill.assert_called_once_with(pid, signal.SIGKILL)
                with self.assertRaises(ChildProcessError):
                    os.waitpid(pid, os.WNOHANG)

    @unittest.skipUnless(Path('/proc/self/stat').exists(), 'Linux process-state check')
    def test_timeout_also_stops_descendants_in_the_owned_group(self):
        import time
        with tempfile.TemporaryDirectory() as temporary:
            pids = Path(temporary)/'pids'
            program = ("import os,signal,time\nfrom pathlib import Path\n"
                       "signal.signal(signal.SIGHUP,signal.SIG_IGN)\nchild=os.fork()\n"
                       "if child==0: time.sleep(60);os._exit(0)\n"
                       f"Path({str(pids)!r}).write_text(str(os.getpid())+' '+str(child))\n"
                       "print('descendant ready',flush=True)\nos._exit(0)\n")
            with self.assertRaisesRegex(TimeoutError, 'deadline.*descendant ready'):
                run_pty([sys.executable, '-c', program], timeout=.3)
            leader, child = map(int, pids.read_text().split())
            with self.assertRaises(ChildProcessError):
                os.waitpid(leader, os.WNOHANG)
            # A killed descendant may briefly remain an init-owned zombie;
            # distinguish that from a running leaked process.
            deadline = time.monotonic()+1
            state = Path(f'/proc/{child}/stat')
            while state.exists():
                try:
                    process_state = state.read_text().rsplit(')',1)[1].split()[0]
                except FileNotFoundError:
                    break
                if process_state == 'Z':
                    break
                self.assertLess(time.monotonic(), deadline, 'PTY descendant survived cleanup')
                time.sleep(.01)


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
        self.assertIn('DEFAULT_RELEASE="v1.2.3"', (self.assets/"install-nuvo-depth.sh").read_text())
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

    def test_github_safe_beta_names_keep_exact_debian_control_versions(self):
        packages = self.directory / "beta-packages"
        packages.mkdir()
        version = "1.2.3~beta.2"
        for package in release.RUNTIME_PACKAGES:
            tree = packages/package/"DEBIAN"
            tree.mkdir(parents=True)
            (tree/"control").write_text(f"Package: {package}\nVersion: {version}\nArchitecture: arm64\nMaintainer: Test <test@example.com>\nDescription: Beta fixture\n")
            subprocess.run(["dpkg-deb","--build","--root-owner-group",str(tree.parent),str(packages/f"{package}_{version}_arm64.deb")],check=True,capture_output=True)
        extra = packages / "cohort_1.2.3~beta.2_arm64.buildinfo"
        extra.write_text("test build record")
        assets = self.directory/"beta-release"
        manifest = builder.build(ROOT,packages,assets,"test-owner/nuvo","v1.2.3-beta.2",version,self.platform,self.key,[extra])
        self.assertTrue(all("~" not in f["name"] and "+" not in f["name"] for f in manifest["files"]))
        self.assertTrue((assets/"cohort_1.2.3.beta.2_arm64.buildinfo").is_file())
        actual, paths = release.acquire_release(self.directory, assets/"release.pub",bundle=assets)
        self.assertEqual(actual["version"], version)
        self.assertEqual(len(paths),8)
        self.assertTrue(all(f.get("version",version)==version for f in actual["files"]))

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

    def bootstrap_fixture(self, name, program="print('verified fixture installer executed')\n"):
        directory=self.directory/name
        directory.mkdir()
        assets=directory/"assets"
        shutil.copytree(self.assets,assets)
        with zipfile.ZipFile(assets/"nuvo-install.pyz","w") as archive:
            archive.writestr("__main__.py",program)
        import hashlib
        manifest=json.loads((assets/"manifest.json").read_text())
        item=next(f for f in manifest["files"] if f["name"]=="nuvo-install.pyz")
        item.update(size=(assets/item["name"]).stat().st_size,sha256=hashlib.sha256((assets/item["name"]).read_bytes()).hexdigest())
        (assets/"manifest.json").write_text(json.dumps(manifest))
        subprocess.run(["openssl","pkeyutl","-sign","-inkey",str(self.key),"-rawin","-in",str(assets/"manifest.json"),"-out",str(assets/"manifest.sig")],check=True)
        publication=directory/"downloads"
        builder.publish_bundle(assets,publication,self.key)
        shell=publication/"install-nuvo-depth.sh"
        commands=directory/"bin"
        commands.mkdir()
        curl=commands/"curl"
        curl.write_text('#!/usr/bin/python3\nimport os,pathlib,shutil,sys\na=sys.argv\nurl=next(v for v in a if v.startswith("https://"))\nassert url.startswith("https://github.com/test-owner/nuvo/releases/download/v1.2.3/")\np=pathlib.Path(os.environ["FIXTURE_ASSETS"])/url.rsplit("/",1)[1]\nif "-o" in a: shutil.copyfile(p,a[a.index("-o")+1])\nelse: sys.stdout.buffer.write(p.read_bytes())\n')
        curl.chmod(0o700)
        env=os.environ|{"PATH":str(commands)+os.pathsep+os.environ["PATH"],"FIXTURE_ASSETS":str(publication)}
        return directory,assets,publication,shell,env

    def test_standalone_shell_verifies_before_executing_archive(self):
        directory,assets,publication,shell,env=self.bootstrap_fixture("bootstrap")
        args=["bash",str(shell),"--factory"]
        result=subprocess.run(args,env=env,capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn("verified fixture installer executed",result.stdout)
        with (publication/release.BUNDLE_NAME).open("ab") as output: output.write(b"tamper")
        result=subprocess.run(args,env=env,capture_output=True,text=True)
        self.assertNotEqual(result.returncode,0)
        self.assertNotIn("verified fixture installer executed",result.stdout)
        self.assertIn("bundle signature verification failed",result.stderr)
        # A signed archive cannot bypass the inner installer's signed checksum.
        with (assets/"nuvo-install.pyz").open("ab") as output: output.write(b"tamper")
        with tarfile.open(publication/release.BUNDLE_NAME,"w:gz") as archive:
            for path in assets.iterdir(): archive.add(path,arcname=path.name)
        subprocess.run(["openssl","pkeyutl","-sign","-inkey",str(self.key),"-rawin","-in",str(publication/release.BUNDLE_NAME),"-out",str(publication/(release.BUNDLE_NAME+".sig"))],check=True)
        result=subprocess.run(args,env=env,capture_output=True,text=True)
        self.assertNotEqual(result.returncode,0)
        self.assertIn("checksum mismatch",result.stderr)
        self.assertNotIn("verified fixture installer executed",result.stdout)

    def test_latest_bootstrap_resolves_then_executes_only_the_exact_signed_release(self):
        directory,assets,publication,shell,env=self.bootstrap_fixture('latest-bootstrap')
        archive=release.BUNDLE_NAME
        metadata=dict(tag_name='v1.2.3',draft=False,prerelease=False,published_at='2026-10-21T00:00:00Z',assets=[
                      dict(name=archive,state='uploaded',size=(publication/archive).stat().st_size),
                      dict(name=archive+'.sig',state='uploaded',size=64)])
        approval=dict(schema_version=1,repository='test-owner/nuvo',approved=True,channel='stable',tag='v1.2.3',profiles=['appliance'])
        curl=directory/'bin/curl'
        source=curl.read_text()
        dispatch=('if url == "https://raw.githubusercontent.com/test-owner/nuvo/main/deployment/public-release.json": sys.stdout.buffer.write('+repr(json.dumps(approval).encode())+'); sys.exit(0)\n'
                  'if url == "https://api.github.com/repos/test-owner/nuvo/releases/tags/v1.2.3": sys.stdout.buffer.write('+repr(json.dumps(metadata).encode())+'); sys.exit(0)\n')
        curl.write_text(source.replace('assert url.startswith(',dispatch+'assert url.startswith(',1))
        result=subprocess.run(['bash',str(shell),'--release','latest','--factory'],
            env=env,capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('Selected public appliance release: v1.2.3',result.stdout)
        self.assertIn('verified fixture installer executed',result.stdout)
        unpacked=directory/'offline';release.unpack_bundle(publication/archive,unpacked)
        result=subprocess.run(['bash',str(shell),'--release','latest','--bundle',str(unpacked),'--factory'],
            env=env,capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertNotIn('Selected public',result.stdout)

    def test_three_downloads_include_all_signed_source_and_package_files(self):
        directory,assets,publication,shell,env=self.bootstrap_fixture("three-files")
        self.assertEqual({p.name for p in publication.iterdir()},{"install-nuvo-depth.sh",release.BUNDLE_NAME,release.BUNDLE_NAME+".sig"})
        release.verify_signature(publication/release.BUNDLE_NAME,publication/(release.BUNDLE_NAME+".sig"),assets/"release.pub")
        unpacked=directory/"unpacked"
        release.unpack_bundle(publication/release.BUNDLE_NAME,unpacked)
        for item in release.load_json(unpacked/"manifest.json")["files"]: release.check_asset(unpacked/item["name"],item)
        result=subprocess.run(["bash",str(shell),"--bundle",str(unpacked),"--factory"],env=env,capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertNotIn("Downloading",result.stdout)

    def test_online_bundle_and_legacy_releases_remain_installable(self):
        directory,assets,publication,_,_=self.bootstrap_fixture("online-bundle")
        calls=[]
        def download(url,target,limit):
            calls.append(target.name)
            shutil.copyfile(publication/target.name,target)
        with mock.patch.object(release,"download",side_effect=download):
            manifest,packages=release.acquire_release(directory/"online",assets/"release.pub","test-owner/nuvo","v1.2.3")
        self.assertEqual(len(packages),8)
        self.assertEqual(calls,[release.BUNDLE_NAME,release.BUNDLE_NAME+".sig"])
        def corrupted(url,target,limit):
            download(url,target,limit)
            if target.name==release.BUNDLE_NAME:
                with target.open("ab") as file: file.write(b"tamper")
        with mock.patch.object(release,"download",side_effect=corrupted),mock.patch.object(release,"unpack_bundle") as unpack:
            with self.assertRaises(ValueError):
                release.acquire_release(directory/"corrupted",assets/"release.pub","test-owner/nuvo","v1.2.3")
            unpack.assert_not_called()
        calls.clear()
        def legacy(url,target,limit):
            calls.append(target.name)
            if target.name==release.BUNDLE_NAME: raise urllib.error.HTTPError(url,404,"not found",{},None)
            shutil.copyfile(assets/target.name,target)
        with mock.patch.object(release,"download",side_effect=legacy):
            _,packages=release.acquire_release(directory/"legacy",assets/"release.pub","test-owner/nuvo","v1.2.3")
        self.assertEqual(len(packages),8)
        self.assertIn("manifest.json",calls)
        with mock.patch.object(release,"download",side_effect=urllib.error.HTTPError("https://fixture",403,"forbidden",{},None)) as downloads:
            with self.assertRaises(urllib.error.HTTPError):
                release.acquire_release(directory/"forbidden",assets/"release.pub","test-owner/nuvo","v1.2.3")
        self.assertEqual(downloads.call_count,1)

    def test_bundle_rejects_path_traversal_links_duplicates_and_special_files(self):
        for index,(name,kind,duplicate) in enumerate([("../outside",tarfile.REGTYPE,False),("link",tarfile.SYMTYPE,False),("device",tarfile.CHRTYPE,False),("same",tarfile.REGTYPE,True)]):
            path=self.directory/f"unsafe-{index}.tar.gz"
            with tarfile.open(path,"w:gz") as archive:
                member=tarfile.TarInfo(name); member.type=kind; member.size=1 if kind==tarfile.REGTYPE else 0
                archive.addfile(member,io.BytesIO(b"x") if member.size else None)
                if duplicate: archive.addfile(member,io.BytesIO(b"x"))
            with self.assertRaises(ValueError): release.unpack_bundle(path,self.directory/f"unsafe-{index}")
        self.assertFalse((self.directory/"outside").exists())

    def test_factory_pipe_ignores_code_in_the_working_directory(self):
        directory,_,_,shell,env=self.bootstrap_fixture("factory-pipe")
        (directory/"tools").mkdir()
        (directory/"tools/nuvo_install.py").write_text("raise SystemExit('untrusted local code executed')")
        (directory/"environment").touch()
        env=env|{"BOOTSTRAP_URL":"https://github.com/test-owner/nuvo/releases/download/v1.2.3/install-nuvo-depth.sh"}
        result=subprocess.run(["bash","-c",'set -o pipefail; curl -fsSL "$BOOTSTRAP_URL" | bash -s -- --factory'],cwd=directory,env=env,capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn("verified fixture installer executed",result.stdout)

    @unittest.skipUnless(os.name=="posix","requires a controlling PTY")
    def test_curl_pipe_preserves_questions_and_masked_passwords(self):
        program="import getpass,sys\nassert sys.stdin.isatty()\nassert input('Network choice: ')=='current'\nassert getpass.getpass('Wi-Fi password: ')=='private test password'\nprint('interactive fixture complete')\n"
        directory,_,_,_,env=self.bootstrap_fixture("interactive-pipe",program)
        env=env|{"BOOTSTRAP_URL":"https://github.com/test-owner/nuvo/releases/download/v1.2.3/install-nuvo-depth.sh"}
        code, transcript = run_pty(["bash", "-c", 'set -o pipefail; curl -fsSL "$BOOTSTRAP_URL" | bash'],
                                   cwd=directory, env=env,
                                   prompts=[(b"Network choice: ", b"current\n"),
                                            (b"Wi-Fi password: ", b"private test password\n")])
        self.assertEqual(code, 0, transcript.decode(errors="replace"))
        self.assertIn(b"interactive fixture complete", transcript)
        self.assertNotIn(b"private test password", transcript)

    def test_normal_pipe_without_a_terminal_fails_before_downloads(self):
        directory,_,_,_,env=self.bootstrap_fixture("no-terminal")
        env=env|{"BOOTSTRAP_URL":"https://github.com/test-owner/nuvo/releases/download/v1.2.3/install-nuvo-depth.sh"}
        result=subprocess.run(["bash","-c",'set -o pipefail; curl -fsSL "$BOOTSTRAP_URL" | bash'],env=env,capture_output=True,text=True,start_new_session=True,timeout=10)
        self.assertNotEqual(result.returncode,0)
        self.assertIn("controlling terminal",result.stderr)
        self.assertNotIn("Downloading",result.stdout)

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
