# Architecture decision: deployment boundary

Status: **Proposed — owner decision required**

The processing design is the same in every option: a C++20, ROS-free core owns
capture, trigger/sync validation, rectification, HailoRT, post-processing, and
preallocated buffers. Only the deployment and ROS boundary changes.

## Invariants

1. Raw stereo frames and intermediate tensors never enter DDS.
2. The newest complete pair wins; queues never build latency.
3. Calibration, capture timestamps, and health travel with every result.
4. ROS types are absent from the core and model layers.
5. Hardware access has a single owner.
6. The future compact cloud consumes disparity through the same stable result
   interface; it does not change capture or inference.

## Option A — host daemon plus ROS bridge (recommended)

```text
Pi host: depthd (cameras + Hailo + processing)
                 |
                 | file-backed mmap ring + Unix socket
                 v
existing Nuvo ROS container: depthd_ros_bridge -> ROS topics
```

The bridge maps a read-only, versioned four-slot ring under `/run/depthd` and
receives sequence notifications/control over a Unix-domain socket. It publishes
the newest completed result and reconnects across either process restarting.

Advantages:

- Cleanest separation from the existing Dockerized ROS deployment.
- Camera and Hailo access stay native and easy to diagnose.
- ROS restarts cannot interrupt capture or inference.
- Depth runtime and ROS integration can be tested independently.

Costs:

- Two executables and a small versioned IPC protocol.
- One explicit handoff of the final disparity/JPEG result.

This is the best balance for Nuvo. The final disparity is already a contiguous
ROS-ready float32 image, so the bridge does not perform depth math or image
conversion.

## Option B — one process with an optional ROS adapter

```text
depthd process
  ROS-free core -> in-process ROS adapter -> ROS topics
```

Advantages:

- Fewest runtime parts and no IPC protocol.
- Lowest possible handoff overhead.
- Easiest single-host debugging when ROS is installed natively.

Costs:

- The executable and deployment inherit ROS libraries and lifecycle behavior.
- Awkward with Nuvo's existing ROS container: either move the process into that
  container and pass camera/Hailo devices through, or install ROS on the host.
- A ROS crash/restart also restarts hardware ownership.

Choose this only if absolute runtime simplicity matters more than keeping the
current host/container boundary.

## Option C — separate depth and ROS containers

```text
host drivers -> depth container -> shared /run/depthd -> ROS container
```

Advantages:

- Independent image-based OTA and rollback for depth and ROS.
- Reuses the same core and IPC contract as Option A.

Costs:

- CSI camera access requires several media/video/DMA devices and matching Pi
  camera libraries inside the image.
- Hailo host driver and container HailoRT versions must stay pinned together.
- More fragile bring-up and more deployment surface than Option A.

Choose this after Option A works if independent depth-container OTA is worth the
device-mapping complexity.

## Decision summary

| Criterion | A: host + bridge | B: one process | C: two containers |
| --- | --- | --- | --- |
| Hot-path CPU | Excellent | Best | Excellent |
| ROS isolation | Best | Weak | Best |
| Initial bring-up | Good | Best only with native ROS | Hardest |
| Existing Nuvo fit | Best | Poor | Good |
| OTA independence | Good | Weak | Best |
| Maintenance risk | Low | Low/medium | Medium/high |

Recommendation: **Option A**, with the IPC layer kept narrow enough that the same
binary can move into Option C later without redesigning the core.
