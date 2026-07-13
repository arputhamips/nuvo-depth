#!/usr/bin/env python3
"""Latest-only native semantic worker and isolated HailoRT backends."""

from __future__ import annotations

import ctypes
import math
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Protocol

import cv2
import numpy as np

IMAGE_WIDTH = 728
IMAGE_HEIGHT = 544
SOURCE_INSTANCE = 1
SOURCE_DENSE_REGION = 2
CLASS_UNKNOWN = 0
CLASS_HUMAN = 1
CLASS_ANIMAL = 2
CLASS_VEGETATION = 3
CLASS_WALL = 4
CLASS_FENCE = 5
CLASS_TERRAIN = 6
CLASS_GENERIC_OBSTACLE = 7
HEALTH_STARTING = 0
HEALTH_OK = 1
HEALTH_DEGRADED = 2
HEALTH_STALE = 3
HEALTH_FAULT = 4


@dataclass(frozen=True, slots=True)
class Observation:
    source_kind: int
    semantic_class: int
    observation_id: int
    model_class_id: int
    model_label: str
    model_name: str
    confidence: float
    box: tuple[float, float, float, float]
    pixel_count: int = 0


@dataclass(frozen=True, slots=True)
class ModelRun:
    model_name: str
    model_version: str
    model_sha256: str
    health: int
    completed_runs: int
    errors: int
    latency_ms: float
    inference_latency_ms: float
    postprocess_latency_ms: float


class SemanticBackend(Protocol):
    name: str

    def infer(self, image_bgr: np.ndarray) -> list[Observation]: ...

    def close(self) -> None: ...


@dataclass(slots=True)
class BackendRegistration:
    backend: SemanticBackend
    cadence_hz: float = 5.0


class _OutputConfig(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("semantic_ring_path", ctypes.c_char_p),
        ("target_frame", ctypes.c_char_p),
        ("optical_to_target", ctypes.c_double * 16),
    ]


class _Observation(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("source_kind", ctypes.c_uint32),
        ("semantic_class", ctypes.c_uint32),
        ("observation_id", ctypes.c_uint32),
        ("model_class_id", ctypes.c_uint32),
        ("model_label", ctypes.c_char_p),
        ("model_name", ctypes.c_char_p),
        ("confidence", ctypes.c_float),
        ("x_min", ctypes.c_float),
        ("y_min", ctypes.c_float),
        ("x_max", ctypes.c_float),
        ("y_max", ctypes.c_float),
        ("pixel_count", ctypes.c_uint32),
    ]


class _ModelRun(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("model_name", ctypes.c_char_p),
        ("model_version", ctypes.c_char_p),
        ("model_sha256", ctypes.c_char_p),
        ("health", ctypes.c_uint32),
        ("completed_runs", ctypes.c_uint64),
        ("errors", ctypes.c_uint64),
        ("latency_ms", ctypes.c_double),
        ("inference_latency_ms", ctypes.c_double),
        ("postprocess_latency_ms", ctypes.c_double),
    ]


class _OutputFrame(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("source_sequence", ctypes.c_uint64),
        ("capture_realtime_ns", ctypes.c_int64),
        ("ready_realtime_ns", ctypes.c_int64),
        ("health", ctypes.c_uint32),
        ("disparity", ctypes.POINTER(ctypes.c_float)),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("disparity_stride_elements", ctypes.c_uint32),
        ("valid_x_offset", ctypes.c_uint32),
        ("valid_y_offset", ctypes.c_uint32),
        ("valid_width", ctypes.c_uint32),
        ("valid_height", ctypes.c_uint32),
        ("min_disparity", ctypes.c_float),
        ("max_disparity", ctypes.c_float),
        ("observations", ctypes.POINTER(_Observation)),
        ("observation_count", ctypes.c_uint32),
        ("model_runs", ctypes.POINTER(_ModelRun)),
        ("model_run_count", ctypes.c_uint32),
        ("preview_jpeg", ctypes.POINTER(ctypes.c_uint8)),
        ("preview_jpeg_bytes", ctypes.c_uint32),
        ("submitted_frames", ctypes.c_uint64),
        ("replaced_pending_frames", ctypes.c_uint64),
        ("processed_frames", ctypes.c_uint64),
        ("published_results", ctypes.c_uint64),
        ("worker_errors", ctypes.c_uint64),
        ("frame_latency_ms", ctypes.c_double),
        ("max_frame_latency_ms", ctypes.c_double),
    ]


class _OutputStats(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("semantic_sequence", ctypes.c_uint64),
        ("object_count", ctypes.c_uint32),
        ("hazard_active", ctypes.c_uint32),
        ("hazard_fail_closed_required", ctypes.c_uint32),
    ]


def _require_array(
    value: np.ndarray,
    *,
    dtype: np.dtype,
    shape: tuple[int, ...],
    name: str,
) -> np.ndarray:
    if not isinstance(value, np.ndarray):
        raise TypeError(f"{name} must be a numpy array")
    if value.dtype != dtype:
        raise TypeError(f"{name} must have dtype {dtype}")
    if value.shape != shape:
        raise ValueError(f"{name} must have shape {shape}")
    if not value.flags.c_contiguous:
        raise ValueError(f"{name} must be C-contiguous")
    return value


class SemanticOutputPublisher:
    """ctypes seam into the authoritative C++ fusion and semantic ring."""

    def __init__(
        self,
        ring_path: Path,
        library_path: Path | None = None,
        target_frame: str = "base_link",
    ) -> None:
        self._handle = None
        if library_path is None:
            library_path = (
                Path(__file__).resolve().parents[2]
                / "build"
                / "libdepthd_semantic_output.so"
            )
        self._library = ctypes.CDLL(str(library_path))
        self._library.depthd_semantic_output_default_config.argtypes = [
            ctypes.POINTER(_OutputConfig)
        ]
        self._library.depthd_semantic_output_default_config.restype = None
        self._library.depthd_semantic_output_create.argtypes = [
            ctypes.POINTER(_OutputConfig), ctypes.c_char_p, ctypes.c_size_t
        ]
        self._library.depthd_semantic_output_create.restype = ctypes.c_void_p
        self._library.depthd_semantic_output_publish.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_OutputFrame),
            ctypes.POINTER(_OutputStats),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        self._library.depthd_semantic_output_publish.restype = ctypes.c_int
        self._library.depthd_semantic_output_destroy.argtypes = [ctypes.c_void_p]
        self._library.depthd_semantic_output_destroy.restype = None
        config = _OutputConfig()
        self._library.depthd_semantic_output_default_config(ctypes.byref(config))
        self._ring_path = str(ring_path).encode()
        self._target_frame = target_frame.encode()
        config.semantic_ring_path = self._ring_path
        config.target_frame = self._target_frame
        error = ctypes.create_string_buffer(512)
        self._handle = self._library.depthd_semantic_output_create(
            ctypes.byref(config), error, len(error)
        )
        if not self._handle:
            raise RuntimeError(error.value.decode(errors="replace"))

    def publish(
        self,
        *,
        source_sequence: int,
        capture_realtime_ns: int,
        disparity: np.ndarray,
        observations: list[Observation],
        model_runs: list[ModelRun],
        counters: dict[str, int | float],
        preview_jpeg: np.ndarray | None = None,
    ) -> dict[str, int]:
        if not self._handle:
            raise RuntimeError("semantic output publisher is closed")
        disparity = _require_array(
            disparity,
            dtype=np.dtype(np.float32),
            shape=(IMAGE_HEIGHT, IMAGE_WIDTH),
            name="disparity",
        )
        encoded_strings: list[bytes] = []
        native_observations = (_Observation * len(observations))()
        for index, source in enumerate(observations):
            label = source.model_label.encode()
            name = source.model_name.encode()
            encoded_strings.extend((label, name))
            target = native_observations[index]
            target.struct_size = ctypes.sizeof(target)
            target.source_kind = source.source_kind
            target.semantic_class = source.semantic_class
            target.observation_id = source.observation_id
            target.model_class_id = source.model_class_id
            target.model_label = label
            target.model_name = name
            target.confidence = source.confidence
            target.x_min, target.y_min, target.x_max, target.y_max = source.box
            target.pixel_count = source.pixel_count
        native_runs = (_ModelRun * len(model_runs))()
        for index, source in enumerate(model_runs):
            name = source.model_name.encode()
            version = source.model_version.encode()
            sha256 = source.model_sha256.encode()
            encoded_strings.extend((name, version, sha256))
            target = native_runs[index]
            target.struct_size = ctypes.sizeof(target)
            target.model_name = name
            target.model_version = version
            target.model_sha256 = sha256
            target.health = source.health
            target.completed_runs = source.completed_runs
            target.errors = source.errors
            target.latency_ms = source.latency_ms
            target.inference_latency_ms = source.inference_latency_ms
            target.postprocess_latency_ms = source.postprocess_latency_ms
        preview = None
        if preview_jpeg is not None:
            if preview_jpeg.dtype != np.uint8 or not preview_jpeg.flags.c_contiguous:
                raise ValueError("semantic preview must be contiguous uint8")
            preview = preview_jpeg
        null_u8 = ctypes.POINTER(ctypes.c_uint8)()
        frame = _OutputFrame()
        frame.struct_size = ctypes.sizeof(frame)
        frame.source_sequence = source_sequence
        frame.capture_realtime_ns = capture_realtime_ns
        frame.ready_realtime_ns = time.time_ns()
        frame.health = int(counters.get("health", HEALTH_OK))
        frame.disparity = disparity.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
        frame.width = IMAGE_WIDTH
        frame.height = IMAGE_HEIGHT
        frame.disparity_stride_elements = disparity.strides[0] // 4
        frame.valid_x_offset = 128
        frame.valid_y_offset = 0
        frame.valid_width = IMAGE_WIDTH - 128
        frame.valid_height = IMAGE_HEIGHT
        frame.min_disparity = 0.0
        frame.max_disparity = 128.0
        frame.observations = (
            ctypes.cast(native_observations, ctypes.POINTER(_Observation))
            if observations
            else ctypes.POINTER(_Observation)()
        )
        frame.observation_count = len(observations)
        frame.model_runs = (
            ctypes.cast(native_runs, ctypes.POINTER(_ModelRun))
            if model_runs
            else ctypes.POINTER(_ModelRun)()
        )
        frame.model_run_count = len(model_runs)
        frame.preview_jpeg = (
            preview.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8))
            if preview is not None
            else null_u8
        )
        frame.preview_jpeg_bytes = 0 if preview is None else preview.nbytes
        frame.submitted_frames = int(counters["submitted_frames"])
        frame.replaced_pending_frames = int(counters["replaced_pending_frames"])
        frame.processed_frames = int(counters["processed_frames"])
        frame.published_results = int(counters["published_results"])
        frame.worker_errors = int(counters["worker_errors"])
        frame.frame_latency_ms = float(counters["frame_latency_ms"])
        frame.max_frame_latency_ms = float(counters["max_frame_latency_ms"])
        stats = _OutputStats()
        stats.struct_size = ctypes.sizeof(stats)
        error = ctypes.create_string_buffer(512)
        code = self._library.depthd_semantic_output_publish(
            self._handle,
            ctypes.byref(frame),
            ctypes.byref(stats),
            error,
            len(error),
        )
        if code != 0:
            raise RuntimeError(error.value.decode(errors="replace"))
        return {
            "semantic_sequence": int(stats.semantic_sequence),
            "object_count": int(stats.object_count),
            "hazard_active": int(stats.hazard_active),
            "hazard_fail_closed_required": int(
                stats.hazard_fail_closed_required
            ),
        }

    def close(self) -> None:
        if getattr(self, "_handle", None):
            self._library.depthd_semantic_output_destroy(self._handle)
            self._handle = None


@dataclass(slots=True)
class _BackendState:
    backend: SemanticBackend
    period_ns: int
    last_run_ns: int = 0
    completed_runs: int = 0
    errors: int = 0
    last_latency_ms: float = 0.0
    last_inference_latency_ms: float = 0.0
    last_postprocess_latency_ms: float = 0.0


@dataclass(slots=True)
class _Job:
    source_sequence: int
    capture_realtime_ns: int
    submitted_ns: int
    color_bgr: np.ndarray
    disparity: np.ndarray


class SemanticWorker:
    """One worker, one pending frame, and no waits in the stereo thread."""

    def __init__(
        self,
        registrations: list[BackendRegistration],
        output: Any,
        *,
        preview_enabled: bool = False,
        preview_quality: int = 70,
    ) -> None:
        if not registrations:
            raise ValueError("at least one semantic backend is required")
        self._backends: list[_BackendState] = []
        for registration in registrations:
            if not math.isfinite(registration.cadence_hz) or registration.cadence_hz <= 0:
                raise ValueError("semantic cadence must be positive")
            self._backends.append(
                _BackendState(
                    registration.backend,
                    round(1_000_000_000 / registration.cadence_hz),
                )
            )
        self._output = output
        self._preview_enabled = preview_enabled
        self._preview_quality = preview_quality
        self._condition = threading.Condition()
        self._pending: _Job | None = None
        self._stopping = False
        self._submitted = 0
        self._replaced = 0
        self._processed = 0
        self._published = 0
        self._errors = 0
        self._last_frame_latency_ms = 0.0
        self._max_frame_latency_ms = 0.0
        self._last_output: dict[str, int] | None = None
        self._thread = threading.Thread(
            target=self._run, name="depthd-semantic", daemon=True
        )
        self._thread.start()

    def wants_frame(self, now_ns: int | None = None) -> bool:
        now_ns = time.monotonic_ns() if now_ns is None else now_ns
        with self._condition:
            if self._stopping:
                return False
            return any(
                state.last_run_ns == 0
                or now_ns - state.last_run_ns >= state.period_ns
                for state in self._backends
            )

    def submit(
        self,
        *,
        source_sequence: int,
        capture_realtime_ns: int,
        color_bgr: np.ndarray,
        disparity: np.ndarray,
        submitted_ns: int | None = None,
    ) -> bool:
        color_bgr = _require_array(
            color_bgr,
            dtype=np.dtype(np.uint8),
            shape=(IMAGE_HEIGHT, IMAGE_WIDTH, 3),
            name="left_rectified_color",
        )
        disparity = _require_array(
            disparity,
            dtype=np.dtype(np.float32),
            shape=(IMAGE_HEIGHT, IMAGE_WIDTH),
            name="disparity",
        )
        job = _Job(
            source_sequence,
            capture_realtime_ns,
            time.monotonic_ns() if submitted_ns is None else submitted_ns,
            color_bgr,
            disparity,
        )
        with self._condition:
            if self._stopping:
                return False
            self._submitted += 1
            if self._pending is not None:
                self._replaced += 1
            self._pending = job
            self._condition.notify()
        return True

    def _run(self) -> None:
        while True:
            with self._condition:
                self._condition.wait_for(
                    lambda: self._stopping or self._pending is not None
                )
                if self._stopping:
                    return
                job = self._pending
                self._pending = None
            assert job is not None
            self._process(job)

    def _process(self, job: _Job) -> None:
        frame_started_ns = time.monotonic_ns()
        with self._condition:
            due = [
                state
                for state in self._backends
                if state.last_run_ns == 0
                or job.submitted_ns - state.last_run_ns >= state.period_ns
            ]
            for state in due:
                state.last_run_ns = job.submitted_ns
            self._processed += 1
        if not due:
            return
        observations: list[Observation] = []
        model_runs: list[ModelRun] = []
        successes = 0
        for state in due:
            started_ns = time.monotonic_ns()
            health = HEALTH_OK
            try:
                observations.extend(state.backend.infer(job.color_bgr))
                state.completed_runs += 1
                successes += 1
            except Exception:
                state.errors += 1
                health = HEALTH_FAULT
                with self._condition:
                    self._errors += 1
            state.last_latency_ms = (time.monotonic_ns() - started_ns) / 1_000_000
            state.last_inference_latency_ms = float(
                getattr(
                    state.backend,
                    "last_inference_latency_ms",
                    state.last_latency_ms,
                )
            )
            state.last_postprocess_latency_ms = float(
                getattr(state.backend, "last_postprocess_latency_ms", 0.0)
            )
            model_runs.append(
                ModelRun(
                    state.backend.name,
                    str(getattr(state.backend, "model_version", "unversioned")),
                    str(getattr(state.backend, "model_sha256", "unverified")),
                    health,
                    state.completed_runs,
                    state.errors,
                    state.last_latency_ms,
                    state.last_inference_latency_ms,
                    state.last_postprocess_latency_ms,
                )
            )
        observations.sort(
            key=lambda value: (
                value.semantic_class not in {CLASS_HUMAN, CLASS_ANIMAL},
                value.source_kind != SOURCE_INSTANCE,
                -value.pixel_count,
                -value.confidence,
            )
        )
        observations = observations[:64]
        frame_latency_ms = (time.monotonic_ns() - frame_started_ns) / 1_000_000
        preview = None
        if self._preview_enabled:
            encoded, preview = cv2.imencode(
                ".jpg",
                job.color_bgr,
                [cv2.IMWRITE_JPEG_QUALITY, self._preview_quality],
            )
            if not encoded:
                preview = None
        health = (
            HEALTH_OK
            if successes == len(due)
            else (HEALTH_FAULT if successes == 0 else HEALTH_DEGRADED)
        )
        with self._condition:
            predicted_published = self._published + 1
            self._last_frame_latency_ms = frame_latency_ms
            self._max_frame_latency_ms = max(
                self._max_frame_latency_ms, frame_latency_ms
            )
            counters = {
                "health": health,
                "submitted_frames": self._submitted,
                "replaced_pending_frames": self._replaced,
                "processed_frames": self._processed,
                "published_results": predicted_published,
                "worker_errors": self._errors,
                "frame_latency_ms": self._last_frame_latency_ms,
                "max_frame_latency_ms": self._max_frame_latency_ms,
            }
        try:
            result = self._output.publish(
                source_sequence=job.source_sequence,
                capture_realtime_ns=job.capture_realtime_ns,
                disparity=job.disparity,
                observations=observations,
                model_runs=model_runs,
                counters=counters,
                preview_jpeg=preview,
            )
            with self._condition:
                self._published = predicted_published
                self._last_output = result
                self._condition.notify_all()
        except Exception:
            with self._condition:
                self._errors += 1
                self._condition.notify_all()

    def stats(self) -> dict[str, Any]:
        with self._condition:
            return {
                "submitted_frames": self._submitted,
                "replaced_pending_frames": self._replaced,
                "processed_frames": self._processed,
                "published_results": self._published,
                "worker_errors": self._errors,
                "last_frame_latency_ms": self._last_frame_latency_ms,
                "max_frame_latency_ms": self._max_frame_latency_ms,
                "pending": self._pending is not None,
                "last_output": self._last_output,
                "models": {
                    state.backend.name: {
                        "completed_runs": state.completed_runs,
                        "errors": state.errors,
                        "last_latency_ms": state.last_latency_ms,
                        "last_inference_latency_ms": (
                            state.last_inference_latency_ms
                        ),
                        "last_postprocess_latency_ms": (
                            state.last_postprocess_latency_ms
                        ),
                    }
                    for state in self._backends
                },
            }

    def wait_for_published(self, count: int, timeout_s: float) -> bool:
        deadline = time.monotonic() + timeout_s
        with self._condition:
            while self._published < count and self._errors == 0:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False
                self._condition.wait(remaining)
            return self._published >= count

    def close(self) -> None:
        with self._condition:
            if self._stopping:
                return
            self._stopping = True
            self._pending = None
            self._condition.notify_all()
        self._thread.join()
        seen: set[int] = set()
        for state in self._backends:
            backend_id = id(state.backend)
            if backend_id not in seen:
                seen.add(backend_id)
                state.backend.close()
        self._output.close()


@dataclass(slots=True)
class _HailoNetwork:
    name: str
    hef: Any
    network_group: Any
    activation_params: Any
    input_info: Any
    output_infos: list[Any]
    infer_pipeline: Any


class HailoSemanticOwner:
    """One persistent HailoRT owner for every semantic network."""

    def __init__(self, model_paths: dict[str, Path]) -> None:
        try:
            import hailo_platform as hp  # type: ignore[import-not-found]
        except ImportError as exc:
            raise RuntimeError(f"hailo_platform is unavailable: {exc}") from exc
        for name, path in model_paths.items():
            if not path.is_file():
                raise FileNotFoundError(f"{name} HEF not found: {path}")
        self._hp = hp
        self._lock = threading.Lock()
        self._vdevice = hp.VDevice()
        self._networks: dict[str, _HailoNetwork] = {}
        try:
            for name, path in model_paths.items():
                hef = hp.HEF(str(path))
                configure = hp.ConfigureParams.create_from_hef(
                    hef=hef, interface=hp.HailoStreamInterface.PCIe
                )
                group = self._vdevice.configure(hef, configure)[0]
                activation = group.create_params()
                inputs = hp.InputVStreamParams.make_from_network_group(
                    group, quantized=True, format_type=hp.FormatType.UINT8
                )
                output_uint8 = name == "segformer_b0_bn"
                outputs = hp.OutputVStreamParams.make_from_network_group(
                    group,
                    quantized=output_uint8,
                    format_type=(
                        hp.FormatType.UINT8
                        if output_uint8
                        else hp.FormatType.FLOAT32
                    ),
                )
                pipeline = hp.InferVStreams(group, inputs, outputs)
                pipeline.__enter__()
                self._networks[name] = _HailoNetwork(
                    name,
                    hef,
                    group,
                    activation,
                    hef.get_input_vstream_infos()[0],
                    list(hef.get_output_vstream_infos()),
                    pipeline,
                )
        except Exception:
            self.close()
            raise

    def infer(self, model_name: str, image_bgr: np.ndarray) -> dict[str, Any]:
        network = self._networks[model_name]
        shape = tuple(int(value) for value in network.input_info.shape)
        if len(shape) != 3:
            raise RuntimeError(f"unsupported {model_name} input shape: {shape}")
        height, width = shape[0], shape[1]
        resized_bgr = cv2.resize(
            image_bgr, (width, height), interpolation=cv2.INTER_LINEAR
        )
        model_rgb = cv2.cvtColor(resized_bgr, cv2.COLOR_BGR2RGB)
        input_name = network.input_info.name
        with self._lock:
            with network.network_group.activate(network.activation_params):
                return network.infer_pipeline.infer(
                    {input_name: np.expand_dims(model_rgb, axis=0)}
                )

    def close(self) -> None:
        networks = getattr(self, "_networks", {})
        for network in reversed(list(networks.values())):
            try:
                network.infer_pipeline.__exit__(None, None, None)
            except Exception:
                pass
        networks.clear()
        vdevice = getattr(self, "_vdevice", None)
        if vdevice is not None and hasattr(vdevice, "release"):
            try:
                vdevice.release()
            except Exception:
                pass
        self._vdevice = None


_COCO_LABELS = (
    "person bicycle car motorcycle airplane bus train truck boat traffic_light "
    "fire_hydrant stop_sign parking_meter bench bird cat dog horse sheep cow "
    "elephant bear zebra giraffe backpack umbrella handbag tie suitcase frisbee "
    "skis snowboard sports_ball kite baseball_bat baseball_glove skateboard "
    "surfboard tennis_racket bottle wine_glass cup fork knife spoon bowl banana "
    "apple sandwich orange broccoli carrot hot_dog pizza donut cake chair couch "
    "potted_plant bed dining_table toilet tv laptop mouse remote keyboard cell_phone "
    "microwave oven toaster sink refrigerator book clock vase scissors teddy_bear "
    "hair_drier toothbrush"
).split()


class HailoYoloBackend:
    name = "yolov8s"
    model_version = "robot-validated-hailo8l"
    model_sha256 = "17270a35bcf3e27a8dcd1dfae8d328a31318bb7c5b7c7a725392b83d008642c6"

    def __init__(self, owner: HailoSemanticOwner, confidence: float = 0.35) -> None:
        self._owner = owner
        self._confidence = confidence

    def infer(self, image_bgr: np.ndarray) -> list[Observation]:
        inference_started_ns = time.monotonic_ns()
        outputs = self._owner.infer(self.name, image_bgr)
        self.last_inference_latency_ms = (
            time.monotonic_ns() - inference_started_ns
        ) / 1_000_000
        postprocess_started_ns = time.monotonic_ns()
        result: list[Observation] = []
        observation_id = 1
        for class_id, row in _iter_hailo_nms_rows(outputs):
            score = float(row[4])
            if score < self._confidence or not 0 <= class_id < len(_COCO_LABELS):
                continue
            label = _COCO_LABELS[class_id]
            semantic_class = (
                CLASS_HUMAN
                if label == "person"
                else CLASS_ANIMAL if label in {"bird", "cat", "dog"} else None
            )
            if semantic_class is None:
                continue
            y0, x0, y1, x1 = (float(value) for value in row[:4])
            if max(y0, x0, y1, x1) <= 1.5:
                box = (
                    x0 * IMAGE_WIDTH,
                    y0 * IMAGE_HEIGHT,
                    x1 * IMAGE_WIDTH,
                    y1 * IMAGE_HEIGHT,
                )
            else:
                box = (x0, y0, x1, y1)
            result.append(
                Observation(
                    SOURCE_INSTANCE,
                    semantic_class,
                    observation_id,
                    class_id,
                    label,
                    self.name,
                    score,
                    box,
                )
            )
            observation_id += 1
        self.last_postprocess_latency_ms = (
            time.monotonic_ns() - postprocess_started_ns
        ) / 1_000_000
        return result

    def close(self) -> None:
        self._owner.close()


def _iter_hailo_nms_rows(outputs: dict[str, Any]):
    for value in outputs.values():
        candidate = value
        if isinstance(candidate, np.ndarray) and candidate.dtype == object:
            candidate = candidate.tolist()
        if isinstance(candidate, (list, tuple)):
            groups = candidate
            if len(groups) == 1 and isinstance(groups[0], (list, tuple)):
                groups = groups[0]
            for class_id, rows_value in enumerate(groups):
                rows = np.asarray(rows_value, dtype=np.float32)
                if rows.size == 0:
                    continue
                rows = rows.reshape(-1, rows.shape[-1])
                for row in rows:
                    if row.size >= 5:
                        yield class_id, row
        else:
            tensor = np.asarray(candidate).squeeze()
            if tensor.ndim == 2 and tensor.shape[-1] >= 6:
                for row in tensor:
                    yield int(round(float(row[5]))), row


_CITYSCAPES_MAPPING = {
    3: ("wall", CLASS_WALL),
    4: ("fence", CLASS_FENCE),
    8: ("vegetation", CLASS_VEGETATION),
    9: ("terrain", CLASS_TERRAIN),
    11: ("person", CLASS_HUMAN),
    12: ("rider", CLASS_HUMAN),
}


class HailoSegformerBackend:
    name = "segformer_b0_bn"
    model_version = "hailo-model-zoo-v2.17.0"
    model_sha256 = "2093951835BAFDB0585F93E3EDDFB13FAC2AA926C45475CCEDABE5B2DAA7A674"

    def __init__(self, owner: HailoSemanticOwner, min_region_pixels: int = 128) -> None:
        self._owner = owner
        self._min_region_pixels = min_region_pixels

    def infer(self, image_bgr: np.ndarray) -> list[Observation]:
        inference_started_ns = time.monotonic_ns()
        outputs = self._owner.infer(self.name, image_bgr)
        self.last_inference_latency_ms = (
            time.monotonic_ns() - inference_started_ns
        ) / 1_000_000
        postprocess_started_ns = time.monotonic_ns()
        if len(outputs) != 1:
            raise RuntimeError(f"unexpected SegFormer outputs: {list(outputs)}")
        mask = np.asarray(next(iter(outputs.values()))).squeeze()
        if mask.shape != (512, 1024):
            raise RuntimeError(f"unexpected SegFormer output shape: {mask.shape}")
        result: list[Observation] = []
        observation_id = 10_000
        scale_x = IMAGE_WIDTH / mask.shape[1]
        scale_y = IMAGE_HEIGHT / mask.shape[0]
        for class_id, (label, semantic_class) in _CITYSCAPES_MAPPING.items():
            binary = np.equal(mask, class_id).astype(np.uint8)
            count, _, stats, _ = cv2.connectedComponentsWithStats(binary, 8)
            for component in range(1, count):
                x, y, width, height, pixels = (
                    int(value) for value in stats[component]
                )
                if pixels < self._min_region_pixels:
                    continue
                result.append(
                    Observation(
                        SOURCE_DENSE_REGION,
                        semantic_class,
                        observation_id,
                        class_id,
                        label,
                        self.name,
                        1.0,
                        (
                            x * scale_x,
                            y * scale_y,
                            (x + width) * scale_x,
                            (y + height) * scale_y,
                        ),
                        pixels,
                    )
                )
                observation_id += 1
        result.sort(key=lambda value: value.pixel_count, reverse=True)
        self.last_postprocess_latency_ms = (
            time.monotonic_ns() - postprocess_started_ns
        ) / 1_000_000
        return result[:48]

    def close(self) -> None:
        self._owner.close()


def make_hailo_backends(
    yolo_hef: Path,
    segformer_hef: Path,
    *,
    yolo_hz: float = 5.0,
    segformer_hz: float = 5.0,
) -> tuple[list[BackendRegistration], HailoSemanticOwner]:
    owner = HailoSemanticOwner(
        {"yolov8s": yolo_hef, "segformer_b0_bn": segformer_hef}
    )
    return (
        [
            BackendRegistration(HailoYoloBackend(owner), yolo_hz),
            BackendRegistration(HailoSegformerBackend(owner), segformer_hz),
        ],
        owner,
    )
