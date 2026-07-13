# Native semantic perception architecture

## Decision

Semantic perception is a ROS-free module inside Depth Daemon. It uses the
already rectified full-color left image, the same-frame disparity, and the
existing calibration/transform. ROS receives only compact semantic objects,
hazards, health, and an optional preview from a separate versioned mmap ring.

The production stereo path and its result-ring ABI are unchanged. Geometry is
always authoritative: semantic classification can annotate an obstacle and
add a living-obstacle stop, but it never removes points from the compact cloud
or declares unknown geometry safe.

## Image branch and ownership

```text
triggered left/right camera pair
  -> grayscale conversion + mono rectification
  -> CPU SGBM + restored 728x544 disparity
  -> compact obstacle cloud + depth result ring (unchanged, 10 Hz)

left camera buffer
  -> color rectification only when JPEG or semantic work is due
  -> shared immutable rectified BGR owner
       -> optional 2 Hz JPEG encoder
       -> capacity-one semantic worker
            -> YOLOv8s instances (5 Hz)
            -> SegFormer dense regions (5 Hz)
            -> same-frame disparity fusion
            -> /run/depthd/semantic.ring
```

The branch is not after SGBM in a data-dependency sense: inference only needs
the color image. Submission is made after that frame's disparity becomes
available so the job contains a coherent image/disparity/timestamp tuple. The
worker then overlaps inference with capture and stereo processing for the next
frame. It does not consume the grayscale matcher image or the lossy 2 Hz JPEG.

The job owns `shared_ptr<const vector<...>>` buffers. A future camera runtime
can move an existing pooled color/disparity buffer into this owner with no
full-frame copy. Hailo-specific input staging remains inside its backend and is
limited to the resize/letterbox and DMA requirements of the selected network.

The deployed Python seam follows the same ownership rule: OpenCV's full-size
`remap` result is already an owned contiguous BGR ndarray, and the latest-only
worker retains that exact object. BGR-to-RGB conversion happens only after the
frame is resized into the unavoidable model-sized input staging buffer. There
is no full-resolution channel-swap copy.

## Backend and scheduling contract

`ISemanticBackend` returns either instance detections or compact dense regions.
`SemanticPerceptionModule` owns all backends and one worker thread. Submission
is nonblocking and the pending queue has capacity one; a newer frame replaces
an older pending frame rather than building latency. Replacement, error, run,
and latency counters are explicit.

Each backend has an independent cadence clock. Both selected models start at
5 Hz, but this supports lowering dense segmentation or adding future models
without changing the camera/depth loop. One runtime owner must serialize or
schedule all Hailo-8L work because this robot has one accelerator. Stereo stays
on the CPU.

The portable core has no Hailo headers or link dependency. The deployed Hailo
backend is a small adapter around persistent HailoRT VStreams and its decoder. This
keeps host regression tests deterministic and prevents accelerator/toolchain
changes from leaking into safety geometry.

Schema v1 never carries an observation across frames: every box or region is
fused only with disparity from its own image and retains that frame's sequence
and timestamp. When model cadences differ, the ring contains a model-specific
partial update and its model-run list states exactly which model ran. Consumers
must not interpret an omitted model as a negative detection. Both production
models remain aligned at 5 Hz, so normal production updates contain both.

The deployed backend keeps one HailoRT `VDevice`, both configured network
groups, and both `InferVStreams` pipelines alive for the process lifetime. One
lock serializes activation/inference because the robot has a single Hailo-8L.
Per-model records distinguish inference, postprocess, and total latency and
carry model version plus SHA-256 provenance.

## Fusion and safety

The fusion stage samples valid disparity inside the central 60% of each
detection/region, rejects invalid/out-of-range samples, projects through the
calibrated stereo model, transforms to `base_link`, and reports a robust median
point, horizontal range/bearing, valid fraction, support count, and range
standard deviation.

Initial safety policy mirrors the existing behavior:

- a ranged human or animal inside 3.0 m and +/-0.90 rad activates a stop;
- a detected human or animal without trustworthy range fails closed;
- stale semantic perception is explicitly fail-closed, never a fresh clear;
- vegetation, walls, fences, and terrain are annotations; the compact geometry
  pipeline remains the collision authority for them and for all unknown items.

Future semantic filtering must be additive until field data proves a safe rule.
Any non-OK result health, including one failed model in an otherwise successful
frame, publishes a fail-closed hazard. A hung or dead worker cannot publish a
new sequence; the ROS bridge must treat ring age beyond its configured deadline
as fail-closed. It must never turn an old slot into a newly stamped clear
heartbeat.

## Separate mmap ABI

`/run/depthd/semantic.ring` has schema version 1, four fixed slots, generation
guards, 64 fixed-size object records, model/health counters, one hazard record,
and a 512 KiB optional JPEG area. No raw class mask is published by default.
Each model record carries its version, SHA-256, total latency, inference
latency, and postprocess latency. The reader rejects incompatible versions and
torn generations.

Keeping this separate from `/run/depthd/results.ring` protects the locked depth
ABI and lets the ROS bridge poll depth and semantics independently. A later ROS
adapter should publish the existing semantic-object/hazard/health contracts
and should not republish raw images or masks unless a diagnostic flag explicitly
enables the preview.

## Initial models and risks

- Instances: the known robot `yolov8s` HEF. Initially keep only person and pet
  mappings; irrelevant COCO classes are dropped by postprocessing.
- Dense regions: official Hailo Model Zoo v2.17.0 `segformer_b0_bn` for
  vegetation, wall, fence, and terrain.

SegFormer Cityscapes is an urban-domain starting point, not proof of lawn
quality. It requires recorded indoor/outdoor validation. The manifest also
records the Ultralytics, NVIDIA, and dataset license review gates. A future
custom lawn dataset can replace only the dense backend while preserving the
module, fusion, ring, and ROS contracts.
