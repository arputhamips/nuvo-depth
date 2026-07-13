#!/usr/bin/env python3
"""Validate live Depth Daemon products after conversion into ROS messages."""

from __future__ import annotations

import argparse
import json
import math
import struct
import sys
import time
from collections import defaultdict
from typing import Any

import rclpy
from diagnostic_msgs.msg import DiagnosticArray
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, HistoryPolicy, QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import CameraInfo, CompressedImage, Image, PointCloud2
from stereo_msgs.msg import DisparityImage


TOPICS = {
    "disparity": "/camera/front/disparity",
    "left_info": "/camera/front/left/camera_info",
    "right_info": "/camera/front/right/camera_info",
    "jpeg": "/camera/front/left/image_rect/compressed",
    "mono": "/camera/front/left/image_rect",
    "cloud": "/camera/front/obstacles",
    "diagnostics": "/camera/front/depthd/diagnostics",
}


def qos(reliable: bool) -> QoSProfile:
    return QoSProfile(
        history=HistoryPolicy.KEEP_LAST,
        depth=1,
        reliability=(
            ReliabilityPolicy.RELIABLE
            if reliable
            else ReliabilityPolicy.BEST_EFFORT
        ),
        durability=DurabilityPolicy.VOLATILE,
    )


def stamp_ns(message: Any) -> int:
    stamp = message.header.stamp
    return int(stamp.sec) * 1_000_000_000 + int(stamp.nanosec)


def uint8_value(value: Any) -> int:
    if isinstance(value, (bytes, bytearray)):
        if len(value) != 1:
            raise ValueError(f"expected one byte, got {len(value)}")
        return value[0]
    return int(value)


class BridgeValidator(Node):
    def __init__(self) -> None:
        super().__init__("depthd_ros_bridge_validator")
        self.counts: dict[str, int] = defaultdict(int)
        self.callback_times: dict[str, list[float]] = defaultdict(list)
        self.stamps: dict[str, list[int]] = defaultdict(list)
        self.latest: dict[str, Any] = {}
        self.disparity_nan_fraction: float | None = None
        self.max_cloud_points = 0
        self.cloud_sample_finite = True
        self.healthy_diagnostics = 0
        self.max_torn_reads = 0
        self.max_schema_mismatches = 0
        self.max_dropped_sequences = 0
        self.max_overwritten_sequences = 0
        self.diagnostic_states: dict[str, int] = defaultdict(int)
        self.source_age_min_s: float | None = None
        self.source_age_max_s: float | None = None
        self.latest_diagnostic_values: dict[str, str] = {}

        self.create_subscription(
            DisparityImage, TOPICS["disparity"], self.on_disparity, qos(True)
        )
        self.create_subscription(
            CameraInfo, TOPICS["left_info"],
            lambda msg: self.record("left_info", msg), qos(True)
        )
        self.create_subscription(
            CameraInfo, TOPICS["right_info"],
            lambda msg: self.record("right_info", msg), qos(True)
        )
        self.create_subscription(
            CompressedImage, TOPICS["jpeg"], self.on_jpeg, qos(False)
        )
        self.create_subscription(
            Image, TOPICS["mono"], lambda msg: self.record("mono", msg), qos(False)
        )
        self.create_subscription(
            PointCloud2, TOPICS["cloud"], self.on_cloud, qos(False)
        )
        self.create_subscription(
            DiagnosticArray, TOPICS["diagnostics"], self.on_diagnostics, qos(True)
        )

    def record(self, name: str, message: Any) -> None:
        self.counts[name] += 1
        self.callback_times[name].append(time.monotonic())
        self.latest[name] = message
        if hasattr(message, "header"):
            self.stamps[name].append(stamp_ns(message))

    def on_disparity(self, message: DisparityImage) -> None:
        self.record("disparity", message)
        if self.disparity_nan_fraction is None:
            values = struct.iter_unpack("<f", bytes(message.image.data))
            total = message.image.width * message.image.height
            invalid = sum(1 for (value,) in values if math.isnan(value))
            self.disparity_nan_fraction = invalid / total if total else 1.0

    def on_jpeg(self, message: CompressedImage) -> None:
        self.record("jpeg", message)

    def on_cloud(self, message: PointCloud2) -> None:
        self.record("cloud", message)
        self.max_cloud_points = max(self.max_cloud_points, int(message.width))
        payload = bytes(message.data)
        for offset in range(0, min(len(payload), 1200), max(12, message.point_step)):
            if offset + 12 > len(payload):
                break
            self.cloud_sample_finite &= all(
                math.isfinite(value) for value in struct.unpack_from("<fff", payload, offset)
            )

    def on_diagnostics(self, message: DiagnosticArray) -> None:
        self.record("diagnostics", message)
        for status in message.status:
            if status.name != "depthd_ros_bridge":
                continue
            values = {item.key: item.value for item in status.values}
            source_state = values.get("source_state", "missing")
            level = uint8_value(status.level)
            self.diagnostic_states[f"{level}:{source_state}"] += 1
            self.latest_diagnostic_values = values
            source_age = float(values.get("source_age_s", "nan"))
            if math.isfinite(source_age):
                self.source_age_min_s = (
                    source_age
                    if self.source_age_min_s is None
                    else min(self.source_age_min_s, source_age)
                )
                self.source_age_max_s = (
                    source_age
                    if self.source_age_max_s is None
                    else max(self.source_age_max_s, source_age)
                )
            if level == 0 and values.get("source_state", "").startswith("healthy_"):
                self.healthy_diagnostics += 1
            self.max_torn_reads = max(
                self.max_torn_reads, int(values.get("torn_reads", "0"))
            )
            self.max_schema_mismatches = max(
                self.max_schema_mismatches, int(values.get("schema_mismatch", "0"))
            )
            self.max_dropped_sequences = max(
                self.max_dropped_sequences, int(values.get("dropped_sequences", "0"))
            )
            self.max_overwritten_sequences = max(
                self.max_overwritten_sequences,
                int(values.get("overwritten_sequences", "0")),
            )


def message_rate_hz(stamps: list[int]) -> float:
    unique = sorted(set(stamps))
    if len(unique) < 2 or unique[-1] <= unique[0]:
        return 0.0
    return (len(unique) - 1) * 1_000_000_000.0 / (unique[-1] - unique[0])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration", type=float, default=20.0)
    parser.add_argument("--output", help="optional JSON report path")
    args = parser.parse_args()
    if args.duration < 5.0:
        parser.error("duration must be at least five seconds")

    rclpy.init()
    node = BridgeValidator()
    deadline = time.monotonic() + args.duration
    try:
        while time.monotonic() < deadline:
            rclpy.spin_once(node, timeout_sec=0.1)

        failures: list[str] = []

        def require(condition: bool, message: str) -> None:
            if not condition:
                failures.append(message)

        disparity = node.latest.get("disparity")
        left_info = node.latest.get("left_info")
        right_info = node.latest.get("right_info")
        jpeg = node.latest.get("jpeg")
        cloud = node.latest.get("cloud")

        expected_depth_frames = max(20, int(args.duration * 6.0))
        require(node.counts["disparity"] >= expected_depth_frames,
                "too few disparity frames")
        require(node.counts["cloud"] >= expected_depth_frames,
                "too few obstacle clouds")
        require(node.counts["left_info"] >= expected_depth_frames,
                "too few left CameraInfo messages")
        require(node.counts["right_info"] >= expected_depth_frames,
                "too few right CameraInfo messages")
        require(node.counts["jpeg"] >= max(5, int(args.duration)),
                "JPEG stream is below its expected throttled rate")
        require(node.counts["mono"] == 0, "mono stream published although disabled")
        require(node.healthy_diagnostics > 0, "no healthy bridge diagnostics received")
        require(node.max_torn_reads == 0, "ring reader reported a torn read")
        require(node.max_schema_mismatches == 0, "ring reader reported a schema mismatch")

        if disparity is not None:
            require(disparity.header.frame_id == "camera_front_left_optical",
                    "unexpected disparity frame")
            require(disparity.image.width == 728 and disparity.image.height == 544,
                    "disparity dimensions changed")
            require(disparity.image.encoding == "32FC1" and disparity.image.step == 2912,
                    "disparity image encoding/stride changed")
            require(len(disparity.image.data) == 728 * 544 * 4,
                    "disparity payload size changed")
            require(abs(disparity.f - 493.542) < 0.1, "unexpected stereo focal length")
            require(abs(disparity.t - 0.16146) < 0.001, "unexpected stereo baseline")
            require(disparity.min_disparity == 0.0 and disparity.max_disparity == 128.0,
                    "locked disparity bounds changed")
            require(abs(disparity.delta_d - 0.0625) < 1e-7,
                    "unexpected disparity quantization")
            window = disparity.valid_window
            require(
                (window.x_offset, window.y_offset, window.width, window.height)
                == (128, 0, 600, 544),
                "unexpected valid disparity window",
            )
            require(node.disparity_nan_fraction is not None and
                    0.0 < node.disparity_nan_fraction < 1.0,
                    "disparity validity mask was not preserved")

        if left_info is not None and right_info is not None and disparity is not None:
            require(left_info.header.frame_id == "camera_front_left_optical",
                    "unexpected left CameraInfo frame")
            require(right_info.header.frame_id == "camera_front_right_optical",
                    "unexpected right CameraInfo frame")
            require(left_info.width == 728 and left_info.height == 544,
                    "left CameraInfo dimensions changed")
            require(right_info.width == 728 and right_info.height == 544,
                    "right CameraInfo dimensions changed")
            require(abs(left_info.p[3]) < 1e-12, "logical-left P[3] is not zero")
            require(abs(right_info.p[3] + disparity.f * disparity.t) < 0.1,
                    "logical-right P[3] does not encode the stereo baseline")

        if jpeg is not None:
            payload = bytes(jpeg.data)
            require(jpeg.header.frame_id == "camera_front_left_optical",
                    "unexpected JPEG frame")
            require(jpeg.format == "jpeg", "unexpected compressed-image format")
            require(payload.startswith(b"\xff\xd8") and payload.endswith(b"\xff\xd9"),
                    "invalid JPEG markers")

        if cloud is not None:
            require(cloud.header.frame_id == "base_link", "cloud is not in base_link")
            require(cloud.height == 1 and cloud.point_step == 12,
                    "unexpected compact cloud layout")
            require([(f.name, f.offset, f.datatype, f.count) for f in cloud.fields]
                    == [("x", 0, 7, 1), ("y", 4, 7, 1), ("z", 8, 7, 1)],
                    "unexpected compact cloud fields")
            require(cloud.row_step == cloud.width * 12 and
                    len(cloud.data) == cloud.row_step,
                    "compact cloud payload size is inconsistent")
            require(node.max_cloud_points > 0, "live scene produced no obstacle points")
            require(node.cloud_sample_finite, "compact cloud contains non-finite points")

        synchronized = ("disparity", "left_info", "right_info", "cloud")
        if all(node.stamps[name] for name in synchronized):
            require(len({node.stamps[name][-1] for name in synchronized}) == 1,
                    "latest depth products do not share one capture timestamp")

        publishers = {
            name: node.count_publishers(topic) for name, topic in TOPICS.items()
        }
        for name in ("disparity", "left_info", "right_info", "jpeg", "cloud", "diagnostics"):
            require(publishers[name] == 1, f"{name} has {publishers[name]} publishers")
        require(node.count_publishers("/camera/front/points2") == 0,
                "legacy dense point cloud still has a publisher")
        require(node.count_publishers("/camera/front/obstacle_scan") == 0,
                "legacy obstacle scan still has a publisher")

        report = {
            "duration_s": args.duration,
            "counts": dict(node.counts),
            "rates_hz": {
                name: message_rate_hz(node.stamps[name])
                for name in ("disparity", "left_info", "right_info", "jpeg", "cloud")
            },
            "publishers": publishers,
            "disparity_nan_fraction": node.disparity_nan_fraction,
            "max_cloud_points": node.max_cloud_points,
            "healthy_diagnostics": node.healthy_diagnostics,
            "diagnostic_states": dict(node.diagnostic_states),
            "source_age_min_s": node.source_age_min_s,
            "source_age_max_s": node.source_age_max_s,
            "latest_diagnostic_values": node.latest_diagnostic_values,
            "max_torn_reads": node.max_torn_reads,
            "max_schema_mismatches": node.max_schema_mismatches,
            "max_dropped_sequences": node.max_dropped_sequences,
            "max_overwritten_sequences": node.max_overwritten_sequences,
            "failures": failures,
            "passed": not failures,
        }
        encoded = json.dumps(report, indent=2) + "\n"
        print(encoded, end="")
        if args.output:
            with open(args.output, "w", encoding="utf-8") as stream:
                stream.write(encoded)
        if failures:
            raise SystemExit(1)
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
