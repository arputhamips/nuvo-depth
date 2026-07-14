#!/usr/bin/env python3
"""Regression tests for live capture deadline accounting."""

from __future__ import annotations

import ast
from pathlib import Path


SOURCE = (
    Path(__file__).parents[1]
    / "tools"
    / "pi"
    / "profile_async_zero_copy_pipeline.py"
)


def _function():
    tree = ast.parse(SOURCE.read_text(encoding="utf-8"))
    definition = next(
        node
        for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name == "count_deadline_misses"
    )
    module = ast.Module(body=[definition], type_ignores=[])
    namespace: dict[str, object] = {}
    exec(compile(module, str(SOURCE), "exec"), namespace)
    return namespace["count_deadline_misses"]


def test_submillisecond_pacing_jitter_is_not_a_missed_frame() -> None:
    count = _function()
    timestamps = [0, 100_100_000, 199_900_000, 300_050_000]

    assert count(timestamps, fps=10.0, ready_pair_replaced=0) == 0


def test_skipped_period_or_ready_pair_replacement_is_a_miss() -> None:
    count = _function()

    assert (
        count(
            [0, 100_000_000, 300_000_000],
            fps=10.0,
            ready_pair_replaced=0,
        )
        == 1
    )
    assert count([0, 100_000_000], fps=10.0, ready_pair_replaced=2) == 2


if __name__ == "__main__":
    test_submillisecond_pacing_jitter_is_not_a_missed_frame()
    test_skipped_period_or_ready_pair_replacement_is_a_miss()
