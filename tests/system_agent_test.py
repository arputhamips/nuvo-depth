import importlib.util
import json
from pathlib import Path
import unittest
from unittest import mock

SOURCE=Path(__file__).parents[1]/"tools/nuvo_system_agent.py"
spec=importlib.util.spec_from_file_location("nuvo_system_agent",SOURCE)
module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)


class SystemAgentTest(unittest.TestCase):
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
        ):
            with self.assertRaises(ValueError): module.execute(request)

    @mock.patch.object(module.subprocess,"run")
    def test_encoding_update_uses_fixed_validated_argv(self,run):
        run.return_value=mock.Mock(returncode=0,stdout='{"codec":"h264"}',stderr="")
        value={"schema_version":1,"mode":"both","codec":"h264","source":"left_rectified",
               "width":0,"height":0,"cadence_hz":10,"bitrate_bps":4000000,
               "jpeg_quality":80,"keyframe_interval":30,"threads":4,
               "low_latency":True,"preset":"veryfast"}
        module.execute({"operation":"encoding.update","configuration":value})
        command=run.call_args.args[0]
        self.assertEqual(command[:4],["/usr/bin/nuvoctl","encoding","set","--mode"])
        self.assertIn("veryfast",command)
        self.assertFalse(run.call_args.kwargs.get("shell",False))

    @mock.patch.object(module.subprocess,"run")
    def test_stereo_update_uses_json_argv_without_shell(self,run):
        run.return_value=mock.Mock(returncode=0,stdout='{"algorithm":"bm"}',stderr="")
        value=json.loads((Path(__file__).parents[1]/"config/stereo.json").read_text())
        value["algorithm"]="bm"
        module.execute({"operation":"stereo.update","configuration":value})
        command=run.call_args.args[0]
        self.assertEqual(command[:4],["/usr/bin/nuvoctl","stereo","set","--json"])
        self.assertEqual(json.loads(command[4]),value)
        self.assertFalse(run.call_args.kwargs.get("shell",False))


if __name__=="__main__": unittest.main()
