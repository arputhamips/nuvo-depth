#!/usr/bin/env python3
"""Deterministic latest-only tests without HailoRT or camera hardware."""

from __future__ import annotations

import threading
import sys
import tempfile
import types
from pathlib import Path
from typing import Any

import numpy as np

from depthd_semantic import (
    BackendRegistration,
    CLASS_HUMAN,
    CLASS_VEGETATION,
    Observation,
    HailoSemanticOwner,
    HailoSegformerBackend,
    HailoYoloBackend,
    SemanticWorker,
    SOURCE_DENSE_REGION,
    SOURCE_INSTANCE,
)


class _Context:
    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False


class FakeHailoModule(types.ModuleType):
    def __init__(self) -> None:
        super().__init__("hailo_platform")
        module = self
        self.vdevices = 0
        self.pipelines = 0
        self.pipeline_exits = 0
        self.infer_inputs: list[np.ndarray] = []
        self.HailoStreamInterface = types.SimpleNamespace(PCIe="pcie")
        self.FormatType = types.SimpleNamespace(UINT8="uint8", FLOAT32="float32")

        class HEF:
            def __init__(self, path: str) -> None:
                self.path = path

            def get_input_vstream_infos(self):
                shape = (512, 1024, 3) if "segformer" in self.path else (640, 640, 3)
                return [types.SimpleNamespace(name="input", shape=shape)]

            def get_output_vstream_infos(self):
                return [types.SimpleNamespace(name="output")]

        class VDevice:
            def __init__(self) -> None:
                module.vdevices += 1

            def configure(self, hef, _params):
                return [Group(hef)]

            def release(self) -> None:
                return None

        class Group:
            def __init__(self, hef) -> None:
                self.hef = hef

            def create_params(self):
                return object()

            def activate(self, _params):
                return _Context()

        class ConfigureParams:
            @staticmethod
            def create_from_hef(**_kwargs):
                return object()

        class Params:
            @staticmethod
            def make_from_network_group(*_args, **_kwargs):
                return object()

        class InferVStreams:
            def __init__(self, *_args) -> None:
                module.pipelines += 1

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                module.pipeline_exits += 1

            def infer(self, values):
                module.infer_inputs.append(next(iter(values.values())))
                return {"output": np.zeros((1,), dtype=np.float32)}

        self.HEF = HEF
        self.VDevice = VDevice
        self.ConfigureParams = ConfigureParams
        self.InputVStreamParams = Params
        self.OutputVStreamParams = Params
        self.InferVStreams = InferVStreams


class FakeOutput:
    def __init__(self) -> None:
        self.calls: list[dict[str, Any]] = []
        self.closed = False

    def publish(self, **values: Any) -> dict[str, int]:
        self.calls.append(values)
        return {
            "semantic_sequence": len(self.calls),
            "object_count": len(values["observations"]),
            "hazard_active": 0,
            "hazard_fail_closed_required": 0,
        }

    def close(self) -> None:
        self.closed = True


class FakeBackend:
    def __init__(
        self,
        name: str,
        source_kind: int,
        semantic_class: int,
        *,
        gate: threading.Event | None = None,
        entered: threading.Event | None = None,
    ) -> None:
        self.name = name
        self.source_kind = source_kind
        self.semantic_class = semantic_class
        self.gate = gate
        self.entered = entered
        self.calls = 0
        self.image_ids: list[int] = []
        self.closed = False

    def infer(self, image_bgr: np.ndarray) -> list[Observation]:
        self.calls += 1
        self.image_ids.append(id(image_bgr))
        if self.entered is not None:
            self.entered.set()
        if self.gate is not None:
            assert self.gate.wait(2.0)
        return [
            Observation(
                self.source_kind,
                self.semantic_class,
                self.calls,
                0,
                self.name,
                self.name,
                0.9,
                (200.0, 150.0, 500.0, 500.0),
                1000,
            )
        ]

    def close(self) -> None:
        self.closed = True


def arrays() -> tuple[np.ndarray, np.ndarray]:
    return (
        np.zeros((544, 728, 3), dtype=np.uint8),
        np.full((544, 728), 30.0, dtype=np.float32),
    )


def submit(
    worker: SemanticWorker,
    sequence: int,
    schedule_ns: int,
    color: np.ndarray,
    disparity: np.ndarray,
) -> None:
    assert worker.submit(
        source_sequence=sequence,
        capture_realtime_ns=1_780_000_000_000_000_000 + sequence,
        color_bgr=color,
        disparity=disparity,
        submitted_ns=schedule_ns,
    )


def main() -> None:
    instance = FakeBackend("instances", SOURCE_INSTANCE, CLASS_HUMAN)
    dense = FakeBackend("regions", SOURCE_DENSE_REGION, CLASS_VEGETATION)
    output = FakeOutput()
    worker = SemanticWorker(
        [BackendRegistration(instance, 5.0), BackendRegistration(dense, 2.0)],
        output,
    )
    color1, disparity1 = arrays()
    submit(worker, 1, 1_000_000_000, color1, disparity1)
    assert worker.wait_for_published(1, 1.0)
    assert instance.image_ids == [id(color1)]
    assert dense.image_ids == [id(color1)]
    assert id(output.calls[0]["disparity"]) == id(disparity1)
    assert output.calls[0]["capture_realtime_ns"] == 1_780_000_000_000_000_001

    color2, disparity2 = arrays()
    submit(worker, 2, 1_250_000_000, color2, disparity2)
    assert worker.wait_for_published(2, 1.0)
    assert len(output.calls[1]["model_runs"]) == 1
    assert len(output.calls[1]["observations"]) == 1
    color3, disparity3 = arrays()
    submit(worker, 3, 1_550_000_000, color3, disparity3)
    assert worker.wait_for_published(3, 1.0)
    assert instance.calls == 3
    assert dense.calls == 2
    worker.close()

    class FakeYoloOwner:
        def infer(self, _name: str, _image: np.ndarray):
            groups: list[np.ndarray] = [
                np.empty((0, 5), dtype=np.float32) for _ in range(80)
            ]
            groups[0] = np.asarray(
                [
                    [0.1, 0.2, 0.5, 0.7, 0.9],
                    [10.0, 20.0, 50.0, 70.0, 0.8],
                ],
                dtype=np.float32,
            )
            return {"nms": [groups]}

        def close(self) -> None:
            return None

    parser_backend = HailoYoloBackend(FakeYoloOwner())
    parser_image, _ = arrays()
    parsed = parser_backend.infer(parser_image)
    assert len(parsed) == 2
    assert np.allclose(parsed[0].box, (145.6, 54.4, 509.6, 272.0))
    assert parsed[1].box == (20.0, 10.0, 70.0, 50.0)

    class FakeSegformerOwner:
        def infer(self, _name: str, _image: np.ndarray) -> dict[str, np.ndarray]:
            mask = np.zeros((512, 1024), dtype=np.uint8)
            mask[0:20, 0:20] = 11  # person: owned by the instance detector
            mask[30:50, 0:20] = 12  # rider: owned by the instance detector
            mask[60:80, 0:20] = 8  # vegetation: dense "stuff"
            return {"mask": mask}

        def close(self) -> None:
            return None

    segformer_backend = HailoSegformerBackend(FakeSegformerOwner())
    dense_regions = segformer_backend.infer(parser_image)
    assert [item.model_label for item in dense_regions] == ["vegetation"]
    assert all(item.semantic_class != CLASS_HUMAN for item in dense_regions)

    fake_hailo = FakeHailoModule()
    previous_hailo = sys.modules.get("hailo_platform")
    sys.modules["hailo_platform"] = fake_hailo
    try:
        with tempfile.TemporaryDirectory() as directory:
            yolo = Path(directory) / "yolov8s.hef"
            segformer = Path(directory) / "segformer_b0_bn.hef"
            yolo.write_bytes(b"test")
            segformer.write_bytes(b"test")
            owner = HailoSemanticOwner(
                {"yolov8s": yolo, "segformer_b0_bn": segformer}
            )
            color = np.empty((544, 728, 3), dtype=np.uint8)
            color[:] = (11, 22, 33)
            owner.infer("yolov8s", color)
            owner.infer("yolov8s", color)
            owner.infer("segformer_b0_bn", color)
            owner.close()
            owner.close()
        assert fake_hailo.vdevices == 1
        assert fake_hailo.pipelines == 2
        assert len(fake_hailo.infer_inputs) == 3
        assert fake_hailo.pipeline_exits == 2
        # RGB conversion happens only after resize into each model buffer.
        assert tuple(fake_hailo.infer_inputs[0][0, 0, 0]) == (33, 22, 11)
    finally:
        if previous_hailo is None:
            sys.modules.pop("hailo_platform", None)
        else:
            sys.modules["hailo_platform"] = previous_hailo
    assert instance.closed and dense.closed and output.closed

    gate = threading.Event()
    entered = threading.Event()
    blocked = FakeBackend(
        "blocked",
        SOURCE_INSTANCE,
        CLASS_HUMAN,
        gate=gate,
        entered=entered,
    )
    output = FakeOutput()
    worker = SemanticWorker([BackendRegistration(blocked, 100.0)], output)
    first_color, first_disparity = arrays()
    submit(worker, 10, 2_000_000_000, first_color, first_disparity)
    assert entered.wait(1.0)
    for sequence in (11, 12, 13):
        color, disparity = arrays()
        submit(
            worker,
            sequence,
            2_000_000_000 + sequence * 20_000_000,
            color,
            disparity,
        )
    gate.set()
    assert worker.wait_for_published(2, 2.0)
    state = worker.stats()
    assert state["replaced_pending_frames"] >= 2
    assert output.calls[-1]["source_sequence"] == 13
    worker.close()


if __name__ == "__main__":
    main()
