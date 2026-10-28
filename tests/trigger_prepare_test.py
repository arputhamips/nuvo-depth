"""Verify crash cleanup cannot write unrelated PWM channels."""
import importlib.util
import io
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stderr
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('trigger_prepare', Path(__file__).resolve().parents[1] / 'tools/pi/nuvo_trigger_prepare.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class TriggerPrepareTest(unittest.TestCase):
    def test_config_dump_timeout_terminates_a_real_stalled_child(self):
        with tempfile.TemporaryDirectory() as directory:
            depthd = Path(directory) / 'depthd'
            depthd.write_text('#!' + sys.executable + '\nimport time\ntime.sleep(10)\n')
            depthd.chmod(0o755)
            started = time.monotonic()
            with patch.object(module, 'CONFIG_DUMP_TIMEOUT_SECONDS', 0.05):
                with self.assertRaisesRegex(RuntimeError, 'Configuration dump exceeded.*host process/I/O stalls'):
                    module.configuration(str(depthd), 'daemon.yaml')
            self.assertLess(time.monotonic() - started, 2)

    def test_config_dump_failure_preserves_bounded_actionable_stderr(self):
        failure = subprocess.CalledProcessError(2, ['depthd'], stderr='invalid camera profile\n' + 'x' * 2000)
        with patch.object(module.subprocess, 'check_output', side_effect=failure) as dump:
            with self.assertRaisesRegex(RuntimeError, 'exit 2.*invalid camera profile') as raised:
                module.configuration('/usr/bin/depthd', 'daemon.yaml')
        self.assertLess(len(str(raised.exception)), 600)
        self.assertEqual(dump.call_args.kwargs['timeout'], 5)
        self.assertEqual(dump.call_args.kwargs['stderr'], subprocess.PIPE)

    def test_stage_diagnostics_are_flushed_and_keep_failure_phase_and_monotonic_duration(self):
        with patch.object(module.time, 'monotonic', side_effect=[10.0, 10.25]), patch('builtins.print') as output:
            with self.assertRaisesRegex(RuntimeError, r'channel\s+unavailable'):
                with module.stage('find_channel'):
                    raise RuntimeError('channel\nunavailable')
        messages = [call.args[0] for call in output.call_args_list]
        self.assertEqual(len(messages), 2)
        self.assertIn('stage=find_channel event=start monotonic_s=10.000000', messages[0])
        self.assertIn('stage=find_channel event=error monotonic_s=10.250000 elapsed_s=0.250000', messages[1])
        self.assertIn('RuntimeError: channel unavailable', messages[1])
        for call in output.call_args_list:
            self.assertIs(call.kwargs['file'], sys.stderr)
            self.assertTrue(call.kwargs['flush'])

    def test_config_failure_does_not_touch_existing_pwm_channel(self):
        with patch('sys.argv', ['nuvo-trigger-prepare', 'daemon.yaml']), \
                patch.object(module.subprocess, 'check_output', side_effect=subprocess.TimeoutExpired(['depthd'], 5)), \
                patch.object(module, 'disable_owned_channel') as cleanup, redirect_stderr(io.StringIO()) as log:
            with self.assertRaisesRegex(RuntimeError, 'Configuration dump exceeded'):
                module.main()
        cleanup.assert_not_called()
        self.assertIn('stage=config_dump event=start', log.getvalue())
        self.assertIn('stage=config_dump event=error', log.getvalue())
        self.assertNotIn('stage=cleanup_previous_channel', log.getvalue())

    def test_verified_channel_cleanup_and_wrong_controller_rejection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            chip = root / 'pwmchip19'
            node = chip / 'device/of_node'
            node.mkdir(parents=True)
            (node / 'compatible').write_bytes(b'raspberrypi,pwm-pio-rp1\0')
            (node / 'gpios').write_bytes(struct.pack('>III', 9, 17, 0))
            channel = chip / 'pwm0'
            channel.mkdir()
            (channel / 'enable').write_text('1')
            link = root / 'owned'
            link.symlink_to(channel)
            values = list(link.lstat()); values[4] = 0
            with patch.object(Path, 'lstat', return_value=os.stat_result(values)):
                self.assertEqual(module.find_channel(17, root), chip)
                self.assertIsNone(module.find_channel(18, root))
                module.disable_owned_channel(link, root)
                self.assertEqual((channel / 'enable').read_text(), '0')
                (channel / 'enable').write_text('1')
                (node / 'compatible').write_bytes(b'raspberrypi,rp1-pwm\0')
                with self.assertRaisesRegex(RuntimeError, 'RP1 PIO PWM'):
                    module.disable_owned_channel(link, root)
                self.assertEqual((channel / 'enable').read_text(), '1')

    def test_disabled_configuration_still_cleans_old_waveform(self):
        config = b'{"configuration":{"capture":{"hardware_trigger":{"enabled":false}}}}'
        with patch('sys.argv', ['nuvo-trigger-prepare', 'daemon.yaml']), patch.object(module.subprocess, 'check_output', return_value=config) as dump, patch.object(module, 'disable_owned_channel') as cleanup:
            module.main()
            cleanup.assert_called_once()
            self.assertEqual(dump.call_args.args[0],
                             ['/usr/lib/nuvo-depth/depthd-config', '--dump-effective-config', 'daemon.yaml'])

    def test_missing_config_utility_never_falls_back_to_heavy_daemon(self):
        with patch('sys.argv', ['nuvo-trigger-prepare', 'daemon.yaml']), \
                patch.object(module.subprocess, 'check_output', side_effect=FileNotFoundError('missing config utility')) as dump, \
                patch.object(module, 'disable_owned_channel') as cleanup, redirect_stderr(io.StringIO()):
            with self.assertRaises(FileNotFoundError):
                module.main()
        dump.assert_called_once()
        cleanup.assert_not_called()

    def test_stop_does_not_require_working_daemon_configuration(self):
        with patch('sys.argv', ['nuvo-trigger-prepare', '--stop']), patch.object(module, 'disable_owned_channel') as cleanup, patch.object(module.subprocess, 'check_output') as dump:
            module.main()
            cleanup.assert_called_once()
            dump.assert_not_called()


if __name__ == '__main__':
    unittest.main()
