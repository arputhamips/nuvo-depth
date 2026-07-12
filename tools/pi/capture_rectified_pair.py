#!/usr/bin/env python3
"""Capture one hardware-triggered IMX296 pair and rectify it without ROS.

This is a temporary hardware-validation tool. The production capture path will
be C++ and will keep the same camera, trigger, orientation, and calibration
contract.
"""

from __future__ import annotations

import argparse
import json
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

import cv2
import numpy as np
import yaml
from libcamera import Transform
from picamera2 import Picamera2


WIDTH = 728
HEIGHT = 544


class TriggerLine:
    def __init__(self, chip: str, offset: int) -> None:
        import gpiod

        self.gpiod = gpiod
        self.offset = offset
        self.request: Any = None
        self.chip: Any = None
        self.line: Any = None
        if hasattr(gpiod, "request_lines"):
            from gpiod.line import Direction, Value

            settings = gpiod.LineSettings(
                direction=Direction.OUTPUT, output_value=Value.ACTIVE
            )
            self.request = gpiod.request_lines(
                chip, consumer="depthd_validation_trigger", config={offset: settings}
            )
        else:
            self.chip = gpiod.Chip(chip)
            self.line = self.chip.get_line(offset)
            self.line.request(
                consumer="depthd_validation_trigger",
                type=gpiod.LINE_REQ_DIR_OUT,
                default_vals=[1],
            )

    def set(self, active: bool) -> None:
        value = 1 if active else 0
        if self.request is not None:
            from gpiod.line import Value

            self.request.set_value(
                self.offset, Value.ACTIVE if active else Value.INACTIVE
            )
        else:
            self.line.set_value(value)

    def close(self) -> None:
        # The IMX296 trigger input idles high. Cameras must be stopped before
        # releasing the line so a pull-down cannot create a long exposure.
        self.set(True)
        if self.request is not None:
            self.request.release()
        if self.line is not None:
            self.line.release()
        if self.chip is not None:
            self.chip.close()


def pulse_loop(
    line: TriggerLine,
    stop: threading.Event,
    frequency_hz: float = 10.0,
    low_pulse_s: float = 0.003,
) -> None:
    if frequency_hz <= 0.0:
        raise ValueError("trigger frequency must be positive")
    period_s = 1.0 / frequency_hz
    if low_pulse_s <= 0.0 or low_pulse_s >= period_s:
        raise ValueError("trigger low pulse must be inside one trigger period")

    # InnoMaker's isolated TRIG input idles high. The falling edge starts the
    # global exposure and its duration is the low pulse plus about 14.26 us.
    line.set(True)
    next_pulse = time.monotonic()
    while not stop.is_set():
        line.set(False)
        time.sleep(low_pulse_s)
        line.set(True)
        next_pulse += period_s
        stop.wait(max(0.0, next_pulse - time.monotonic()))


def calibration(path: Path) -> tuple[np.ndarray, ...]:
    data = yaml.safe_load(path.read_text())
    matrix = np.asarray(data["camera_matrix"]["data"], dtype=np.float64).reshape(3, 3)
    distortion = np.asarray(
        data["distortion_coefficients"]["data"], dtype=np.float64
    )
    rectification = np.asarray(
        data["rectification_matrix"]["data"], dtype=np.float64
    ).reshape(3, 3)
    projection = np.asarray(
        data["projection_matrix"]["data"], dtype=np.float64
    ).reshape(3, 4)
    return matrix, distortion, rectification, projection


def rectify(image: np.ndarray, path: Path) -> np.ndarray:
    matrix, distortion, rectification, projection = calibration(path)
    map_x, map_y = cv2.initUndistortRectifyMap(
        matrix,
        distortion,
        rectification,
        projection[:, :3],
        (WIDTH, HEIGHT),
        cv2.CV_16SC2,
    )
    return cv2.remap(image, map_x, map_y, cv2.INTER_LINEAR)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--left-calibration", type=Path, required=True)
    parser.add_argument("--right-calibration", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--left-camera-index", type=int, default=1)
    parser.add_argument("--right-camera-index", type=int, default=0)
    parser.add_argument("--gpiochip", default="/dev/gpiochip0")
    parser.add_argument("--line", type=int, default=17)
    parser.add_argument("--trigger-start-delay", type=float, default=0.0)
    parser.add_argument("--pair-candidates", type=int, default=8)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    controls = {
        "FrameDurationLimits": (100000, 100000),
        "ExposureTime": 3000,
        "AnalogueGain": 1.5,
    }
    transform = Transform(hflip=1, vflip=1)
    if args.left_camera_index == args.right_camera_index:
        parser.error("left and right camera indices must differ")
    cameras = [
        Picamera2(args.left_camera_index),
        Picamera2(args.right_camera_index),
    ]
    for camera in cameras:
        config = camera.create_video_configuration(
            main={"size": (WIDTH, HEIGHT), "format": "RGB888"},
            controls=controls,
            transform=transform,
            buffer_count=4,
        )
        camera.configure(config)

    trigger = TriggerLine(args.gpiochip, args.line)
    stop = threading.Event()
    thread = threading.Thread(target=pulse_loop, args=(trigger, stop), daemon=True)
    trigger_started = False
    try:
        for camera in cameras:
            camera.start()
        time.sleep(args.trigger_start_delay)
        thread.start()
        trigger_started = True
        time.sleep(0.5)

        def capture_candidates(camera: Picamera2) -> list[tuple[np.ndarray, dict[str, Any]]]:
            candidates = []
            for _ in range(args.pair_candidates):
                request = camera.capture_request(flush=True)
                try:
                    candidates.append(
                        (request.make_array("main").copy(), request.get_metadata())
                    )
                finally:
                    request.release()
            return candidates

        with ThreadPoolExecutor(max_workers=2) as executor:
            candidate_sets = list(executor.map(capture_candidates, cameras))

        paired = min(
            (
                (left, right)
                for left in candidate_sets[0]
                for right in candidate_sets[1]
            ),
            key=lambda pair: abs(
                int(pair[0][1].get("SensorTimestamp", 0))
                - int(pair[1][1].get("SensorTimestamp", 0))
            ),
        )
        images = [paired[0][0], paired[1][0]]
        metadata = [paired[0][1], paired[1][1]]
    finally:
        stop.set()
        if trigger_started:
            thread.join(timeout=1.0)
        for camera in cameras:
            camera.stop()
            camera.close()
        trigger.close()

    # Keep the oriented but unrectified frames for calibration diagnostics.
    # Picamera2/libcamera's RGB888 is already B, G, R byte order for OpenCV.
    cv2.imwrite(str(args.output_dir / "left_raw.png"), images[0])
    cv2.imwrite(str(args.output_dir / "right_raw.png"), images[1])
    left = rectify(images[0], args.left_calibration)
    right = rectify(images[1], args.right_calibration)
    # Converting RGB->BGR here would swap red and blue in the saved images.
    cv2.imwrite(str(args.output_dir / "left_rect.png"), left)
    cv2.imwrite(str(args.output_dir / "right_rect.png"), right)
    summary = {
        "left_camera_index": args.left_camera_index,
        "right_camera_index": args.right_camera_index,
        "pixel_memory_order": "BGR",
        "left_metadata": metadata[0],
        "right_metadata": metadata[1],
        "sensor_timestamp_delta_ns": abs(
            int(metadata[0].get("SensorTimestamp", 0))
            - int(metadata[1].get("SensorTimestamp", 0))
        ),
    }
    (args.output_dir / "capture.json").write_text(
        json.dumps(summary, indent=2, default=str) + "\n"
    )
    print(json.dumps(summary, indent=2, default=str))


if __name__ == "__main__":
    main()
