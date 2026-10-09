"""Decode real native wire fixtures, including three-channel color ordering."""

from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).parents[1] / "sdk/python"))
from nuvo_depth import RecordingReader, RecordingWriter, decode_frame


def check_frame(frame, data_format):
    assert frame.capture_epoch == 1
    assert frame.capture_sequence == 2
    assert frame.configuration_epoch == 3
    assert frame.device_uuid == "01" + "00" * 15
    assert frame.run_uuid == "02" + "00" * 15
    assert frame.calibration_sha256 == "03" + "00" * 31
    assert set(frame.products) == {"left_rectified", "right_rectified"}
    for name, expected in (
        ("left_rectified", bytes((1, 27, 203, 4, 55, 161))),
        ("right_rectified", bytes((207, 31, 2, 167, 59, 5))),
    ):
        product = frame.products[name]
        assert product.format == data_format
        assert (product.width, product.height) == (2, 1)
        assert bytes(product.data) == expected


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    subprocess.run([sys.argv[1], directory], check=True)
    extended = decode_frame((root / "extended.nuv").read_bytes())
    assert len(extended.products) == 14
    assert extended.metadata["available_products"] == 16383
    assert extended.metadata["selected_products"] == 16383
    assert (extended.products["rectified_stereo_rgb"].width,
            extended.products["rectified_stereo_rgb"].height) == (4, 1)
    assert bytes(extended.products["rectified_stereo_rgb"].data) == bytes(
        (5, 15, 25, 6, 16, 26, 7, 17, 27, 8, 18, 28))
    assert bytes(extended.products["left_mono"].data) == bytes((5, 6))
    for data_format in ("bgr8", "rgb8"):
        wire = (root / f"{data_format}.nuv").read_bytes()
        frame = decode_frame(wire)
        check_frame(frame, data_format)
        recording = root / f"{data_format}.nvr"
        with RecordingWriter(recording) as writer:
            writer.write(frame)
        with RecordingReader(recording) as reader:
            frames = list(reader)
        assert len(frames) == 1
        check_frame(frames[0], data_format)

print("frame_wire_sdk_roundtrip_test: pass")
