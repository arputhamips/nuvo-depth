"""Matcher observations must stay independent of faster mono publications."""
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).parents[1] / "tools/pi/collect_live_performance.py"
spec = importlib.util.spec_from_file_location("live_performance", SOURCE)
collector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collector)


def health(capture, matched, matcher_sequence, processing=300):
    return {"status": "running", "run_uuid": "a", "capture_epoch": 1,
            "capture_sequence": capture, "captured_pairs": capture,
            "published_frames": capture, "matched_frames": matched,
            "stereo_backend": "not_requested", "processing_ms": 1,
            "valid_fraction": 0, "depth_width": 0, "depth_height": 0,
            "matcher_statistics": {"capture_sequence": matcher_sequence,
                                   "processing_ms": processing,
                                   "rectification_ms": 5,
                                   "stereo_preprocess_ms": 1,
                                   "stereo_match_ms": processing - 10,
                                   "stereo_postprocess_ms": 2}}


class LivePerformanceTest(unittest.TestCase):
    def test_mono_publications_do_not_create_matcher_samples(self):
        a = collector.matcher_sample(health(10, 1, 9))
        b = collector.matcher_sample(health(20, 1, 9))
        self.assertEqual(a[0], b[0])
        self.assertEqual(a[1]["processing_ms"], 300)
        self.assertNotIn("valid_fraction", a[1])
        self.assertNotIn("depth_width", a[1])
        c = collector.matcher_sample(health(21, 2, 19))
        self.assertNotEqual(a[0], c[0])
        self.assertIsNone(collector.matcher_sample(health(1, 0, 0)))
        old = health(1, 1, 1); del old["matched_frames"]
        self.assertIsNone(collector.matcher_sample(old))

    def test_rates_use_distinct_capture_and_matcher_counters(self):
        polls = [dict(health(10, 4, 9), sample_monotonic_seconds=0),
                 dict(health(20, 5, 19), sample_monotonic_seconds=1),
                 dict(health(30, 6, 29), sample_monotonic_seconds=2)]
        self.assertEqual(collector.counter_rate(polls, "matched_frames"), 1)
        self.assertEqual(collector.counter_rate(polls, "captured_pairs"), 10)
        polls[-1]["run_uuid"] = "restarted"
        self.assertIsNone(collector.counter_rate(polls, "matched_frames"))
        polls[-1]["run_uuid"] = "a"; polls[-1]["matched_frames"] = 0
        self.assertIsNone(collector.counter_rate(polls, "matched_frames"))

    def test_quality_fields_follow_processing_identity_not_latest_capture(self):
        current = health(20, 1, 9)
        current.update(processing_statistics_capture_sequence=9,
                       stereo_backend="opencv_sgbm_cpu", valid_fraction=.75,
                       depth_width=728, depth_height=544)
        _, sample = collector.matcher_sample(current)
        self.assertEqual(sample["valid_fraction"], .75)
        self.assertEqual(sample["depth_width"], 728)
        current["processing_statistics_capture_sequence"] = 19
        _, sample = collector.matcher_sample(current)
        self.assertNotIn("valid_fraction", sample)
        self.assertNotIn("depth_width", sample)
        # Older producers that align top-level capture identity still work.
        del current["processing_statistics_capture_sequence"]
        current["capture_sequence"] = 9
        self.assertEqual(collector.matcher_sample(current)[1]["valid_fraction"], .75)

    def test_cli_uses_nested_timings_and_excludes_repeated_polls(self):
        snapshots = iter([health(1, 0, 0), health(10, 1, 9, 300),
                          health(11, 1, 9, 300), health(20, 2, 19, 400)])
        clock = iter(n / 10 for n in range(100))
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "result.json"
            with patch.object(sys, "argv", [str(SOURCE), "--frames", "2", "--output", str(output)]), \
                 patch.object(collector, "get_json", return_value={"mode": "raw"}), \
                 patch.object(collector, "get_bytes", side_effect=lambda *_: (json.dumps(next(snapshots)).encode(), 1)), \
                 patch.object(collector.time, "monotonic", side_effect=lambda: next(clock)), \
                 patch.object(collector.time, "sleep"):
                collector.main()
            result = json.loads(output.read_text())
        self.assertEqual(result["distinct_depth_frames"], 2)
        self.assertEqual(result["stages"]["processing_ms"]["median"], 350)
        self.assertEqual(result["valid_fraction"], None)
        self.assertEqual(result["grid_sizes"], [])
        self.assertLess(result["depth_output_rate_hz"], result["accepted_pair_rate_hz"])

    def test_encoded_bounds_use_the_current_sdk_decoder(self):
        metadata = json.dumps({"schema_version": 1, "capture_epoch": 1,
            "capture_sequence": 1, "capture_time_ns": 10, "source": "left_color",
            "codec": "h264", "mime_type": "video/h264", "width": 2,
            "height": 1, "keyframe": True, "encode_ms": 3}).encode()
        packet = struct.pack(">4sHHIQQI", b"NVE1", 1, 1, len(metadata), 1, 1, 0) + metadata + b"x"
        self.assertEqual(collector.encoded_metadata(packet)["payload_bytes"], 1)
        for invalid in (packet[:-1], b"bad" + packet[3:], packet + b"extra"):
            with self.assertRaises(ValueError):
                collector.encoded_metadata(invalid)


if __name__ == "__main__":
    unittest.main()
