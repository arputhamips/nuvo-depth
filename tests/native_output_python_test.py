#!/usr/bin/env python3
"""Dependency-light regression test for the Python/native output boundary."""

from __future__ import annotations

import argparse
import tempfile
import time
from pathlib import Path

import numpy as np

from depthd_native_output import (
    NativeOutputPublisher,
    sensor_monotonic_to_realtime_ns,
)


def expect_raises(error_type, callable_) -> None:
    try:
        callable_()
    except error_type:
        return
    raise AssertionError(f"expected {error_type.__name__}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", type=Path, required=True)
    args = parser.parse_args()

    translated = sensor_monotonic_to_realtime_ns(time.monotonic_ns())
    assert abs(translated - time.time_ns()) < 10_000_000
    expect_raises(ValueError, lambda: sensor_monotonic_to_realtime_ns(0))

    path = Path(tempfile.gettempdir()) / "depthd-native-output-python-test.ring"
    path.unlink(missing_ok=True)
    disparity = np.full(
        (544, 728),
        493.542046188 * 0.161458780 / 2.0,
        dtype=np.float32,
    )
    mono = np.full((544, 728), 75, dtype=np.uint8)
    jpeg = np.array([0xFF, 0xD8, 1, 2, 0xFF, 0xD9], dtype=np.uint8)
    with NativeOutputPublisher(path, args.library) as publisher:
        stats = publisher.publish(
            capture_realtime_ns=time.time_ns(),
            disparity=disparity,
            left_rectified_mono=mono,
            left_rectified_jpeg=jpeg,
        )
        assert stats["sequence"] == 1
        assert stats["obstacle_point_count"] > 0
        assert stats["voxel_capacity_overflow"] == 0
        expect_raises(
            TypeError,
            lambda: publisher.publish(
                capture_realtime_ns=time.time_ns(),
                disparity=disparity.astype(np.float64),
            ),
        )
        expect_raises(
            ValueError,
            lambda: publisher.publish(
                capture_realtime_ns=time.time_ns(),
                disparity=disparity[:, ::-1],
            ),
        )
    path.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
