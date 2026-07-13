#!/usr/bin/env python3
"""Small pair-level auto-exposure controller for triggered stereo cameras."""

from __future__ import annotations

from collections import deque
from dataclasses import asdict, dataclass
from typing import Any

import numpy as np


ABSOLUTE_MAX_SHUTTER_US = 8_000


@dataclass(frozen=True)
class AutoExposureConfig:
    target_median: float = 75.0
    median_low: float = 65.0
    median_high: float = 85.0
    dark_threshold: int = 16
    clipping_threshold: int = 245
    max_clipped_fraction: float = 0.005
    min_shutter_us: int = 250
    max_shutter_us: int = ABSOLUTE_MAX_SHUTTER_US
    min_gain: float = 1.0
    max_gain: float = 3.0
    update_interval_frames: int = 2
    settle_frames_after_update: int = 4
    response_exponent: float = 0.5
    max_increase_factor: float = 1.5
    min_decrease_factor: float = 0.25
    roi_x_start: int = 80
    roi_y_start: int = 48
    roi_y_end: int = 210
    sample_stride: int = 4
    shutter_quantum_us: int = 50
    gain_quantum: float = 0.05

    def validate(self) -> None:
        if not (0.0 < self.median_low <= self.target_median <= self.median_high):
            raise ValueError("invalid auto-exposure median target/deadband")
        if not (0 <= self.dark_threshold < self.clipping_threshold <= 255):
            raise ValueError("invalid dark or clipping threshold")
        if not (0.0 <= self.max_clipped_fraction < 1.0):
            raise ValueError("invalid clipped-pixel fraction")
        if not (
            0 < self.min_shutter_us <= self.max_shutter_us
            <= ABSOLUTE_MAX_SHUTTER_US
        ):
            raise ValueError("shutter range must stay at or below 8 ms")
        if not (1.0 <= self.min_gain <= self.max_gain <= 3.0):
            raise ValueError("gain range must stay between 1x and 3x")
        if (
            self.update_interval_frames < 1
            or self.settle_frames_after_update < 0
            or self.sample_stride < 1
        ):
            raise ValueError("invalid sampling cadence")
        if not (0.0 < self.response_exponent <= 1.0):
            raise ValueError("invalid controller response exponent")
        if self.max_increase_factor < 1.0:
            raise ValueError("maximum increase factor must be at least one")
        if not (0.0 < self.min_decrease_factor <= 1.0):
            raise ValueError("invalid minimum decrease factor")
        if self.shutter_quantum_us < 1 or self.gain_quantum <= 0.0:
            raise ValueError("invalid exposure quantization")


@dataclass(frozen=True)
class ExposureCommand:
    shutter_us: int
    gain: float
    reason: str
    median: float
    dark_fraction: float
    clipped_fraction: float


class StereoAutoExposure:
    """Control one shared shutter and matched gains from stereo-pair statistics."""

    def __init__(
        self,
        config: AutoExposureConfig,
        *,
        initial_shutter_us: int,
        initial_gain: float,
    ) -> None:
        config.validate()
        if not config.min_shutter_us <= initial_shutter_us <= config.max_shutter_us:
            raise ValueError("initial shutter is outside the configured range")
        if not config.min_gain <= initial_gain <= config.max_gain:
            raise ValueError("initial gain is outside the configured range")
        self.config = config
        self.shutter_us = self._quantize_shutter(initial_shutter_us)
        self.gain = self._quantize_gain(initial_gain)
        self.frame_count = 0
        self.observation_count = 0
        self.update_count = 0
        self._settle_frames_remaining = 0
        self.last_observation: dict[str, float] | None = None
        self.updates: deque[dict[str, Any]] = deque(maxlen=64)

    def _quantize_shutter(self, value: float) -> int:
        quantum = self.config.shutter_quantum_us
        quantized = round(value / quantum) * quantum
        return int(
            min(
                self.config.max_shutter_us,
                max(self.config.min_shutter_us, quantized),
            )
        )

    def _quantize_gain(self, value: float) -> float:
        quantum = self.config.gain_quantum
        quantized = round(value / quantum) * quantum
        bounded = min(
            self.config.max_gain,
            max(self.config.min_gain, quantized),
        )
        return round(bounded, 6)

    def _sample(self, image: np.ndarray) -> np.ndarray:
        if image.ndim != 2 or image.dtype != np.uint8:
            raise ValueError("auto exposure expects uint8 grayscale images")
        if (
            self.config.roi_x_start >= image.shape[1]
            or self.config.roi_y_start >= self.config.roi_y_end
            or self.config.roi_y_end > image.shape[0]
        ):
            raise ValueError("auto-exposure ROI is outside the image")
        return image[
            self.config.roi_y_start : self.config.roi_y_end : self.config.sample_stride,
            self.config.roi_x_start :: self.config.sample_stride,
        ].reshape(-1)

    def _split_exposure_value(self, exposure_value: float) -> tuple[int, float]:
        minimum_value = self.config.min_shutter_us * self.config.min_gain
        maximum_value = self.config.max_shutter_us * self.config.max_gain
        exposure_value = min(maximum_value, max(minimum_value, exposure_value))
        if exposure_value <= self.config.max_shutter_us * self.config.min_gain:
            gain = self.config.min_gain
            shutter_us = exposure_value / gain
        else:
            shutter_us = float(self.config.max_shutter_us)
            gain = exposure_value / shutter_us
        return self._quantize_shutter(shutter_us), self._quantize_gain(gain)

    def observe(
        self, left_gray: np.ndarray, right_gray: np.ndarray
    ) -> ExposureCommand | None:
        if left_gray.shape != right_gray.shape:
            raise ValueError("left and right exposure images must have equal geometry")
        self.frame_count += 1
        if self._settle_frames_remaining > 0:
            self._settle_frames_remaining -= 1
            return None
        if self.frame_count % self.config.update_interval_frames != 0:
            return None

        left = self._sample(left_gray)
        right = self._sample(right_gray)
        pixels = np.concatenate((left, right))
        median = float(np.median(pixels))
        dark_fraction = float(np.mean(pixels < self.config.dark_threshold))
        clipped_fraction = max(
            float(np.mean(left > self.config.clipping_threshold)),
            float(np.mean(right > self.config.clipping_threshold)),
        )
        self.observation_count += 1
        self.last_observation = {
            "median": median,
            "dark_fraction": dark_fraction,
            "clipped_fraction": clipped_fraction,
        }

        if clipped_fraction > self.config.max_clipped_fraction:
            ratio = self.config.target_median / max(median, 1.0)
            factor = min(0.5, ratio)
            factor = max(self.config.min_decrease_factor, factor)
            reason = "clipping"
        elif self.config.median_low <= median <= self.config.median_high:
            return None
        else:
            ratio = self.config.target_median / max(median, 1.0)
            factor = ratio**self.config.response_exponent
            factor = min(self.config.max_increase_factor, max(0.5, factor))
            reason = "dark" if median < self.config.median_low else "bright"

        current_value = self.shutter_us * self.gain
        shutter_us, gain = self._split_exposure_value(current_value * factor)
        new_value = shutter_us * gain
        if abs(new_value - current_value) / current_value < 0.02:
            return None

        self.shutter_us = shutter_us
        self.gain = gain
        self.update_count += 1
        self._settle_frames_remaining = self.config.settle_frames_after_update
        command = ExposureCommand(
            shutter_us=shutter_us,
            gain=gain,
            reason=reason,
            median=median,
            dark_fraction=dark_fraction,
            clipped_fraction=clipped_fraction,
        )
        self.updates.append(asdict(command))
        return command

    def report(self) -> dict[str, Any]:
        return {
            "configuration": asdict(self.config),
            "current_shutter_us": self.shutter_us,
            "current_gain": self.gain,
            "frames_seen": self.frame_count,
            "observation_count": self.observation_count,
            "update_count": self.update_count,
            "last_observation": self.last_observation,
            "updates": list(self.updates),
        }
