#!/usr/bin/env python3
"""Asynchronous, bounded, zero-copy Picamera2 stereo capture primitives."""

from __future__ import annotations

import threading
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator

import numpy as np
from libcamera import Transform
from picamera2 import MappedArray, Picamera2

from capture_rectified_pair import HEIGHT, WIDTH, TriggerLine, pulse_loop
from stereo_auto_exposure import ABSOLUTE_MAX_SHUTTER_US


class FrameLease:
    """Own one reference to a Picamera2 completed request."""

    def __init__(self, side: str, request: Any) -> None:
        request.acquire()
        try:
            self.side = side
            self.request = request
            self.metadata = request.get_metadata()
            self.callback_monotonic_ns = time.monotonic_ns()
            self.sensor_timestamp_ns = int(
                self.metadata.get("SensorTimestamp", 0)
            )
            self._closed = False
        except Exception:
            request.release()
            raise

    def close(self) -> None:
        if not self._closed:
            self._closed = True
            self.request.release()


class StereoPairLease:
    """Own left/right requests until the consumer releases the pair."""

    def __init__(self, left: FrameLease, right: FrameLease) -> None:
        self.left = left
        self.right = right
        self.ready_monotonic_ns = time.monotonic_ns()
        self._closed = False

    @property
    def sensor_delta_ns(self) -> int:
        return abs(
            self.left.sensor_timestamp_ns - self.right.sensor_timestamp_ns
        )

    @property
    def callback_delta_ns(self) -> int:
        return abs(
            self.left.callback_monotonic_ns - self.right.callback_monotonic_ns
        )

    @contextmanager
    def mapped_main(self) -> Iterator[tuple[np.ndarray, np.ndarray]]:
        """Map both camera-owned buffers without copying their pixels."""
        with MappedArray(
            self.left.request, "main", write=False
        ) as left_mapping, MappedArray(
            self.right.request, "main", write=False
        ) as right_mapping:
            if left_mapping.array is None or right_mapping.array is None:
                raise RuntimeError("camera buffer mapping returned no array")
            yield left_mapping.array, right_mapping.array

    def close(self) -> None:
        if not self._closed:
            self._closed = True
            self.left.close()
            self.right.close()


class LatestStereoPairSlot:
    """Pair callbacks by sensor time while retaining at most one ready pair."""

    def __init__(self, tolerance_ns: int) -> None:
        if tolerance_ns < 0:
            raise ValueError("pair tolerance cannot be negative")
        self._tolerance_ns = tolerance_ns
        self._condition = threading.Condition()
        self._pending: dict[str, FrameLease | None] = {
            "left": None,
            "right": None,
        }
        self._ready: StereoPairLease | None = None
        self._closed = False
        self._stats: dict[str, int] = {
            "left_callbacks": 0,
            "right_callbacks": 0,
            "paired": 0,
            "same_side_replaced": 0,
            "timestamp_rejected": 0,
            "ready_pair_replaced": 0,
        }

    def submit(self, frame: FrameLease) -> None:
        if frame.side not in self._pending:
            frame.close()
            raise ValueError(f"unknown stereo side: {frame.side}")

        release_frames: list[FrameLease] = []
        release_pairs: list[StereoPairLease] = []
        with self._condition:
            self._stats[f"{frame.side}_callbacks"] += 1
            if self._closed:
                release_frames.append(frame)
            else:
                previous = self._pending[frame.side]
                if previous is not None:
                    release_frames.append(previous)
                    self._stats["same_side_replaced"] += 1
                self._pending[frame.side] = frame

                left = self._pending["left"]
                right = self._pending["right"]
                if left is not None and right is not None:
                    delta_ns = abs(
                        left.sensor_timestamp_ns - right.sensor_timestamp_ns
                    )
                    if delta_ns <= self._tolerance_ns:
                        pair = StereoPairLease(left, right)
                        self._pending["left"] = None
                        self._pending["right"] = None
                        if self._ready is not None:
                            release_pairs.append(self._ready)
                            self._stats["ready_pair_replaced"] += 1
                        self._ready = pair
                        self._stats["paired"] += 1
                        self._condition.notify()
                    else:
                        older_side = (
                            "left"
                            if left.sensor_timestamp_ns < right.sensor_timestamp_ns
                            else "right"
                        )
                        older = self._pending[older_side]
                        self._pending[older_side] = None
                        if older is not None:
                            release_frames.append(older)
                        self._stats["timestamp_rejected"] += 1

        for stale in release_frames:
            stale.close()
        for stale_pair in release_pairs:
            stale_pair.close()

    def take(self, timeout_s: float | None = None) -> StereoPairLease | None:
        deadline = None if timeout_s is None else time.monotonic() + timeout_s
        with self._condition:
            while self._ready is None and not self._closed:
                remaining = (
                    None if deadline is None else deadline - time.monotonic()
                )
                if remaining is not None and remaining <= 0.0:
                    return None
                self._condition.wait(remaining)
            pair = self._ready
            self._ready = None
            return pair

    def stats(self) -> dict[str, int]:
        with self._condition:
            return dict(self._stats)

    def close(self) -> None:
        release_frames: list[FrameLease] = []
        release_pair: StereoPairLease | None = None
        with self._condition:
            if self._closed:
                return
            self._closed = True
            for side in ("left", "right"):
                frame = self._pending[side]
                self._pending[side] = None
                if frame is not None:
                    release_frames.append(frame)
            release_pair = self._ready
            self._ready = None
            self._condition.notify_all()
        for frame in release_frames:
            frame.close()
        if release_pair is not None:
            release_pair.close()


class AsyncStereoCapture:
    """Own two Picamera2 streams, the shared trigger, and the latest-pair slot."""

    def __init__(
        self,
        *,
        left_camera_index: int = 1,
        right_camera_index: int = 0,
        gpiochip: str = "/dev/gpiochip0",
        trigger_line: int = 17,
        frequency_hz: float = 10.0,
        trigger_low_ms: float = 8.0,
        analogue_gain: float = 2.0,
        colour_gains: tuple[float, float] | None = (2.4771, 2.3425),
        pair_tolerance_us: float = 500.0,
        buffer_count: int = 4,
    ) -> None:
        if left_camera_index == right_camera_index:
            raise ValueError("left and right camera indices must differ")
        if not 0.0 < trigger_low_ms <= ABSOLUTE_MAX_SHUTTER_US / 1000.0:
            raise ValueError("trigger exposure must stay at or below 8 ms")
        if not 1.0 <= analogue_gain <= 3.0:
            raise ValueError("analogue gain must stay between 1x and 3x")
        if colour_gains is not None and any(gain <= 0.0 for gain in colour_gains):
            raise ValueError("colour gains must be positive")
        self._camera_indices = (left_camera_index, right_camera_index)
        self._gpiochip = gpiochip
        self._trigger_line = trigger_line
        self._frequency_hz = frequency_hz
        self._trigger_low_s = trigger_low_ms / 1000.0
        self._analogue_gain = analogue_gain
        self._colour_gains = colour_gains
        self._control_lock = threading.Lock()
        self._exposure_updates = 0
        self._buffer_count = buffer_count
        self._slot = LatestStereoPairSlot(
            round(pair_tolerance_us * 1000.0)
        )
        self._trigger: TriggerLine | None = None
        self._trigger_stop = threading.Event()
        self._trigger_thread: threading.Thread | None = None
        self._cameras: list[Picamera2] = []
        self._started_cameras: list[Picamera2] = []
        self._running = False

    def _completed(self, side: str, request: Any) -> None:
        frame: FrameLease | None = None
        try:
            frame = FrameLease(side, request)
            self._slot.submit(frame)
        except Exception:
            # Picamera2 callbacks must never terminate its camera event loop.
            if frame is not None:
                frame.close()
            return

    def _current_trigger_low_s(self) -> float:
        with self._control_lock:
            return self._trigger_low_s

    def start(self) -> None:
        if self._running:
            raise RuntimeError("capture is already running")
        parameter = Path("/sys/module/imx296/parameters/trigger_mode")
        if not parameter.exists() or parameter.read_text().strip() != "1":
            raise RuntimeError("IMX296 driver external-trigger mode is not enabled")

        self._trigger = TriggerLine(self._gpiochip, self._trigger_line)
        self._trigger.set(True)
        try:
            self._cameras = [Picamera2(index) for index in self._camera_indices]
            transform = Transform(hflip=1, vflip=1)
            frame_duration_us = round(1_000_000.0 / self._frequency_hz)
            exposure_us = round(self._trigger_low_s * 1_000_000.0)
            for side, camera in zip(("left", "right"), self._cameras):
                controls: dict[str, Any] = {
                    "FrameDurationLimits": (
                        frame_duration_us,
                        frame_duration_us,
                    ),
                    "ExposureTime": exposure_us,
                    "AnalogueGain": self._analogue_gain,
                }
                if self._colour_gains is not None:
                    controls["ColourGains"] = self._colour_gains
                configuration = camera.create_video_configuration(
                    main={"size": (WIDTH, HEIGHT), "format": "RGB888"},
                    controls=controls,
                    transform=transform,
                    buffer_count=self._buffer_count,
                )
                camera.configure(configuration)
                camera.post_callback = (
                    lambda request, callback_side=side: self._completed(
                        callback_side, request
                    )
                )
            for camera in self._cameras:
                camera.start()
                self._started_cameras.append(camera)

            self._trigger_thread = threading.Thread(
                target=pulse_loop,
                args=(
                    self._trigger,
                    self._trigger_stop,
                    self._frequency_hz,
                    self._current_trigger_low_s,
                ),
                daemon=True,
            )
            self._trigger_thread.start()
            self._running = True
        except Exception:
            self.stop()
            raise

    def take(self, timeout_s: float | None = None) -> StereoPairLease | None:
        return self._slot.take(timeout_s)

    def set_exposure(self, shutter_us: int, analogue_gain: float) -> None:
        if not 0 < shutter_us <= ABSOLUTE_MAX_SHUTTER_US:
            raise ValueError("trigger exposure must stay at or below 8 ms")
        if not 1.0 <= analogue_gain <= 3.0:
            raise ValueError("analogue gain must stay between 1x and 3x")
        controls = {
            "ExposureTime": int(shutter_us),
            "AnalogueGain": float(analogue_gain),
        }
        for camera in self._cameras:
            camera.set_controls(controls)
        with self._control_lock:
            self._trigger_low_s = shutter_us / 1_000_000.0
            self._analogue_gain = analogue_gain
            self._exposure_updates += 1

    def exposure_state(self) -> dict[str, Any]:
        with self._control_lock:
            return {
                "requested_shutter_us": round(
                    self._trigger_low_s * 1_000_000.0
                ),
                "requested_analogue_gain": self._analogue_gain,
                "fixed_colour_gains": self._colour_gains,
                "update_count": self._exposure_updates,
            }

    def stats(self) -> dict[str, int]:
        return self._slot.stats()

    def stop(self) -> None:
        self._trigger_stop.set()
        if self._trigger_thread is not None:
            self._trigger_thread.join(timeout=1.0)
            self._trigger_thread = None
        for camera in reversed(self._started_cameras):
            camera.stop()
        self._started_cameras.clear()
        self._slot.close()
        for camera in self._cameras:
            camera.close()
        self._cameras.clear()
        if self._trigger is not None:
            self._trigger.close()
            self._trigger = None
        self._running = False

    def __enter__(self) -> "AsyncStereoCapture":
        self.start()
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        self.stop()
