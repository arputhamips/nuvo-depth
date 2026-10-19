"""Provisioning tests use temporary roots and a fake process runner, never APT/NM."""
import argparse
from contextlib import redirect_stdout
import io
import http.client
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import nuvo_install as installer
import nuvo_setup as setup


def options(factory=False):
    return argparse.Namespace(factory=factory, wifi_interface="wlan0", wifi_country="US", ap_address="192.168.4.1/24", calibration_bundle=None, reboot=False)


class ProvisioningTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name, text in {"etc/hostname": "nuvo-test\n", "proc/sys/kernel/random/boot_id": "boot-one"}.items():
            setup.admin.atomic_write(self.root / name, text, 0o644)
        self.calls = []

    def run_command(self, args, **kwargs):
        self.calls.append(args)
        output = json.dumps({"accepted": False}) if "provisioning-status" in args else ""
        return subprocess.CompletedProcess(args, 0, output, "")

    def factory_plan(self):
        return installer.collect_plan(self.root, options(True), input_fn=mock.Mock(side_effect=AssertionError("factory prompted")), password_fn=mock.Mock(side_effect=AssertionError("factory prompted")))

    def apply(self, plan):
        with mock.patch.object(setup.admin, "provision_identity", return_value={"device_uuid": "12345678-1234-1234-1234-123456789abc"}), mock.patch.object(setup.admin, "platform_prepare", return_value={"reboot_required": True}):
            return setup.apply_configuration(self.root, plan, self.run_command)

    def test_terminal_answers_current_network_and_password(self):
        answers = iter(["1", "Office camera", "2", "", "y", "y", "n"])
        with redirect_stdout(io.StringIO()):
            plan = installer.collect_plan(self.root, options(), input_fn=lambda _: next(answers), password_fn=lambda _: "my new nuvo password")
        self.assertEqual(plan["access_policy"], "keep_current")
        self.assertEqual(plan["hardware"], "custom")
        self.assertTrue(setup.verify_password("my new nuvo password", plan["owner_password"]))
        self.assertNotIn("my new nuvo password", json.dumps(plan))
        result = self.apply(plan)
        self.assertEqual(result["origin"], "terminal")
        self.assertTrue(result["claimed"])
        self.assertFalse((self.root / setup.PROFILE).exists())
        self.assertFalse((self.root / setup.AP_ENABLED).exists())
        self.assertFalse(any(call[0] == "nmcli" for call in self.calls))

    def test_completed_reboot_clears_the_reported_requirement(self):
        self.apply(self.factory_plan())
        self.assertTrue(setup.status(self.root)["reboot_required"])
        setup.admin.atomic_write(self.root / "proc/sys/kernel/random/boot_id", "boot-two", 0o644)
        self.assertFalse(setup.status(self.root)["reboot_required"])

    def test_terminal_hotspot_is_not_factory_origin(self):
        answers = iter(["2", "", "2", "", "n", "y", "n", "n"])
        with redirect_stdout(io.StringIO()):
            plan = installer.collect_plan(self.root, options(), input_fn=lambda _: next(answers))
        state = self.apply(plan)
        self.assertEqual(state["origin"], "terminal")
        self.assertEqual(state["access_stage"], "armed")
        self.assertFalse(state["claimed"])

    def test_factory_is_same_path_and_preserves_unique_secrets_on_retry(self):
        plan = self.factory_plan()
        state = self.apply(plan)
        private = setup.read(self.root, setup.PRIVATE)
        self.assertEqual(state["origin"], "factory")
        self.assertEqual(state["access_stage"], "armed")
        self.assertFalse(state["claimed"])
        self.assertFalse(state["shipping_ready"])
        self.assertFalse((self.root / "etc/nuvo-depth/capture-enabled").exists())
        self.assertEqual(self.apply(plan)["access_stage"], "armed")
        self.assertEqual(private, setup.read(self.root, setup.PRIVATE))
        self.assertNotIn(private["ap_password"], json.dumps(self.calls))
        if os.name == "posix":
            self.assertEqual((self.root / setup.PRIVATE).stat().st_mode & 0o777, 0o600)
            self.assertEqual((self.root / setup.PROFILE).stat().st_mode & 0o777, 0o600)

    def test_factory_refuses_owned_device_and_existing_terminal_origin(self):
        setup.save(self.root, setup.AUTH, {"claimed": True})
        with self.assertRaises(ValueError): self.factory_plan()
        setup.save(self.root, setup.AUTH, {"claimed": False})
        setup.save(self.root, setup.STATE, {"origin": "terminal"})
        with self.assertRaises(ValueError): self.factory_plan()

    def test_network_mutations_and_installs_cannot_overlap(self):
        self.active()
        state=setup.read(self.root,setup.STATE)
        state["phase"]="installing"
        setup.save(self.root,setup.STATE,state)
        with self.assertRaises(ValueError): setup.stage_network(self.root,"home","password123")
        self.assertFalse((self.root/setup.STATION).exists())
        state.update(phase="installed",access_stage="connecting")
        setup.save(self.root,setup.STATE,state)
        with self.assertRaises(ValueError): self.factory_plan()

    def test_factory_country_must_be_known_without_prompting(self):
        args=options(True)
        args.wifi_country=None
        with self.assertRaises(ValueError): installer.collect_plan(self.root,args)

    def test_pairing_consumes_code_then_owner_password_supports_login(self):
        self.apply(self.factory_plan())
        code = setup.read(self.root, setup.PRIVATE)["pairing_code"]
        self.assertEqual(len(code), 9)
        result = setup.authenticate(self.root, code, now=1000)
        self.assertTrue(result["authenticated"])
        self.assertTrue(result["needs_owner_password"])
        self.assertFalse(setup.authenticate(self.root, code, now=1001)["authenticated"])
        setup.set_owner_password(self.root, "my long owner password")
        self.assertTrue(setup.authenticate(self.root, "my long owner password", now=1002)["authenticated"])
        material = json.dumps(setup.read(self.root, setup.AUTH))
        self.assertNotIn("my long owner password", material)
        self.assertNotIn("pairing_code", setup.read(self.root, setup.PRIVATE))

    def test_pairing_failure_limit_and_expiration_persist(self):
        code = setup.issue_pairing(self.root)
        for index in range(5):
            self.assertFalse(setup.authenticate(self.root, "wrong", now=1000+index)["authenticated"])
        self.assertFalse(setup.authenticate(self.root, code, now=1010)["authenticated"])
        self.assertTrue(setup.authenticate(self.root, code, now=1310)["authenticated"])
        code = setup.issue_pairing(self.root)
        setup.authenticate(self.root, "wrong", now=2000)
        self.assertFalse(setup.authenticate(self.root, code, now=2601)["authenticated"])

    def test_failed_configuration_never_arms_active_network(self):
        plan = self.factory_plan()
        def fail(args, **kwargs):
            if "--check-config" in args: raise RuntimeError("configuration rejected")
            return self.run_command(args, **kwargs)
        with mock.patch.object(setup.admin, "provision_identity", return_value={"device_uuid": "123456"}), mock.patch.object(setup.admin, "platform_prepare", return_value={"reboot_required": True}):
            with self.assertRaises(RuntimeError): setup.apply_configuration(self.root, plan, fail)
        setup.reconcile(self.root, self.run_command)
        self.assertFalse(any(call[0] == "nmcli" for call in self.calls))
        self.assertEqual(setup.read(self.root, setup.STATE)["phase"], "configuring")

    def test_camera_start_failure_does_not_block_console_or_selected_hotspot(self):
        plan=self.factory_plan()
        plan["engineering"]=True
        def runner(args,**kwargs):
            if args==["systemctl","restart","nuvo-depthd-native.service"]:
                return subprocess.CompletedProcess(args,1,"","")
            return self.run_command(args,**kwargs)
        with mock.patch.object(setup.admin,"provision_identity",return_value={"device_uuid":"123456"}),mock.patch.object(setup.admin,"platform_prepare",return_value={"reboot_required":False}):
            state=setup.apply_configuration(self.root,plan,runner)
        self.assertEqual(state["phase"],"installed")
        self.assertEqual(state["access_stage"],"armed")
        self.assertIn("Capture did not start",state["capture_error"])
        self.assertTrue(any("nuvo-gateway.service" in call and "restart" in call for call in self.calls))

    def test_hotspot_only_after_deliberate_activation_or_new_boot(self):
        self.apply(self.factory_plan())
        setup.reconcile(self.root, self.run_command)
        self.assertFalse(any(call[0] == "nmcli" for call in self.calls))
        (self.root / "proc/sys/kernel/random/boot_id").write_text("boot-two")
        setup.reconcile(self.root, self.run_command)
        self.assertEqual(setup.status(self.root)["access_stage"], "active")
        self.assertFalse(setup.status(self.root)["reboot_required"])
        (self.root / "proc/sys/kernel/random/boot_id").write_text("boot-three")
        self.calls.clear()
        setup.reconcile(self.root, self.run_command)
        self.assertTrue(any("up" in call for call in self.calls))

    def active(self):
        self.apply(self.factory_plan())
        state = setup.read(self.root, setup.STATE)
        state.update(access_stage="active", active_boot_id="boot-one")
        setup.save(self.root, setup.STATE, state)

    def test_wifi_credentials_are_keyfile_only_and_confirmation_checks_candidate(self):
        self.active()
        result = setup.stage_network(self.root, "My home; network", "secret wifi password")
        self.assertNotIn("secret wifi password", json.dumps(self.calls))
        self.assertIn("psk=secret\\swifi\\spassword", (self.root / setup.STATION).read_text())
        state = setup.read(self.root, setup.STATE)
        state.update(network_started=True)
        setup.save(self.root, setup.STATE, state)
        ticket = result["confirm_url"].split("#finish=")[1]
        with self.assertRaises(ValueError): setup.confirm_network(self.root, ticket, self.run_command, local_address="192.168.1.20")
        runner = lambda args, **kw: subprocess.CompletedProcess(args, 0, state["station_uuid"]+"\n100 (connected)\n192.168.1.20/24" if "GENERAL.CON-UUID,GENERAL.STATE,IP4.ADDRESS" in args else "", "")
        with self.assertRaises(ValueError): setup.confirm_network(self.root, ticket, runner, local_address="10.0.0.20")
        setup.confirm_network(self.root, ticket, runner, local_address="192.168.1.20")
        self.assertEqual(setup.status(self.root)["access_stage"], "confirmed")
        self.assertNotIn("network_ticket", setup.read(self.root, setup.PRIVATE))
        self.calls.clear()
        setup.reconcile(self.root, self.run_command)
        self.assertEqual(self.calls, [])

    def test_unconfirmed_or_rebooted_network_change_restores_same_hotspot(self):
        self.active()
        original = setup.read(self.root, setup.PRIVATE)["ap_password"]
        setup.stage_network(self.root, "home", "wrong password")
        state = setup.read(self.root, setup.STATE)
        state["network_deadline"] = 0
        setup.save(self.root, setup.STATE, state)
        setup.reconcile(self.root, self.run_command)
        self.assertEqual(setup.status(self.root)["access_stage"], "active")
        self.assertEqual(setup.read(self.root, setup.PRIVATE)["ap_password"], original)
        self.assertTrue(any("up" in call for call in self.calls))

    def test_network_keyfile_rejects_control_injection(self):
        self.active()
        with self.assertRaises(ValueError): setup.stage_network(self.root, "ssid\n[connection]", "password123")

    def test_ap_capability_and_overlap_checks(self):
        plan = self.factory_plan()
        with self.assertRaises(ValueError): installer.check_ap(plan, lambda *a, **kw: subprocess.CompletedProcess([],0,"no", ""))
        def overlap(args, **kwargs):
            return subprocess.CompletedProcess(args,0,"yes" if args[0]=="nmcli" else '[{"dst":"192.168.4.0/24","dev":"eth0"}]', "")
        with self.assertRaises(ValueError): installer.check_ap(plan, overlap)

    def test_rerun_retains_nondefault_ap_configuration(self):
        setup.save(self.root,setup.STATE,{"origin":"factory","wifi_interface":"wlan1","ap_address":"172.20.9.1/24"})
        args=options(True)
        args.wifi_interface=args.ap_address=None
        plan=installer.collect_plan(self.root,args)
        self.assertEqual(plan["wifi_interface"],"wlan1")
        self.assertEqual(plan["ap_address"],"172.20.9.1/24")

    def test_platform_preflight_rejects_os_architecture_kernel_and_wrong_board(self):
        setup.admin.atomic_write(self.root/"proc/device-tree/model","Raspberry Pi 5 Model B",0o644)
        setup.admin.atomic_write(self.root/"boot/firmware/config.txt","[all]\n",0o644)
        setup.admin.atomic_write(self.root/"etc/os-release",'ID=debian\nVERSION_CODENAME=trixie\n',0o644)
        runner=lambda args,**kw: subprocess.CompletedProcess(args,0,"","")
        manifest={"platform":{"kernels":["qualified"]}}
        self.assertEqual(installer.preflight(self.root,manifest,architecture="arm64",kernel="qualified",run=runner)["codename"],"trixie")
        for arch,kernel in (("armhf","qualified"),("arm64","untested")):
            with self.assertRaises(ValueError): installer.preflight(self.root,manifest,architecture=arch,kernel=kernel,run=runner)
        (self.root/"etc/os-release").write_text("ID=debian\nVERSION_CODENAME=bookworm\n")
        with self.assertRaises(ValueError): installer.preflight(self.root,manifest,architecture="arm64",kernel="qualified",run=runner)
        (self.root/"proc/device-tree/model").write_text("Raspberry Pi 4")
        with self.assertRaises(ValueError): installer.preflight(self.root,manifest,architecture="arm64",kernel="qualified",run=runner)

    def test_http_setup_rejects_foreign_origin_bad_nonce_and_non_ap_mutations(self):
        # Loopback stands in for an AP address only in this HTTP fixture.
        setup.save(self.root,setup.STATE,{"phase":"installed","hostname":"nuvo-test","access_stage":"active","ap_address":"127.0.0.1/24"})
        server=setup.SetupServer(("127.0.0.1",0),setup.make_handler(self.root))
        server.nonce="test-nonce"
        thread=threading.Thread(target=server.serve_forever,daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(thread.join,2)
        self.addCleanup(server.shutdown)
        def request(path,body=None,headers=None):
            connection=http.client.HTTPConnection("127.0.0.1",server.server_port,timeout=2)
            connection.request("POST" if body is not None else "GET",path,body=body,headers={"Host":"127.0.0.1","Content-Type":"application/json"}|dict(headers or {}))
            response=connection.getresponse()
            data=response.read()
            connection.close()
            return response,data
        response,data=request("/")
        self.assertEqual(response.status,200)
        self.assertEqual(response.getheader("Cache-Control"),"no-store")
        self.assertNotIn(b"ap_password",data)
        body=json.dumps({"ssid":"home","password":"password123"})
        self.assertEqual(request("/network",body,{"Origin":"http://foreign.test","X-Nuvo-Setup":"test-nonce"})[0].status,400)
        self.assertEqual(request("/network",body,{"X-Nuvo-Setup":"wrong"})[0].status,400)
        with mock.patch.object(setup,"stage_network",return_value={"confirm_url":"http://nuvo-test.local/#finish=fixture"}) as stage:
            self.assertEqual(request("/network",body,{"Origin":"http://127.0.0.1","X-Nuvo-Setup":"test-nonce"})[0].status,200)
            stage.assert_called_once_with(self.root,"home","password123")
        self.assertEqual(request("/confirm",json.dumps({"ticket":"x"}),{"X-Nuvo-Setup":"test-nonce"})[0].status,400)


if __name__ == "__main__": unittest.main()
