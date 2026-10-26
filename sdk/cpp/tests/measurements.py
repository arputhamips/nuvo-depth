"""Universal wire measurement invariants, independent of ROS/consumer policy."""
import copy
import gzip
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from transports import HEADER, encode, frame


def main():
    packet = frame()
    meta_size = HEADER.unpack_from(packet)[5]
    metadata = json.loads(packet[32:32 + meta_size])
    metadata['stereo_configuration_error'] = ''
    metadata['depth_metadata'] = {'minimum_m': 0.1, 'maximum_m': 20.0, 'invalid_is_nan': True}
    metadata['disparity_metadata'] = {
        'accepted_min_px': 0.5, 'accepted_max_px': 64.0,
        'quantization_px': 0.0625, 'invalid_is_nan': True,
        'unit': 'matching_grid_pixels', 'valid_roi': {'x': 0, 'y': 0, 'width': 2, 'height': 1}}
    metadata['products'].insert(0, {'kind': 'disparity', 'format': 'float32_le',
        'width': 2, 'height': 1, 'offset': 0, 'length': 8})
    metadata['products'][1]['offset'] = 8
    metadata['products'][2]['offset'] = 16
    metadata['selected_products'] = metadata['available_products'] = 44
    payload = struct.pack('<ffffBB', 5.0, math.nan, 2.0, math.nan, 1, 6)
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        path = root / 'measurement.nuv'

        def check(document=metadata, samples=payload, accepted=True, typed=False):
            path.write_bytes(encode(document, samples))
            command = [sys.argv[1], 'validate', str(path)]
            if typed:
                command.append('constraints')
            result = subprocess.run(command, capture_output=True, text=True, timeout=10)
            assert (result.returncode == 0) == accepted, (document, samples.hex(), result.stderr)

        check(typed=True)
        # Original report: an accepted 200m value, maximum 20m, disparity implying 2m.
        check(samples=struct.pack('<ffffBB', 5., math.nan, 200., math.nan, 1, 6), accepted=False)
        # Still reject a contradiction entirely inside both declared ranges.
        check(samples=struct.pack('<ffffBB', 5., math.nan, 5., math.nan, 1, 6), accepted=False)
        # Known validity is 0..11; only 1 means accepted, and all invalid depth is NaN.
        for code in range(12):
            check(samples=struct.pack('<ffffBB', 5., 2., 2., math.nan, 1, code), accepted=code != 1)
        for code in (12, 255):
            check(samples=struct.pack('<ffffBB', 5., math.nan, 2., math.nan, 1, code), accepted=False)
        for values in ((5., math.nan, 0., math.nan), (5., math.nan, math.inf, math.nan),
                       (math.inf, math.nan, 2., math.nan), (5., math.nan, math.nan, math.nan),
                       (5., math.nan, 2., 1.), (64., math.nan, 2., math.nan)):
            check(samples=struct.pack('<ffffBB', *values, 1, 6), accepted=False)
        for target, key, value in (
                ('depth_metadata', 'maximum_m', 1.), ('depth_metadata', 'minimum_m', True),
                ('depth_metadata', 'maximum_m', '20'), ('depth_metadata', 'invalid_is_nan', 1),
                ('disparity_metadata', 'accepted_min_px', 6.),
                ('disparity_metadata', 'accepted_max_px', 5.),
                ('disparity_metadata', 'quantization_px', 0.),
                ('disparity_metadata', 'unit', 'image_pixels'),
                ('disparity_metadata', 'invalid_is_nan', False),
                ('disparity_metadata', 'valid_roi', {'x': 1, 'y': 0, 'width': 1, 'height': 1}),
                ('disparity_metadata', 'valid_roi', {'x': 0, 'y': 0, 'width': 0, 'height': 1}),
                ('disparity_metadata', 'valid_roi', {'x': 1, 'y': 0, 'width': 2, 'height': 1})):
            invalid = copy.deepcopy(metadata)
            invalid[target][key] = value
            check(invalid, accepted=False)
        for value in ('rejected range', 0, None):
            invalid = copy.deepcopy(metadata)
            invalid['stereo_configuration_error'] = value
            check(invalid, accepted=False)
        for key in ('minimum_m', 'invalid_is_nan'):
            invalid = copy.deepcopy(metadata)
            del invalid['depth_metadata'][key]
            check(invalid, accepted=False)
        invalid = copy.deepcopy(metadata)
        invalid['products'][0]['width'] = 1
        check(invalid, accepted=False)
        # Principal-point offset is part of the native Z = fB/(d - (cxL-cxR)).
        offset = copy.deepcopy(metadata)
        for geometry in offset['calibration_geometry'].values():
            geometry['right_projection'][2] = -1.
            geometry['reprojection'][15] = -20.
        check(offset, struct.pack('<ffffBB', 7., math.nan, 2., math.nan, 1, 6))
        check(offset, payload, accepted=False)
        without_validity = copy.deepcopy(metadata)
        without_validity['products'].pop()
        without_validity['selected_products'] = without_validity['available_products'] = 12
        check(without_validity, payload[:-2])
        check(without_validity, struct.pack('<ffff', 5., 2., 2., math.nan))
        check(without_validity, struct.pack('<ffff', 5., math.nan, 5., math.nan), accepted=False)
        disparity_only = copy.deepcopy(metadata)
        disparity_only['products'] = disparity_only['products'][:1]
        disparity_only['selected_products'] = disparity_only['available_products'] = 4
        check(disparity_only, struct.pack('<ff', 5., math.nan))
        check(disparity_only, struct.pack('<ff', 64., math.nan), accepted=False)
        # A depth-only subscription needs no image/disparity product or geometry.
        depth_only = copy.deepcopy(metadata)
        depth_only['products'] = depth_only['products'][1:]
        depth_only['products'][0]['offset'] = 0
        depth_only['products'][1]['offset'] = 8
        depth_only['selected_products'] = depth_only['available_products'] = 40
        depth_payload = struct.pack('<ffBB', 2., math.nan, 1, 6)
        check(depth_only, depth_payload)
        check(depth_only, struct.pack('<ffBB', 200., math.nan, 1, 6), accepted=False)
        for key in ('image_grid', 'matching_grid', 'calibration_geometry'):
            depth_only.pop(key)
        check(depth_only, depth_payload)
        # Legacy declarations are absent, not guessed; validity/NaN still apply.
        depth_only.pop('depth_metadata')
        depth_only.pop('disparity_metadata')
        check(depth_only, depth_payload)
        check(depth_only, struct.pack('<ffBB', 2., math.nan, 1, 12), accepted=False)
        # Raw/rectified image-only transport has no measurement prerequisites.
        image_only = copy.deepcopy(metadata)
        for key in ('depth_unit', 'depth_convention', 'depth_metadata', 'disparity_metadata',
                    'image_grid', 'matching_grid', 'calibration_geometry', 'stereo_configuration_error'):
            image_only.pop(key)
        image_only['products'] = [{'kind': 'left_rectified', 'format': 'gray8',
            'width': 2, 'height': 1, 'offset': 0, 'length': 2}]
        image_only['selected_products'] = image_only['available_products'] = 1
        check(image_only, b'\x01\x02')
        # Metadata duplicate keys and unclaimed payload cannot hide contradictions.
        wire = encode(metadata, payload)
        size = HEADER.unpack_from(wire)[5]
        duplicate = wire[32:32 + size].replace(b'"maximum_m":20.0', b'"maximum_m":20.0,"maximum_m":200.0')
        path.write_bytes(HEADER.pack(b'NUV1', 1, 0, 5, 0, len(duplicate), len(payload), 1) + duplicate + payload)
        assert subprocess.run([sys.argv[1], 'validate', str(path)], capture_output=True).returncode != 0
        check(samples=payload + b'\0', accepted=False)
        # The checked-in previous-release recording is provenance-pinned by the
        # repository. Its older absent declarations remain replay-compatible.
        recorded = Path(__file__).resolve().parents[3] / 'tests/fixtures/recordings/beta6-preview.nvr.gz'
        if recorded.is_file():
            recording = root / 'beta6-preview.nvr'
            recording.write_bytes(gzip.decompress(recorded.read_bytes()))
            assert subprocess.run([sys.argv[1], 'eof-replay', str(recording)],
                capture_output=True, text=True, timeout=10).stdout.strip() == '1'


if __name__ == '__main__':
    main()
