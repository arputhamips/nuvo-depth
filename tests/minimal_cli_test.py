"""CLI configuration/export contracts without cameras or privileged writes."""
import argparse
import io
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import nuvo_minimal_cli as cli


def frame():
    metadata = {'schema_version': 1, 'depth_unit': 'metres', 'calibration_authority': 1,
                'products': [dict(kind='depth', format='float32_le', width=2, height=1, offset=0, length=8),
                             dict(kind='validity', format='uint8', width=2, height=1, offset=8, length=2)]}
    document = json.dumps(metadata).encode()
    payload = struct.pack('<ffBB', 1.25, float('nan'), 1, 0)
    return cli.HEADER.pack(b'NUV1', 1, 0, 5, 0, len(document), len(payload), 1)+document+payload


class MinimalCliTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(); self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root/'etc/nuvo-depth').mkdir(parents=True)
        for name in ('stereo.json', 'exposure.json'):
            shutil.copyfile(ROOT/'config'/name, self.root/'etc/nuvo-depth'/name)

    def test_partial_configuration_uses_existing_validation_and_atomic_writes(self):
        cli.set_value(self.root, 'exposure.manual_exposure_us', '6000')
        self.assertEqual(cli.configuration(self.root)['exposure']['manual_exposure_us'], 6000)
        before = (self.root/'etc/nuvo-depth/stereo.json').read_bytes()
        for key, value in [('stereo.search_range','17'), ('stereo.block_size','6'),
                           ('stereo.unknown','4'), ('exposure.manual_analogue_gain','NaN')]:
            with self.assertRaises(ValueError): cli.set_value(self.root, key, value)
        self.assertEqual((self.root/'etc/nuvo-depth/stereo.json').read_bytes(), before)

    def test_wizard_validates_all_controls_before_writing(self):
        before = cli.configuration(self.root)
        answers = iter(['auto', '', '', '', '17', '', '', ''])
        with self.assertRaises(ValueError): cli.configure(self.root, lambda _: next(answers))
        self.assertEqual(cli.configuration(self.root), before)

    def test_depth_and_validity_export_retains_geometry_and_authority(self):
        metadata, depth, validity = cli.decode_capture(frame())
        self.assertEqual(struct.unpack('<f', depth[:4])[0], 1.25)
        self.assertEqual(validity, b'\x01\x00')
        self.assertEqual(metadata['calibration_authority'], 1)
        output = self.root/'depth.f32'
        args = argparse.Namespace(frames=1, hz=10.0, timeout_seconds=10, format='f32', output=str(output), reader='reader', socket='socket')
        with mock.patch.object(cli.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, frame())):
            cli.depth_output(args)
            with self.assertRaises(FileExistsError): cli.depth_output(args)
        self.assertEqual(output.read_bytes(), depth)
        self.assertEqual(Path(str(output)+'.validity.u8').read_bytes(), validity)
        self.assertEqual(json.loads(Path(str(output)+'.json').read_text()), metadata)

    def test_malformed_frame_and_stream_as_f32_are_rejected(self):
        for data in (b'', frame()[:-1], frame()+b'x', b'FAIL'+frame()[4:]):
            with self.assertRaises(ValueError): cli.decode_capture(data)
        args = argparse.Namespace(frames=0, hz=10.0, timeout_seconds=10, format='f32', output='-')
        with self.assertRaises(ValueError): cli.depth_output(args)

    def test_failed_capture_removes_only_its_own_partial_files(self):
        target = self.root/'depth.f32'
        args = argparse.Namespace(frames=1, hz=10.0, timeout_seconds=10, format='f32', output=str(target), reader='reader', socket='socket')
        sentinel = Path(str(target)+'.json'); sentinel.write_text('keep')
        with self.assertRaises(FileExistsError): cli.depth_output(args)
        self.assertFalse(target.exists()); self.assertEqual(sentinel.read_text(), 'keep')
        sentinel.unlink()
        with mock.patch.object(cli.subprocess, 'run', side_effect=subprocess.CalledProcessError(2, ['reader'])):
            with self.assertRaises(subprocess.CalledProcessError): cli.depth_output(args)
        self.assertFalse(any(self.root.glob('depth.f32*')))

    def test_help_exposes_only_terminal_configuration_and_raw_output(self):
        result = subprocess.check_output([sys.executable, str(ROOT/'tools/nuvo_minimal_cli.py'), '--help'], text=True)
        self.assertIn('configure', result); self.assertIn('depth', result)
        for excluded in ('wifi', 'transport', 'encoding', 'identity', 'usb'):
            self.assertNotIn(excluded, result)


if __name__ == '__main__': unittest.main()
