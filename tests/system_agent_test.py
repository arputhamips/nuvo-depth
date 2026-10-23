import importlib.util
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest import mock

SOURCE=Path(__file__).parents[1]/"tools/nuvo_system_agent.py"
spec=importlib.util.spec_from_file_location("nuvo_system_agent",SOURCE)
module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
calibration_spec=importlib.util.spec_from_file_location(
    "nuvo_calibration_transaction", SOURCE.parent/"calibration/nuvo_calibration.py")
calibration=importlib.util.module_from_spec(calibration_spec)
calibration_spec.loader.exec_module(calibration)


class SystemAgentTest(unittest.TestCase):
    @mock.patch.object(module.subprocess,'run')
    def test_diagnostics_is_a_fixed_read_only_operation(self,run):
        run.return_value=mock.Mock(returncode=0,stdout='{"schema_version":1}',stderr='')
        self.assertEqual(module.execute({'operation':'diagnostics.status'})['result']['schema_version'],1)
        self.assertEqual(run.call_args.args[0],['/usr/bin/nuvoctl','diagnostics','--json'])
        for extra in ('command','path','output','secret'):
            with self.assertRaises(ValueError):module.execute({'operation':'diagnostics.status',extra:'arbitrary'})
    @mock.patch.object(module.subprocess,"run")
    def test_setup_secret_uses_stdin_and_strict_allowlist(self,run):
        run.return_value=mock.Mock(returncode=0,stdout='{"authenticated":true}',stderr="")
        secret="private owner password"
        result=module.execute({"operation":"setup.authenticate","secret":secret})
        self.assertTrue(result["result"]["authenticated"])
        arguments,options=run.call_args
        self.assertEqual(arguments[0],["/usr/bin/nuvo-setup","auth"])
        self.assertNotIn(secret,str(arguments))
        self.assertEqual(json.loads(options["input"])["secret"],secret)
        with self.assertRaises(ValueError): module.execute({"operation":"setup.authenticate","secret":secret,"command":"sh"})
        with self.assertRaises(ValueError): module.execute({"operation":"setup.owner","password":123})

    @mock.patch.object(module.subprocess,"run")
    def test_large_calibration_history_retains_latest_details_within_transport_bound(self,run):
        captures=[{"capture_id":f"{index:016x}","accepted":index%3!=0,"common_corners":96,
                   "camera":{"left_camera_id":"left-camera","right_camera_id":"right-camera",
                             "left":{"exposure_us":7999,"analogue_gain":3.0},
                             "right":{"exposure_us":7999,"analogue_gain":3.0},
                             "frame":{"capture_sequence":index,"device_uuid":"a"*36,"run_uuid":"b"*36}},
                   "quality":{"left":{"sharpness":1234.5,"mean_luma":100.,"clipped_fraction":0.,
                                      "center_x":(index%3+.5)/3,"center_y":(index%9//3+.5)/3},
                              "right":{"sharpness":1234.5,"mean_luma":100.,"clipped_fraction":0.}},
                   "reasons":[]} for index in range(400)]
        status={"session_id":"a"*32,"state":"failed","accepted_pairs":266,"rejected_pairs":134,
                "captures":captures,"attempted_capture_ids":[item["capture_id"] for item in captures],
                "validation":{"status":"fail","per_view":{"training_epipolar_rms_px":[.123456789]*320,
                                                                  "holdout_epipolar_rms_px":[.234567891]*80}}}
        with tempfile.TemporaryDirectory() as directory:
            saved=Path(directory)/"status.json"
            saved.write_text(json.dumps(status))
            original=saved.read_bytes()
            self.assertGreater(len(original),64*1024)
            run.return_value=mock.Mock(returncode=0,stdout=saved.read_text(),stderr="")
            response=module.execute({"operation":"calibration.status","session_id":"a"*32})
            result=response["result"]
            self.assertEqual(result["captures"],[captures[-1]])
            self.assertEqual(result["capture_history_total"],400)
            self.assertEqual(result["capture_history_included"],1)
            self.assertTrue(result["capture_history_truncated"])
            self.assertEqual(result["coverage"]["occupied_cell_indices"],[1,2,4,5,7,8])
            self.assertEqual(result["accepted_pairs"],266)
            self.assertEqual(result["validation"],status["validation"])
            self.assertLess(len(json.dumps(response).encode()),64*1024)
            self.assertEqual(saved.read_bytes(),original)

    @mock.patch.object(module.subprocess,"run")
    def test_empty_and_single_capture_history_remain_explicit(self,run):
        for captures in ([],[{"capture_id":"first","accepted":True}]):
            run.return_value=mock.Mock(returncode=0,stdout=json.dumps({"captures":captures}),stderr="")
            result=module.execute({"operation":"calibration.capture","session_id":"a"*32})["result"]
            self.assertEqual(result["captures"],captures)
            self.assertEqual(result["capture_history_total"],len(captures))
            self.assertFalse(result["capture_history_truncated"])

    def test_oversized_response_is_a_bounded_structured_error(self):
        payload=module.encode_response({"status":"ok","result":"\u20ac"*(64*1024)})
        self.assertLess(len(payload),64*1024)
        response=json.loads(payload)
        self.assertEqual(response["status"],"error")
        self.assertEqual(response["error_code"],"response_too_large")

    def test_control_socket_survives_native_daemon_restart(self):
        root=Path(__file__).parents[1]
        socket_path="/run/nuvo-system-agent.sock"
        socket_unit=(root/"deployment/systemd/nuvo-system-agent.socket").read_text()
        gateway=(root/"apps/nuvo_gateway_main.cpp").read_text()
        engineering=(root/"deployment/systemd/nuvo-gateway-engineering-port.conf").read_text()
        self.assertIn(f"ListenSequentialPacket={socket_path}",socket_unit)
        self.assertIn(f'"{socket_path}"',gateway)
        self.assertIn(f"--system-agent-socket {socket_path}",engineering)
        self.assertNotIn("/run/nuvo-depth/system-agent.sock",socket_unit)

    @mock.patch.object(module.subprocess,"run")
    def test_allowlisted_operation_uses_fixed_argv(self,run):
        run.return_value=mock.Mock(returncode=0,stdout='{"preference":"usb"}',stderr="")
        result=module.execute({"operation":"transport.stage","preference":"usb"})
        self.assertEqual(result["status"],"ok")
        command=run.call_args.args[0]
        self.assertEqual(command,["/usr/bin/nuvoctl","transport","stage","usb"])
        self.assertFalse(run.call_args.kwargs.get("shell",False))

    def test_unknown_and_injected_values_are_rejected(self):
        for request in (
            {"operation":"identity.provision"},
            {"operation":"transport.stage","preference":"usb;reboot"},
            {"operation":"transport.stage","preference":["usb"]},
            {"operation":"transport.stage"},
            {"operation":"transport.stage","wifi_enabled":"false"},
            {"operation":"transport.stage","wifi_enabled":0},
            {"operation":"transport.stage","wifi_enabled":None},
            {"operation":"transport.stage","wifi_enabled":False,"command":"reboot"},
            {"operation":"transport.stage","preference":"usb","physical_confirmation":"true"},
            {"operation":"transport.confirm","wifi_enabled":False},
            {"operation":"transport.rollback","path":"arbitrary"},
            {"operation":"transport.status","command":"arbitrary"},
        ):
            with self.assertRaises(ValueError): module.execute(request)

    @mock.patch.object(module.subprocess,"run")
    def test_independent_wifi_patch_is_boolean_and_keeps_optional_preference(self,run):
        run.return_value=mock.Mock(returncode=0,stdout='{}',stderr="")
        for enabled in (False,True):
            module.execute({"operation":"transport.stage","wifi_enabled":enabled})
            self.assertEqual(run.call_args.args[0],["/usr/bin/nuvoctl","transport","stage",
                                                   "--wifi-enabled",str(enabled).lower()])
        module.execute({"operation":"transport.stage","preference":"usb","wifi_enabled":True})
        self.assertEqual(run.call_args.args[0],["/usr/bin/nuvoctl","transport","stage","usb","--wifi-enabled","true"])
        module.execute({"operation":"transport.status"})
        self.assertEqual(run.call_args.args[0],["/usr/bin/nuvoctl","transport","status"])

    @mock.patch.object(module.subprocess,"run")
    def test_encoding_update_uses_fixed_validated_argv(self,run):
        run.return_value=mock.Mock(returncode=0,stdout='{"codec":"h264"}',stderr="")
        value={"schema_version":1,"mode":"both","codec":"h264","source":"left_rectified",
               "width":0,"height":0,"cadence_hz":10,"bitrate_bps":4000000,
               "jpeg_quality":80,"keyframe_interval":1,"threads":4,
               "low_latency":True,"preset":"veryfast"}
        module.execute({"operation":"encoding.update","expected_revision":'"'+"a"*64+'"',"configuration":value})
        command=run.call_args.args[0]
        self.assertEqual(command[:4],["/usr/bin/nuvoctl","encoding","set","--mode"])
        self.assertIn("veryfast",command)
        self.assertFalse(run.call_args.kwargs.get("shell",False))
        color=dict(value,source="left_color")
        module.execute({"operation":"encoding.update","expected_revision":'"'+"a"*64+'"',"configuration":color})
        self.assertIn("left_color",run.call_args.args[0])

    @mock.patch.object(module.subprocess,"run")
    def test_stereo_update_uses_json_argv_without_shell(self,run):
        run.return_value=mock.Mock(returncode=0,stdout='{"algorithm":"bm"}',stderr="")
        value=json.loads((Path(__file__).parents[1]/"config/stereo.json").read_text())
        value["algorithm"]="bm"
        module.execute({"operation":"stereo.update","expected_revision":'"'+"a"*64+'"',"configuration":value})
        command=run.call_args.args[0]
        self.assertEqual(command[:4],["/usr/bin/nuvoctl","stereo","set","--json"])
        self.assertEqual(json.loads(command[4]),value)
        self.assertFalse(run.call_args.kwargs.get("shell",False))

    @mock.patch.object(module.subprocess,"run")
    def test_exposure_update_uses_json_argv_without_shell(self,run):
        run.return_value=mock.Mock(returncode=0,stdout='{"mode":"manual"}',stderr="")
        value=json.loads((Path(__file__).parents[1]/"config/exposure.json").read_text())
        value["manual_exposure_us"]=4000
        module.execute({"operation":"exposure.update","expected_revision":'"'+"a"*64+'"',"configuration":value})
        command=run.call_args.args[0]
        self.assertEqual(command[:4],["/usr/bin/nuvoctl","exposure","set","--json"])
        self.assertEqual(json.loads(command[4]),value)
        self.assertFalse(run.call_args.kwargs.get("shell",False))

    @mock.patch.object(module.subprocess,"run")
    def test_configuration_precondition_is_required_and_conflicts_remain_distinct(self,run):
        value=json.loads((Path(__file__).parents[1]/"config/stereo.json").read_text())
        for revision in (None,"*",'W/"'+'a'*64+'"'):
            with self.assertRaises(ValueError):
                module.execute({"operation":"stereo.update","configuration":value,"expected_revision":revision})
        run.assert_not_called()
        run.return_value=mock.Mock(returncode=3,stdout='{}',stderr='')
        result=module.execute({"operation":"stereo.update","configuration":value,"expected_revision":'"'+'a'*64+'"'})
        self.assertEqual(result["error_code"],"precondition_failed")
        self.assertEqual(run.call_args.args[0][-2:],["--if-match",'"'+'a'*64+'"'])

    @mock.patch.object(module.subprocess,"run")
    def test_calibration_create_uses_fixed_argv_without_shell(self,run):
        run.return_value=mock.Mock(returncode=0,stdout='{"session_id":"'+'a'*32+'"}',stderr="")
        configuration={"schema_version":1}
        result=module.execute({"operation":"calibration.create","configuration":configuration})
        command=run.call_args.args[0]
        self.assertEqual(command[0],"/usr/lib/nuvo-depth/nuvo-calibration")
        self.assertEqual(result["result"]["session_id"],"a"*32)
        self.assertFalse(run.call_args.kwargs.get("shell",False))

    def test_calibration_session_injection_is_rejected(self):
        for value in ("../active","a"*31,"g"*32):
            with self.assertRaises(ValueError):
                module.execute({"operation":"calibration.status","session_id":value})

    @mock.patch.object(module.subprocess,"run")
    def test_calibration_restart_is_part_of_the_durable_command(self,run):
        run.return_value=mock.Mock(returncode=0,stdout='{"state":"active"}',stderr="")
        for request in ({"operation":"calibration.activate","session_id":"a"*32},
                        {"operation":"calibration.rollback"}):
            run.reset_mock()
            module.execute(request)
            self.assertEqual(run.call_count,1)
            self.assertIn("--restart-depth-service",run.call_args.args[0])
            self.assertEqual(run.call_args.kwargs["timeout"],165)
        run.return_value=mock.Mock(returncode=1,stdout="",stderr="prior selection restored")
        with self.assertRaisesRegex(RuntimeError,"prior selection restored"):
            module.execute({"operation":"calibration.activate","session_id":"a"*32})


def calibration_bundle(root: Path, name: str) -> Path:
    path=root/name; path.mkdir()
    for asset in calibration.REQUIRED_ASSETS:
        (path/asset).write_bytes(b'{"status":"pass"}' if asset=="validation.json" else name.encode())
    manifest={"schema_version":1,"unit_id":"unit","left_camera_id":"left",
              "right_camera_id":"right","preset_id":"profile","authority":"user_validated",
              "assets":{asset:calibration.sha256(path/asset) for asset in calibration.REQUIRED_ASSETS}}
    manifest["bundle_id"]=hashlib.sha256(json.dumps(
        manifest,sort_keys=True,separators=(",",":")).encode()).hexdigest()
    calibration.atomic_json(path/"manifest.json",manifest)
    return path


class CalibrationRestartTransactionTest(unittest.TestCase):
    def test_browser_activation_reports_active_only_after_startup(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); state=root/"state"; session="a"*32
            session_path=state/"calibration-sessions"/session
            session_path.mkdir(parents=True)
            shutil.copytree(calibration_bundle(root,"one"),session_path/"bundle")
            status_path=session_path/"status.json"
            calibration.atomic_json(status_path,{"state":"passed","active":False,
                "bundle_id":calibration.validate_bundle(session_path/"bundle")["bundle_id"]})
            def ready(bundle_id):
                self.assertEqual(json.loads(status_path.read_text())["state"],"activating")
                self.assertFalse(calibration.read_web_status(state,session)["active"])
            with mock.patch.object(calibration,"restart_depth_service",side_effect=ready):
                status=calibration.activate_web_session(state,session,restart_depth=True)
            self.assertEqual(status["state"],"active")
            self.assertTrue(calibration.read_web_status(state,session)["active"])

    def test_activation_commits_only_after_ready_callback(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); store=calibration.CalibrationStore(root/"state")
            item=calibration_bundle(root,"one")
            def ready(bundle_id):
                self.assertEqual(os.readlink(store.calibrations/"user"/"active"),bundle_id)
                records=[json.loads(path.read_text()) for path in store.transactions.glob("calibration-*.json")]
                self.assertEqual(records[-1]["state"],"awaiting_restart")
            store.activate(item,restart=ready)
            record=json.loads(next(store.transactions.glob("calibration-*.json")).read_text())
            self.assertEqual(record["state"],"committed")

    def test_failed_first_activation_restores_no_active_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); store=calibration.CalibrationStore(root/"state")
            ready=mock.Mock(side_effect=[RuntimeError("startup rejected geometry"),None])
            with self.assertRaisesRegex(RuntimeError,"previous selection and service restored"):
                store.activate(calibration_bundle(root,"one"),restart=ready)
            self.assertFalse((store.calibrations/"user"/"active").is_symlink())
            self.assertIsNone(ready.call_args.args[0])
            record=json.loads(next(store.transactions.glob("calibration-*.json")).read_text())
            self.assertEqual(record["state"],"aborted")

    def test_failed_activation_restores_previous_bundle_and_session_status(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); state=root/"state"; store=calibration.CalibrationStore(state)
            previous=store.activate(calibration_bundle(root,"one"))
            session="a"*32; session_path=state/"calibration-sessions"/session
            session_path.mkdir(parents=True)
            shutil.copytree(calibration_bundle(root,"two"),session_path/"bundle")
            original={"state":"passed","session_id":session,"active":False,
                      "bundle_id":calibration.validate_bundle(session_path/"bundle")["bundle_id"]}
            calibration.atomic_json(session_path/"status.json",original)
            with mock.patch.object(calibration,"restart_depth_service",
                                   side_effect=[RuntimeError("startup failed"),None]) as ready:
                with self.assertRaisesRegex(RuntimeError,"previous selection and service restored"):
                    calibration.activate_web_session(state,session,restart_depth=True)
                self.assertEqual(ready.call_args.args[0],previous)
            self.assertEqual(os.readlink(store.calibrations/"user"/"active"),previous)
            restored=json.loads((session_path/"status.json").read_text())
            for key,value in original.items(): self.assertEqual(restored[key],value)
            self.assertEqual(restored["last_error"]["operation"],"activate")
            self.assertIn("startup failed",restored["last_error"]["message"])
            self.assertFalse(calibration.read_web_status(state,session)["active"])

    def test_failed_rollback_restores_the_current_bundle(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); store=calibration.CalibrationStore(root/"state")
            previous=store.activate(calibration_bundle(root,"one"))
            current=store.activate(calibration_bundle(root,"two"))
            ready=mock.Mock(side_effect=[RuntimeError("prior bundle cannot start"),None])
            with self.assertRaisesRegex(RuntimeError,"previous selection and service restored"):
                store.rollback(restart=ready)
            self.assertEqual(ready.call_args_list,[mock.call(previous),mock.call(current)])
            self.assertEqual(os.readlink(store.calibrations/"user"/"active"),current)

    def test_service_recovery_failure_is_retained_and_recovered_before_retry(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); store=calibration.CalibrationStore(root/"state")
            item=calibration_bundle(root,"one")
            ready=mock.Mock(side_effect=[RuntimeError("bad startup"),RuntimeError("recovery failed")])
            with self.assertRaisesRegex(RuntimeError,"service recovery failed"):
                store.activate(item,restart=ready)
            record=json.loads(next(store.transactions.glob("calibration-*.json")).read_text())
            self.assertEqual(record["state"],"restoring")
            retry=mock.Mock()
            with self.assertRaisesRegex(RuntimeError,"interrupted calibration transaction restored"):
                store.activate(item,restart=retry)
            retry.assert_called_once_with(None)
            record=json.loads(next(store.transactions.glob("calibration-*.json")).read_text())
            self.assertEqual(record["state"],"aborted")
            store.activate(item,restart=retry)

    def test_interrupted_staging_without_namespace_directory_can_be_recovered(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); store=calibration.CalibrationStore(root/"state")
            calibration.atomic_json(store.transactions/"calibration-1.json",{
                "schema_version":1,"namespace":"user","bundle_id":"a"*64,
                "previous":None,"state":"staging"})
            store.activate(calibration_bundle(root,"one"))
            self.assertEqual(json.loads((store.transactions/"calibration-1.json").read_text())["state"],"aborted")

    def test_factory_precedence_rejects_user_runtime_activation(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); store=calibration.CalibrationStore(root/"state")
            factory=store.calibrations/"factory"/"active"; factory.mkdir(parents=True)
            (factory/"manifest.json").write_text("{}")
            ready=mock.Mock()
            with self.assertRaisesRegex(ValueError,"factory calibration has runtime precedence"):
                store.activate(calibration_bundle(root,"one"),restart=ready)
            ready.assert_not_called()
            self.assertFalse((store.calibrations/"user"/"active").is_symlink())

    def test_rollback_does_not_leave_previous_session_reported_active(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); state=root/"state"; store=calibration.CalibrationStore(state)
            store.activate(calibration_bundle(root,"one"))
            current=store.activate(calibration_bundle(root,"two"))
            session="a"*32; status_path=state/"calibration-sessions"/session/"status.json"
            calibration.atomic_json(status_path,{"state":"active","bundle_id":current,"active":True})
            self.assertTrue(calibration.read_web_status(state,session)["active"])
            store.rollback(restart=mock.Mock())
            status=calibration.read_web_status(state,session)
            self.assertEqual(status["state"],"passed")
            self.assertFalse(status["active"])

    def test_notify_readiness_must_publish_the_requested_bundle_in_a_new_run(self):
        with tempfile.TemporaryDirectory() as directory:
            status_path=Path(directory)/"status.json"
            for run_uuid,bundle_id,accepted in (("new","a"*64,True),
                                                ("old","a"*64,False),
                                                ("new","b"*64,False)):
                calibration.atomic_json(status_path,{"run_uuid":"old"})
                def run(command,**kwargs):
                    if command[1]=="restart":
                        calibration.atomic_json(status_path,{
                            "status":"running","run_uuid":run_uuid,
                            "calibration_sha256":bundle_id,"calibration_authority":1})
                    return mock.Mock(returncode=0,stderr="",stdout="")
                with mock.patch.object(calibration.subprocess,"run",side_effect=run):
                    if accepted:
                        calibration.restart_depth_service("a"*64,status_path)
                    else:
                        with self.assertRaises(RuntimeError):
                            calibration.restart_depth_service("a"*64,status_path)


if __name__=="__main__": unittest.main()
