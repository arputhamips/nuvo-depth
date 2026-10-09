"""Verify crash cleanup cannot write unrelated PWM channels."""
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('trigger_prepare', Path(__file__).resolve().parents[1] / 'tools/pi/nuvo_trigger_prepare.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class TriggerPrepareTest(unittest.TestCase):
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
        with patch('sys.argv', ['nuvo-trigger-prepare', 'daemon.yaml']), patch.object(module.subprocess, 'check_output', return_value=config), patch.object(module, 'disable_owned_channel') as cleanup:
            module.main()
            cleanup.assert_called_once()

    def test_stop_does_not_require_working_daemon_configuration(self):
        with patch('sys.argv', ['nuvo-trigger-prepare', '--stop']), patch.object(module, 'disable_owned_channel') as cleanup, patch.object(module.subprocess, 'check_output') as dump:
            module.main()
            cleanup.assert_called_once()
            dump.assert_not_called()


if __name__ == '__main__':
    unittest.main()
