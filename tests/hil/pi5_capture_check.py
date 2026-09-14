#!/usr/bin/env python3
"""Run and validate the bounded native dual-camera capture check on a Pi 5.

This test validates libcamera ownership, negotiated streams, pair association,
restart epochs, and request-lease drainage. It deliberately does not turn a
software GPIO result into an electrical timing claim; attach independently
reviewed logic-analyzer evidence to the hardware qualification record.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
from typing import Any

import yaml


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--depthd", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--pairs", type=int, default=1000)
    parser.add_argument("--cycles", type=int, default=2)
    parser.add_argument("--pair-timeout-ms", type=int, default=3000)
    parser.add_argument(
        "--timeout-seconds",
        type=float,
        default=900.0,
        help="outer process timeout; independent of the per-pair timeout",
    )
    parser.add_argument(
        "--report",
        type=Path,
        help="optional path for the validated JSON capture report",
    )
    return parser.parse_args()


def validate_report(
    report: dict[str, Any], profile: dict[str, Any], pairs: int, cycles: int
) -> None:
    capture = profile["capture"]
    tolerance_ns = int(capture["hardware_trigger"]["pair_tolerance_us"]) * 1000
    require(report.get("schema_version") == 1, "unexpected report schema")
    require(report.get("status") == "pass", "native capture did not pass")
    require(
        report.get("requested_pairs_per_cycle") == pairs,
        "capture report has the wrong pair target",
    )
    cycle_reports = report.get("cycles", [])
    require(len(cycle_reports) == cycles, "capture report has the wrong cycle count")
    cameras = report.get("enumerated_cameras", [])
    require(len(cameras) >= 2, "libcamera enumerated fewer than two cameras")

    previous_epoch = 0
    expected_ids: tuple[str, str] | None = None
    for index, cycle in enumerate(cycle_reports):
        epoch = int(cycle["capture_epoch"])
        require(epoch > previous_epoch, "capture epoch did not advance on restart")
        previous_epoch = epoch
        require(cycle["trigger_active"] is True, "hardware trigger was not active")
        require(
            cycle["trigger_source"] == "userspace-gpio-smoke-check",
            "capture check did not report its trigger source honestly",
        )

        left = cycle["left_stream"]
        right = cycle["right_stream"]
        require(left["role"] == "left" and right["role"] == "right", "roles swapped")
        require(
            (left["width"], left["height"])
            == (int(capture["width"]), int(capture["height"])),
            "left negotiated geometry differs from the selected preset",
        )
        require(
            (right["width"], right["height"])
            == (int(capture["width"]), int(capture["height"])),
            "right negotiated geometry differs from the selected preset",
        )
        ids = (left["camera"]["stable_id"], right["camera"]["stable_id"])
        require(ids[0] and ids[1] and ids[0] != ids[1], "camera IDs are not distinct")
        if expected_ids is None:
            expected_ids = ids
        require(ids == expected_ids, "logical camera binding changed after restart")

        require(
            int(cycle["last_pair_sequence"])
            - int(cycle["first_pair_sequence"])
            >= pairs - 1,
            f"cycle {index} did not consume the requested number of ordered pairs",
        )
        require(
            int(cycle["maximum_consumed_delta_ns"]) <= tolerance_ns,
            f"cycle {index} exceeded the configured pair tolerance",
        )
        require(
            int(cycle["outstanding_leases_after_stop"]) == 0,
            f"cycle {index} leaked a camera request lease",
        )
        stats = cycle["pairer_stats"]
        require(int(stats["paired"]) >= pairs, "pairer produced too few pairs")
        for field in (
            "zero_timestamp_rejected",
            "wrong_clock_rejected",
            "identity_rejected",
            "epoch_rejected",
            "duplicate_sequence_rejected",
            "nonmonotonic_rejected",
            "stale_dropped",
            "capacity_dropped",
        ):
            require(int(stats[field]) == 0, f"unexpected {field} during clean capture")
        require(
            int(stats["maximum_sensor_delta_ns"]) <= tolerance_ns,
            "pairer exceeded the configured sensor timestamp tolerance",
        )
        require(
            int(cycle["callback_or_trigger_errors"]) == 0,
            "capture callback or trigger errors were recorded",
        )
        require(
            int(cycle["request_requeue_failures"]) == 0,
            "a completed camera request failed to requeue",
        )


def main() -> int:
    args = parse_args()
    if args.pairs <= 0 or args.cycles < 2 or args.pair_timeout_ms <= 0:
        raise RuntimeError("pairs must be positive and cycles must be at least two")
    profile = yaml.safe_load(args.config.read_text(encoding="utf-8"))
    command = [
        str(args.depthd),
        "--capture-check",
        str(args.config),
        "--pairs",
        str(args.pairs),
        "--cycles",
        str(args.cycles),
        "--pair-timeout-ms",
        str(args.pair_timeout_ms),
    ]
    completed = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        timeout=args.timeout_seconds,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"native capture check exited {completed.returncode}: "
            f"{completed.stderr.strip()}"
        )
    report = json.loads(completed.stdout)
    validate_report(report, profile, args.pairs, args.cycles)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    print(
        json.dumps(
            {
                "status": "pass",
                "pairs_per_cycle": args.pairs,
                "cycles": args.cycles,
                "electrical_trigger_verification": "not_performed_by_this_test",
                "next_gate": "attach logic-analyzer pulse and inter-camera timing evidence",
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001 - HIL runner needs one clear failure path.
        print(f"pi5_capture_check: {error}", file=sys.stderr)
        raise SystemExit(1)
