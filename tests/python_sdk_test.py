import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / "sdk/python"))
from nuvo_depth import RecordingReader, RecordingWriter, decode_encoded_frame, decode_frame


def sample():
    payload = b"\x01\x02"
    metadata = json.dumps({
        "schema_version": 1, "capture_epoch": 2, "capture_sequence": 3,
        "capture_time_ns": 4, "configuration_epoch": 5,
        "calibration_sha256": "00" * 32, "calibration_authority": 0,
        "products": [{"kind":"left_rectified", "format":"gray8",
                      "offset":0, "length":2, "width":2, "height":1}],
    }, separators=(",", ":")).encode()
    return struct.pack(">4sHHHHIQQ", b"NUV1", 1, 0, 5, 0,
                       len(metadata), len(payload), 1) + metadata + payload


class SdkTest(unittest.TestCase):
    def test_decode_and_recording_roundtrip(self):
        frame = decode_frame(sample())
        self.assertEqual(frame.capture_sequence, 3)
        self.assertEqual(bytes(frame.products["left_rectified"].data), b"\x01\x02")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.nvr"
            with RecordingWriter(path) as writer: writer.write(frame)
            with RecordingReader(path) as reader:
                frames = list(reader)
            self.assertEqual(len(frames), 1)
            self.assertEqual(frames[0].capture_sequence, 3)

    def test_rejects_overlap_and_truncation(self):
        self.assertRaises(ValueError, decode_frame, sample()[:-1])
        data = bytearray(sample()); data[0] = 0
        self.assertRaises(ValueError, decode_frame, bytes(data))

    def test_decodes_bounded_encoded_envelope(self):
        payload=b"\xff\xd8jpeg\xff\xd9"
        metadata=json.dumps({"schema_version":1,"capture_epoch":2,"capture_sequence":3,
            "capture_time_ns":4,"configuration_epoch":5,"source":"left_rectified",
            "codec":"jpeg","mime_type":"image/jpeg","codec_string":"jpeg",
            "width":2,"height":2,"keyframe":True,"encode_ms":1.5},separators=(",",":")).encode()
        wire=struct.pack(">4sHHIQQI",b"NVE1",1,1,len(metadata),len(payload),7,0)+metadata+payload
        frame=decode_encoded_frame(wire)
        self.assertEqual(frame.stream_sequence,7)
        self.assertEqual(frame.codec,"jpeg")
        self.assertEqual(bytes(frame.data),payload)
        self.assertRaises(ValueError,decode_encoded_frame,wire[:-1])


if __name__ == "__main__": unittest.main()
