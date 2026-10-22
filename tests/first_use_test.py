"""First-use completion rejects stale/invalid data and preserves authority."""
from contextlib import contextmanager
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'sdk/python'))
from nuvo_depth import decode_frame, RecordingReader
from nuvo_depth.first_use import capture, describe


def sample(sequence=1, authority=0, validity=1):
    payload = struct.pack('<ffBB', 1.25, float('nan'), validity, 0)
    metadata = json.dumps(dict(schema_version=1, device_uuid='22'*16, run_uuid='11'*16,
        capture_epoch=1, capture_sequence=sequence, capture_time_ns=sequence*1000000000,
        configuration_epoch=1, calibration_sha256='ab'*32, calibration_authority=authority,
        products=[dict(kind='depth', format='float32_le', width=2, height=1, offset=0, length=8),
                  dict(kind='validity', format='uint8', width=2, height=1, offset=8, length=2)])).encode()
    return decode_frame(struct.pack('>4sHHHHIQQ',b'NUV1',1,0,5,0,len(metadata),len(payload),sequence)+metadata+payload)


class Client:
    def __init__(self, samples): self.samples = samples
    @contextmanager
    def frames(self, **kwargs):
        assert kwargs['products'] == ('depth', 'validity')
        yield iter(self.samples)


class FirstUseTest(unittest.TestCase):
    def test_preview_capture_and_recording_preserve_original_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory)/'first.nvr'
            report = capture(Client([sample(), sample(), sample(2)]), target)
            self.assertIn('accuracy unverified', report)
            self.assertIn('1.2500 m', report)
            with RecordingReader(target) as reader:
                frame = next(iter(reader))
                self.assertEqual(frame.calibration_authority, 0)
                self.assertEqual(frame.capture_sequence, 2)
                self.assertEqual(set(frame.products), {'depth', 'validity'})
            with self.assertRaises(FileExistsError):
                capture(Client([sample(), sample(2)]), target)

    def test_duplicate_only_invalid_and_empty_frames_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            target=Path(directory)/'unused.nvr'
            with self.assertRaisesRegex(RuntimeError, 'two advancing'):
                capture(Client([sample(), sample()]), target)
            with self.assertRaisesRegex(RuntimeError, 'two advancing'):
                capture(Client([sample(2), sample(1), sample(2)]), target)
            with self.assertRaisesRegex(ValueError, 'No valid depth'):
                capture(Client([sample(validity=0)]), target)
            self.assertFalse(target.exists())
        self.assertIn('User-validated', describe(sample(authority=1)))
        self.assertIn('Factory-validated', describe(sample(authority=2)))


if __name__ == '__main__': unittest.main()
