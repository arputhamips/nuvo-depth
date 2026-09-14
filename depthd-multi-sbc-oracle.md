# Executive verdict

**This should be evolved through a bounded architectural refactor, not rewritten.** The project already has a strong portable processing core, unusually good DMA-BUF lifecycle thinking, useful deterministic IPC, and clean separation between depth products, semantic perception, and ROS-facing publication. The weak point is the outer system: capture, accelerator memory, asynchronous execution, calibration identity, timing, safety semantics, packaging, and product transport remain fixed to the current Pi rig.

The practical platform order is:

1. **Pi 5/CM5 + Hailo-8L:** first production implementation.
2. **RK3588:** best next cost/performance implementation.
3. **Jetson:** premium backend and reference quality ceiling.
4. **Milk-V:** a deliberately reduced **Lite** product, not the same feature profile.

One terminology correction is important: **CV1800B, SG2002, and SG2000 are different targets.** The original Milk-V Duo uses CV1800B; Duo256M uses SG2002 with 256 MB and 1 TOPS; the board explicitly advertised with two 2-lane CSI camera interfaces is Duo S, which instead uses SG2000, 512 MB, and approximately 0.5 TOPS. The currently documented off-the-shelf boards therefore do not give you “SG2002 + Duo S dual-camera connectivity” as one package. ([Milk-V][1])

The supplied bundle is sufficient to audit the contracts and selected implementations, but it does not include several implementations referenced by CMake, including the Hailo backend, production EGL platform, capture scripts, semantic-ring implementation, and most tests. Consequently, I can verify the exposed architecture and the shown CPU/IPC/semantic logic, but not the correctness of Hailo tensor decoding, GPU shader output, camera request pairing, or omitted test evidence.

---

# Stop-the-line findings

## 1. The semantic hazard path can issue an unsafe clear

Each frame constructs a new observation vector and runs only the backends whose cadence is due. Results from non-due backends are not retained. The freshly constructed subset is then fused and published as the current result. Because `SemanticBackendRegistration` explicitly allows different cadences, a faster segmentation model can publish a clear result between runs of a slower human detector, erasing the detector’s still-valid positive observation.   

There are several related safety defects:

* “Stale” is inferred from whether the models that ran returned an error. A successful result that arrives far past its deadline is still `kOk`.
* `latest_result_` may remain available indefinitely; this module has no result-age transition to stale.
* There is no temporal tracking, positive hold, clear hysteresis, or requirement that all safety-required models be fresh before clearing.
* `stop_on_unranged_living` stops for any unranged human or animal, but the safety decision ignores model confidence, image location, persistence, and model-specific reliability.
* The first matching object is returned, not the nearest or most severe object.  

**Required correction:** maintain per-model timestamped state, declare required safety models, expire results by monotonic age, and publish clear only when every required source is fresh and has produced a sufficiently persistent clear. Until then, this output should be called **hazard advisory**, not a braking or E-stop authority.

## 2. Runtime calibration and stereo metadata are being lost

Geometry is duplicated across the general contract, obstacle-cloud defaults, IPC layout, C++ camera-model factories, and YAML. The contract hard-codes 728×544, focal length, baseline, and frame names. 

More seriously, the ring’s `ResultSnapshotView` carries only a disparity span and no runtime disparity scale, valid window, min/max disparity, confidence, or calibration reference. The slot header instead defaults to a fixed 128-pixel invalid margin, fixed focal length and baseline, fixed maximum disparity, and fixed `delta_d = 1/16`. The writer never replaces those fields with backend-specific values.   

That is already inaccurate for the shown half-resolution SGBM path: one SGBM quantization step at 364-pixel working width becomes approximately **1/8 full-resolution pixel** after the ×2 scale, before interpolation. Future learned, VPI, Vulkan, or RKNN backends will have different valid regions and output quantization.

The camera-info fallback is also compiled from hard-coded intrinsics and a hard-coded right projection offset. 

**Required correction:** every output must carry a runtime `CalibrationRef`, valid ROI, disparity format and scale, invalid convention, min/max search range, confidence format, and actual rectified camera model. `calibration_id` should be a content hash, not a default integer of `1`.

## 3. The supplied build is not yet a native multi-platform daemon

The algorithm interfaces are native C++, but the supplied CMake defines benchmarks, inspectors, libraries, and shared C wrappers—not an installed `depthd` capture-and-processing executable. Production installation is centered on `tools/pi/*.py`, a Pi/Hailo YAML, and Pi-specific systemd assets.  

The package dependency list is unconditionally Pi/Hailo-oriented—Picamera2, EGL/GLES, HailoRT, and the Hailo PCIe driver—and release packaging explicitly requires the EGL/GL rectifier. 

So the current deliverable is best described as a **native processing core embedded in a Pi-specific Python orchestration system**, not yet a C++20 multi-SBC daemon.

## 4. The current backend contracts cannot express a real zero-copy accelerator pipeline

`StereoBackend::process()` accepts host pointers and returns a host `float*` synchronously. `ISemanticBackend::infer()` accepts a host BGR pointer and returns heap-owning vectors synchronously.  

The rectifier has a sophisticated DMA-BUF input path, but its output is still a `MutableImageView` CPU pointer. Therefore, even a zero-copy camera import can end in GPU-to-CPU readback before stereo or inference. 

The raw `DmabufRequestLease*` is clear and appropriate for the current synchronous call, but it prevents asynchronous work from retaining a camera buffer safely. Cross-platform acceleration requires shared lease ownership plus completion fences.

## 5. The MMAP transport needs hardening before becoming a public ABI

The ring concept is good, but the current layout is fixed at four 728×544 slots, fixed capacities, and a fixed total size of 11,620,352 bytes. 

Specific issues:

* At 10 Hz, disparity alone causes about **15.8 MB/s of writer copying**, followed by another complete copy in each reader.
* The result and semantic rings together reserve about **13.2 MiB**, material on a 256 MB SG2002.
* Readers cannot selectively consume only metadata or obstacle points; `read_latest()` copies full vectors.
* The default file mode is `0644`, exposing camera-derived data to every local user. 
* The two IPC schemas use opposite-looking endian constants: `0x04030201` versus `0x01020304`. That is at minimum a maintenance trap.  
* Layout assertions verify size and trivial copyability, but not each field offset or 64-bit atomic lock-freedom. 
* The non-GNU atomic fallback is volatile plus fences, which is not an adequate general cross-process atomic implementation. 
* Writer publication stores `latest_slot` before `latest_sequence`, while the reader loads sequence before slot. During the small publication window for the next frame, a reader can combine the old sequence with the new slot and report a spurious torn read. The slot should simply be derived from the acquired sequence.  
* Latency validity is based on `system_clock` timestamps and rejects `ready < capture`. A wall-clock adjustment can therefore invalidate a healthy frame. Latency and freshness must use a monotonic clock; wall time should be separate correlation metadata. 

Keep the latest-only ring idea, but do not freeze this layout as the product SDK ABI.

## 6. The CPU stereo baseline has quality hazards

The current implementation downsamples both images to 364×272, runs SGBM, linearly upsamples the raw disparity values, scales them by two, and only afterward applies a nearest-neighbor validity mask. 

Linear interpolation can blend OpenCV’s invalid disparity sentinel into adjacent pixels whose nearest-neighbor validity mask is valid. This can create incorrect depths at precisely the occlusion and object-edge regions most important for obstacle avoidance.

Other limitations are:

* Very permissive `uniqueness_ratio: 1`.
* No confidence output.
* No explicit occlusion classification.
* No edge-aware upsampling.
* No temporal consistency.
* No backend-independent uncertainty representation.
* `cv::setNumThreads()` changes global OpenCV state for the whole process.

This remains a useful deterministic fallback and regression oracle, but it should not define your public depth-quality ceiling.

## 7. The Milk-V target is not hardware-qualified

Duo256M’s documentation establishes SG2002, 256 MB, and 1 TOPS, but does not establish an off-the-shelf dual-camera arrangement. Duo S establishes two 2-lane CSI interfaces, but uses SG2000 and approximately 0.5 TOPS. ([Milk-V][1])

The documented hardware-accelerated Milk-V OpenCV path currently says its MIPI/ISP support is for the official GC2083 module. IMX296 driver, tuning, external-trigger operation, and simultaneous stereo capture are therefore substantial bring-up projects, not mere backend work. ([Milk-V][2])

Make dual IMX296 electrical and driver validation a **hardware go/no-go gate** before porting the daemon.

## 8. Hailo licensing needs commercial review

The Hailo firmware EULA currently published in the official HailoRT repository appears to restrict public benchmarking, public comparative analyses, and use in competing-development activities. It also imposes binary-redistribution and open-source-related conditions. Before publishing OAK/ZED comparisons or distributing a fully open Hailo-backed image, obtain written clarification from Hailo and legal review. ([GitHub][3])

This does not invalidate Hailo technically, but it is a product-positioning and distribution risk that should be resolved before launch.

---

# 1. What is already excellent and reusable

## A. Rectification architecture

This is the strongest part of the codebase.

The code cleanly separates immutable calibration maps, CPU image views, DMA-BUF planes, buffer identity, request lifetime, diagnostics, and backend registration. The explicit warning that an FD is an importable allocation rather than a CPU pointer is exactly the right abstraction boundary. The use of both a stable `buffer_id` and device/inode identity addresses FD reuse across camera restarts.  

The EGL implementation seam is also very good:

* Injectable platform implementation.
* Testable cache/lifecycle policy.
* Explicit import and release.
* Resident map textures.
* Owned-FD accounting.
* GPU timing capability reporting.
* Clean initialization fallback. 

**Keep this design.** Generalize its output memory and asynchronous lifetime rather than replacing it.

## B. Explicit backend selection and fallback policy

`RectificationBackendRegistry` keeps optional implementations outside the common contract and makes fallback an explicit orchestrator decision. That pattern should be copied to stereo, capture, encoders, semantics, and transport. 

The important refinement is that fallback must become deadline-aware. A CPU fallback that turns a 10 Hz pipeline into 2 Hz is not “healthy fallback”; it is a degraded state that downstream safety logic must see.

## C. Latest-only, non-blocking perception handoff

A capacity-one latest-frame queue is exactly right for robotics perception. It prevents a slow detector from accumulating stale work and blocking depth capture. The explicit replaced-frame counters are useful operational telemetry.  

Keep the latest-only policy, but move scheduling to a shared graph/resource scheduler and retain per-model result state.

## D. Model provenance and diagnostics

The semantic result records model name, version, SHA-256, health, run counts, errors, and inference/postprocessing timing. That is substantially better than many prototype perception systems. 

Extend the provenance record with:

* Source ONNX hash.
* Vendor compiler version.
* Runtime/driver version.
* Quantization dataset hash.
* Preprocessing and postprocessing version.
* Label-map hash.
* Target accelerator and architecture.
* Golden-output test-vector ID.

## E. Compact obstacle cloud design

The compact cloud generator is appropriately robot-specific rather than blindly publishing hundreds of thousands of points. It has:

* Fixed-capacity scratch state.
* No intended steady-state heap growth.
* Rich rejection counters.
* Valid-window awareness.
* Voxel support and connected-cluster gates.
* Deterministic sorted output.  

Keep it as the low-bandwidth obstacle product. Add a separate optional dense point-cloud product rather than turning this component into one.

Its main needed evolution is ground- and attitude-awareness. A static optical-to-base transform plus fixed height thresholds is vulnerable to robot pitch, slopes, ruts, and rig flex.

## F. MMAP ring publication concept

The ring has several good ideas:

* Single-writer enforcement with `flock`.
* Magic and schema version.
* Writer-instance ID.
* Restart detection.
* Dropped-sequence accounting.
* Begin/end guards for torn-read detection.
* No payload heap allocation in the writer.
* Explicit fixed capacities.  

Keep it as an internal local data plane, but introduce a v2 descriptor layout rather than extending the fixed v1 struct indefinitely.

## G. Build decomposition and test intent

The code is split into small libraries for contract, rectification, obstacle cloud, rings, semantic core, output, and optional backends. The CMake also references focused tests for the contract, ring, rectifier, semantics, Python adapters, fallback behavior, and production profile.  

The target decomposition is reusable. The packaging logic is what needs replacement.

## H. Operational calibration discipline

The YAML documents a real calibration-side assignment failure and records the measured difference between the correct mapping and the incorrect mapping. That is excellent engineering practice. 

The next step is to encode that knowledge into serial-number binding so it cannot regress through camera-index changes.

---

# 2. What needs per-platform abstraction

The current algorithm virtual classes are not sufficient. Platform differences occur below, beside, and above those algorithms.

## Required platform service interfaces

### `ICaptureBackend`

It should own:

* Sensor enumeration and stable identity.
* Media graph or Argus/libcamera configuration.
* ISP stream configuration.
* Camera controls.
* Exposure/gain coupling across the pair.
* Hardware-trigger configuration.
* Frame pairing.
* Buffer-pool lifecycle.
* Sensor timestamps and dropped-frame accounting.
* Camera restart and recovery.

It should output a **paired capture object**, not two loosely related image callbacks.

### `ITriggerBackend`

External trigger generation is a separate hardware responsibility:

* GPIO character-device implementation.
* Hardware PWM/timer implementation.
* MCU/FPGA implementation.
* Trigger sequence ID.
* Exposure pulse width.
* Actual trigger timestamp.
* Fault detection for missing or extra frames.

A userspace GPIO pulse can be retained for development, but production should prefer a hardware-timed pulse or external timing controller.

### `IBufferStorage` and `IBufferAllocator`

Do not put CUDA, RKNN, or CVI types directly in public frame structs. Use a type-erased storage interface that can expose supported representations:

```cpp
enum class MemoryDomain : std::uint8_t {
    kHost,
    kDmaBuf,
    kCudaDevice,
    kVendorSurface,
};

struct DmaBufPlane {
    int fd{-1};
    std::uint32_t offset{};
    std::uint32_t stride{};
    std::uint32_t size{};
    std::uint32_t fourcc{};
    std::uint64_t modifier{};
};

class BufferStorage {
public:
    virtual ~BufferStorage() = default;
    virtual MemoryDomain domain() const noexcept = 0;
    virtual std::span<const DmaBufPlane> dmabuf_planes() const noexcept = 0;
    virtual std::byte* map_cpu() = 0;
    virtual void unmap_cpu() noexcept = 0;
};

struct ImageBuffer {
    std::shared_ptr<BufferStorage> storage;
    std::uint32_t width{};
    std::uint32_t height{};
    PixelFormat format{};
};
```

The shared storage object becomes the lease. An asynchronous accelerator job retains it until its completion fence signals.

### `CaptureStamp`

The current capture/ready timestamps are insufficient. Add:

```cpp
struct CaptureStamp {
    std::uint64_t pair_sequence{};
    std::uint64_t trigger_sequence{};
    std::int64_t sensor_timestamp_ns{};
    std::int64_t exposure_start_ns{};
    std::int64_t exposure_midpoint_ns{};
    std::int64_t monotonic_arrival_ns{};
    std::int64_t wall_time_ns{};
    std::int64_t pair_skew_ns{};
    ClockDomain clock_domain{};
};
```

Also include exposure duration, analog gain, frame counters, and whether the pair passed the synchronization gate.

### Asynchronous processing interface

A portable accelerator interface needs capabilities and jobs, not just `process()`:

```cpp
struct BackendCapabilities {
    std::vector<PixelFormat> input_formats;
    std::vector<MemoryDomain> input_domains;
    std::vector<MemoryDomain> output_domains;
    std::uint32_t max_width{};
    std::uint32_t max_height{};
    bool asynchronous{};
    bool produces_confidence{};
    bool produces_occlusion{};
};

class ProcessingJob {
public:
    virtual ~ProcessingJob() = default;
    virtual bool ready() const noexcept = 0;
    virtual JobStatus wait(std::chrono::nanoseconds timeout) = 0;
    virtual void cancel() noexcept = 0;
};

class StereoEstimator {
public:
    virtual ~StereoEstimator() = default;
    virtual BackendCapabilities capabilities() const = 0;
    virtual void prepare(const CalibrationBundle&, const StereoProfile&) = 0;
    virtual std::unique_ptr<ProcessingJob> submit(
        const StereoPair&, DisparityProduct&, Deadline) = 0;
    virtual void reset() = 0;
};
```

The same pattern should be used for rectification and semantic inference.

### `CalibrationStore`

Calibration must be selected by:

* Left and right module serial numbers.
* Lens and focus configuration.
* Resolution and crop.
* Physical orientation.
* Baseline assembly ID.
* Optional temperature band.
* Calibration-tool version.

The bundle should include a SHA-256 over all intrinsic, extrinsic, map-generation, and transform data. That hash becomes the published calibration identity.

### `IHardwareMonitor`

Each platform needs telemetry for:

* CPU/GPU/NPU utilization.
* Memory pressure.
* Temperature and throttling.
* Accelerator reset count.
* Camera/ISP errors.
* DMA import failures.
* Deadline misses.
* Power mode.

Without this, automatic fallback can silently turn into unsafe latency.

---

# 3. Per-platform acceleration strategy

## Platform matrix

| Platform              | Capture and ISP                               | Rectification                                          | Stereo                                                                        | Semantics                              | Recommended product role                |
| --------------------- | --------------------------------------------- | ------------------------------------------------------ | ----------------------------------------------------------------------------- | -------------------------------------- | --------------------------------------- |
| Pi 5 / CM5 + Hailo-8L | Native libcamera/PiSP, DMA-BUF luma and color | Existing EGL/GLES DMA-BUF path                         | CPU SGBM initially; learned Hailo stereo only after compile and quality gates | HailoRT async                          | First production/reference low-cost SKU |
| RK3588                | V4L2/media + RK ISP/RKAIQ                     | Mali Vulkan/OpenCL dense remap; RGA for resize/color   | CPU baseline; Vulkan/OpenCL SGM; gated RKNN learned stereo                    | RKNN NPU                               | Best next cost/performance SKU          |
| Milk-V SG200x         | CVI-MMF/VI/ISP/VPSS                           | Vendor GDC/LDC if capable; otherwise reduced CPU remap | Reduced-resolution fixed-point or very small learned model                    | Small TPU detector only                | Lite/sparse-depth SKU                   |
| Jetson                | Argus/NVMM or native V4L2 where appropriate   | VPI remap/CUDA; VIC for simple transforms              | VPI Stereo Disparity where supported; CUDA fallback                           | TensorRT, optional DLA where supported | Premium SKU and depth-quality ceiling   |

## Pi 5 / CM5

The current Pi path is conceptually correct:

* PiSP supplies full-resolution luma for stereo and a scheduled color stream for semantics/preview.
* The rectifier imports camera DMA-BUFs.
* CPU SGBM is isolated to one OpenCV thread.
* Semantics runs latest-only on Hailo.

The Pi 5/CM5 hardware exposes VideoCore VII with OpenGL ES 3.1 and Vulkan 1.3, two four-lane MIPI camera/display transceivers, and PCIe for the Hailo module. Hailo-8L is a 13-TOPS accelerator using PCIe Gen3 x2, and HailoRT exposes asynchronous inference APIs. ([Raspberry Pi][4])

Recommended implementation:

1. **Capture:** replace the Python/Picamera2 orchestration with native libcamera C++ while preserving PiSP’s luma-plus-color profile.
2. **Rectification:** keep the EGL/GLES DMA-BUF backend.
3. **Stereo v1:** retain half-resolution CPU SGBM as the production fallback and regression baseline.
4. **Stereo v2:** do not invest further in exact full SGM on VideoCore VII. Your previously supplied measurements already showed exact V3D SGM was far slower than the CPU baseline, while the hierarchical variant sacrificed too much density and tail accuracy.
5. **Hailo:** use it for semantics first. A Hailo stereo backend should not be declared available until the model produces a real HEF, fits the 8L, passes geometry and edge-quality tests, and coexists with both semantic networks.
6. **Scheduling:** one Hailo resource manager must schedule detector, segmentation, and any future stereo network. Three independent synchronous backends will create opaque contention.
7. **Pre/postprocessing:** keep luma work in PiSP/GPU and use NEON for unavoidable host work.

The YAML currently contains a `hailort` stereo profile, but the supplied source contains no corresponding Hailo stereo backend target or factory. It is currently configuration intent, not an implemented capability. 

For a plug-and-play product, prefer **CM5 on a custom carrier**. The retail Pi 5 documents its USB-C connector as power and its exposed data connectors as host USB ports. CM5 exposes the camera and I/O resources needed for a purpose-built carrier. An OAK-like upstream USB device interface must be designed explicitly; it does not emerge automatically from using a Pi 5 SBC. ([Raspberry Pi][5])

## RK3588 / ROCK 5B

The RK3588 is the most promising second platform. ROCK 5B exposes a Mali-G610MP4 supporting Vulkan and OpenCL, an NPU rated up to 6 TOPS, USB host/OTG ports, and one physical camera port that can represent one four-lane or two two-lane CSI interfaces. ROCK 5B+ exposes two camera ports and is mechanically easier for a stereo prototype. ([Radxa Docs][6])

Recommended pipeline:

1. **Capture:** V4L2/media-controller plus the Rockchip ISP/RKAIQ stack. Bind cameras by media entity and module identity, not `/dev/videoN`.
2. **Preprocessing:** use RGA for colorspace conversion, resize, crop, and letterbox.
3. **Rectification:** implement dense calibration-map remap on Mali through Vulkan compute, OpenCL, or a validated EGL/GLES path. Do not assume RGA’s ordinary transform operations are a substitute for arbitrary per-pixel stereo maps.
4. **Conventional stereo:** develop a Mali Census/SGM backend after the CPU baseline is working. Keep path count, maximum disparity, confidence, and occlusion output configurable.
5. **Semantics:** convert ONNX models to RKNN on an x86 build host and use the RKNN C runtime on the board.
6. **Learned stereo:** investigate only through a compiler proof. Require a report showing every heavy operator on the NPU, no catastrophic CPU fallback, zero-copy input, target latency, and target accuracy.
7. **Resource policy:** reserve the NPU primarily for semantics unless learned stereo demonstrably outperforms the GPU implementation without starving detection.

RKNN’s official deployment flow converts models on x86 and executes through C/C++ or Python on the target. The current toolkit documents zero-copy improvements, weight compression, SRAM use, and multi-core model execution on RK3588. ([Radxa Docs][7])

Use persistent DMA-BUF imports. Rockchip’s own RGA guidance recommends DMA-BUF over virtual addresses, warns about cache-synchronization overhead, and documents significant penalties or allocation failures when some RGA engines access memory above the 4 GB physical-address boundary. ([GitHub][8])

**Board risk:** ROCK 5B’s “two 2-lane cameras through one camera port” still requires the correct FPC breakout, power rails, I²C arrangement, device-tree endpoints, sensor drivers, and synchronized trigger wiring. Treat that as a hardware integration task, not merely a software switch.

## Milk-V SG2002 / SG2000 / CV1800B

This should be a separate product profile.

The existing full profile is unsuitable for direct transplantation:

* 728×544 float disparity.
* Four large result slots.
* Separate semantic ring.
* Full OpenCV.
* Two 5 Hz semantic models.
* BGR color buffers.
* JPEG previews.
* Dense float rectification maps.

On a 256 MB SG2002, the two fixed rings alone consume about 13.2 MiB before camera buffers, ISP pools, rectification maps, the executable, runtime libraries, TPU model memory, and reader copies.

Recommended Lite profile:

* 320×240, 480×360, or at most a proven 640-class input.
* `uint16_t` fixed-point disparity, not float32.
* Two or three slots sized from runtime profile.
* No full rectified mono copy unless requested.
* Sparse or compact obstacle output as the primary product.
* One small person/animal detector, not detector plus dense segmentation.
* Vendor ISP/VPSS for crop, resize, and YUV conversion.
* Vendor GDC/LDC mesh rectification only if the hardware API can represent the required stereo remap.
* No promise of dense point-cloud parity.
* Build without heavyweight desktop OpenCV where possible.

Milk-V’s TDL SDK provides integrated pre/postprocessing and a unified algorithm API, while its documented OpenCV-mobile integration can use VPSS for crop and YUV-to-BGR. That path currently calls out official GC2083 camera support, reinforcing that IMX296 requires new sensor integration. ([GitHub][9])

A learned stereo experiment is reasonable only after the hardware gate, but 0.5–1 TOPS and 256–512 MB mean it must be a purpose-designed tiny model. Do not start by trying to compile the same Pi/Jetson model.

Also verify that the vendor Buildroot toolchain supports the C++20 features used here—`std::span`, `std::bit_ceil`, and the required libstdc++ ABI—or provide a controlled cross-toolchain container.

## Jetson

Jetson is the cleanest path to a high-quality backend:

1. **Capture:** LibArgus/NVMM for integrated CSI sensors, once the IMX296 driver and carrier integration are complete.
2. **Preprocessing:** VIC for straightforward conversion/scaling and CUDA/VPI for GPU-resident operations.
3. **Rectification:** VPI Remap or CUDA texture sampling.
4. **Stereo:** VPI Stereo Disparity where supported by the specific Jetson/VPI combination; otherwise a CUDA SGM backend.
5. **Semantics:** TensorRT FP16 or INT8 with CUDA streams; DLA only on modules and networks that actually support it.
6. **Output:** retain NVMM/CUDA-resident data until a consumer truly needs host memory.

NVIDIA’s current VPI documentation includes stereo disparity, remap, and lens-distortion operations, while TensorRT is the supported optimizing inference runtime for GPU deployment. VPI capabilities vary by Jetson model and backend, so select at runtime rather than assuming every Jetson exposes the same hardware path. ([NVIDIA Docs][10])

NVIDIA’s Jetson multimedia documentation supports simultaneous CSI capture, NVMM buffers, CUDA-device memory, and accelerated conversion paths, while explicitly noting that hardware-to-software copies are CPU-expensive. That directly supports moving the common API away from CPU pointers. ([NVIDIA Docs][11])

Jetson should be your **reference quality implementation** even if it is not the lowest-cost SKU. It gives you a defensible target for edge quality, confidence, fill rate, and latency before deciding what compromises are acceptable on Pi and RK3588.

---

# 4. Competitive gap versus OAK-D and ZED

## Current position

Depth Daemon is ahead of a typical DIY prototype in internal architecture, but it is behind OAK and ZED as a product.

| Capability          | Depth Daemon now                            | Competitive requirement                                                         |
| ------------------- | ------------------------------------------- | ------------------------------------------------------------------------------- |
| Public SDK          | Native internals and Pi Python tools        | Stable C++, Python, and ROS APIs with discovery and version negotiation         |
| Appliance transport | Local MMAP and ROS topics                   | USB or Ethernet device protocol; one-cable discovery and control                |
| Calibration         | Rig-specific YAML plus hard-coded defaults  | Factory per-unit calibration, serial binding, EEPROM/storage, calibration query |
| Dense depth         | Float disparity only; metric depth disabled | Depth, disparity, confidence, validity, occlusion, aligned RGB-D                |
| Point cloud         | Compact obstacle cloud                      | Optional dense and colorized point cloud                                        |
| IMU                 | None in shown contract                      | Hardware-timestamped IMU and camera–IMU extrinsics                              |
| Temporal perception | None                                        | Object IDs, track age, velocity, hysteresis, stale-state handling               |
| Stereo controls     | One half-resolution SGBM profile            | Quality/performance profiles, LR checks, subpixel, filters, confidence          |
| Device management   | systemd and asset paths                     | Capability discovery, diagnostics, logs, updater, recovery, factory test        |
| Viewer              | None shown                                  | Calibration/depth/point-cloud/diagnostic viewer                                 |
| Manufacturing       | Reference calibration                       | Per-unit serials, calibration station, trigger test, depth-scale QA             |

## OAK-D gap

DepthAI exposes a Python API, configurable StereoDepth controls including subpixel, extended disparity and left-right checking, point-cloud/RGBD components, factory and user calibration retrieval from device EEPROM, and IMU nodes. OAK-D products expose USB 2/3 device connectivity and integrated IMUs on common models. ([Luxonis Docs][12])

The largest OAK advantages are not merely the stereo algorithm. They are:

* A coherent host SDK.
* Device enumeration.
* Pipeline construction.
* USB appliance behavior.
* Calibration tied to the unit.
* Stable depth metadata.
* Standard examples and visualization.
* A product-tested camera/IMU assembly.

Depth Daemon’s potential advantage is openness: interchangeable SBCs, interchangeable stereo algorithms, native Linux integration, user-selected sensors/baselines, and a robot-specific compact obstacle product. That advantage disappears if every platform becomes a fork.

## ZED gap

The ZED SDK provides metric dense depth, point clouds, confidence maps and thresholds, temporal depth stabilization, factory calibration, startup self-calibration, synchronized IMU access, and visual-inertial positional tracking. ([StereoLabs Docs][13])

ZED also demonstrates a deeper product principle: calibration is treated as a living system. Current documentation describes factory calibration, serial-specific calibration files, EEPROM-backed calibration on newer units, and startup refinement to compensate for mechanical and thermal changes. ([StereoLabs Docs][14])

You do not need to implement full VSLAM before launch, but you do need:

* Confidence and uncertainty.
* Temporal stabilization or at least temporal consistency metadata.
* Camera–IMU synchronization.
* Per-unit calibration management.
* Dense point-cloud support.
* A stable application-facing SDK.

## Semantic advantage and limitation

Your semantic abstraction is more flexible than a hard-wired detector, and recording model hashes is excellent. But the current box-only representation is insufficient for high-quality depth fusion. `SemanticObservation` provides a box and pixel count, not a mask; fusion samples an inner rectangle, which can select background, ground, or an occluding object.  

Add:

* Optional instance or region mask.
* Preprocessing transform metadata.
* Track ID and track age.
* Velocity.
* Depth-cluster selection inside the mask.
* Model-specific confidence threshold.
* Fusion-quality flags.
* Provenance for every fused object.

ZED’s object system, for comparison, exposes tracked objects, 3D positions, velocities, and optional masks. ([StereoLabs Docs][15])

---

# 5. Recommended architecture evolution

## A. Move from a call stack to a negotiated processing graph

```text
StereoCapture
    │
    ├── Sync / exposure / calibration QA
    │
    ▼
Rectification
    │
    ├──────────────► Scheduled color preparation
    │                         │
    ▼                         ▼
Stereo estimation      Semantic inference
    │                         │
    ▼                         ▼
Confidence / validity   Per-model temporal state
    │                         │
    ├──────────────┬──────────┘
    ▼              ▼
Metric depth   Semantic-depth fusion
    │              │
    ▼              ▼
Dense cloud   Hazard advisory
    │              │
    └─────── Products / data plane ───────┐
                                          ▼
                         ROS / Python / USB / network / recorder
```

Every node should declare supported:

* Pixel formats.
* Memory domains.
* Geometry limits.
* Synchronization behavior.
* In-place capability.
* Asynchronous capability.
* Produced confidence/uncertainty.
* Worst-case queueing policy.

At startup, the orchestrator negotiates a compatible path and records every required copy. “Zero-copy” should mean no hidden readback, not merely zero-copy capture.

## B. Create one native daemon executable

The executable should own:

* Configuration parsing and schema validation.
* Platform and backend discovery.
* Capture.
* Trigger control.
* Buffer pools.
* Processing graph.
* Resource scheduling.
* Local IPC.
* Control socket.
* Watchdog and recovery.
* Diagnostics.

Python should be a client SDK and development adapter, not the production camera orchestrator.

## C. Separate the internal C++ API from the plugin ABI

The virtual C++ classes are fine inside one build. They are not a durable third-party plugin ABI because they expose C++ exceptions, STL types, and compiler ABI details.

For independently packaged platform plugins, use a versioned C function table with opaque handles:

```cpp
extern "C" const depthd_plugin_v2* depthd_get_plugin_v2();
```

The implementation can immediately adapt that C table into the internal C++ interface.

## D. Redesign the depth product contract

A disparity product needs at least:

```cpp
struct DisparityProduct {
    ImageBuffer disparity;
    std::optional<ImageBuffer> confidence;
    std::optional<ImageBuffer> occlusion;

    DisparityFormat format;
    float value_scale;
    float invalid_value;

    ImageRegion valid_region;
    float min_disparity;
    float max_disparity;

    CalibrationRef calibration;
    CaptureStamp stamp;
    BackendRunInfo backend;
};
```

Supported formats should include:

* FP32.
* FP16.
* U16 or S16 fixed-point.
* Backend-native format where explicitly negotiated.

Metric depth should be a separate derived product, not silently inferred from a global focal length and baseline.

## E. Introduce IPC v2

Use:

1. A small immutable schema header.
2. A cache-line-separated atomic control page.
3. Per-slot product descriptors.
4. Runtime geometry and capacity.
5. Offsets and sizes for each product.
6. Monotonic and wall timestamps.
7. Calibration and backend hashes.
8. Optional eventfd or futex notification.
9. Read-only consumer mappings.
10. `0640` or `0600` permissions with a dedicated group.

For large GPU-resident products, the ring should carry metadata and a transferable buffer handle rather than forcing a host copy. Keep the current v1 reader for compatibility during migration.

## F. Make calibration a first-class subsystem

Required workflow:

1. Discover module serials.
2. Resolve calibration bundle.
3. Verify signatures/hashes.
4. Generate backend-specific maps.
5. Run startup epipolar validation.
6. Reject reversed or mismatched cameras.
7. Monitor vertical residual during operation.
8. Publish calibration hash with every frame.
9. Store calibration in per-unit non-volatile storage.
10. Support controlled recalibration and rollback.

Factory acceptance should measure:

* Epipolar vertical residual.
* Baseline and orientation.
* Depth scale at several distances.
* Left/right exposure matching.
* Trigger skew.
* Lens focus.
* Thermal drift.

## G. Redesign semantic safety state

Maintain one record per model:

```text
model_id
last_attempt_sequence
last_success_sequence
last_success_monotonic_ns
result_expiry_ns
health
current_objects
required_for_clear
```

The hazard state machine should:

* Latch positives for a configured hold interval.
* Require multiple fresh clear observations before release.
* Refuse to clear while any required model is expired.
* Use model confidence thresholds.
* Use a physical stop region for unranged detections.
* Rank hazards by class, range, time-to-contact, and confidence.
* Distinguish “hazard detected” from “perception unavailable.”
* Report why fail-closed was entered.

This subsystem should remain one layer of robot safety, not the sole hardware E-stop mechanism.

## H. Add a resource scheduler

Accelerators are shared resources. The scheduler needs:

* Deadline and cadence.
* Latest-only replacement.
* Priority.
* Accelerator affinity.
* Maximum concurrent jobs.
* Memory budget.
* Admission control.
* Degradation profile.
* Warm-up and reset handling.

Example priority:

1. Capture and synchronization.
2. Stereo depth.
3. Human/animal detector.
4. Obstacle product.
5. Segmentation.
6. Preview/JPEG.
7. Recording/debug products.

The scheduler should be able to disable preview or segmentation before missing depth and detector deadlines.

## I. Package models as multi-target artifacts

One logical model package should contain:

```text
model-manifest.json
source/model.onnx
hailo/model.hef
rknn/model.rknn
tpu/model.cvimodel
tensorrt/<platform>/model.engine
labels.json
preprocess.json
postprocess.json
golden_inputs/
golden_outputs/
```

Do not assume compiled artifacts are portable across runtime, driver, accelerator, or board generations. The daemon should verify target compatibility before loading.

## J. Separate transport from products

Implement adapters over the same product API:

* Local MMAP.
* ROS 2.
* Python SDK.
* GStreamer.
* Recorder/replay.
* Ethernet.
* USB device transport.

For an OAK-like USB product, use a composite design:

* UVC for preview where useful.
* Vendor bulk or USB-Ethernet for disparity/depth/objects.
* A control protocol for profiles, calibration, diagnostics, and updates.

UVC alone is not an adequate depth-camera protocol.

---

# 6. Priority roadmap

## P0 — Correctness and safety blockers

1. Fix per-model temporal state and unsafe clear behavior.
2. Add monotonic freshness and end-to-end deadline accounting.
3. Separate hazard detected, perception stale, and perception fault.
4. Replace global calibration constants with a runtime calibration bundle.
5. Publish actual disparity scale, valid ROI, search range, and backend identity.
6. Bind calibration to camera serials and fail startup on mismatch.
7. Fix ring publication sequencing and require lock-free 64-bit atomics.
8. Change ring permissions and harden file creation.
9. Add clock-step, camera-restart, cadence-divergence, and stale-result tests.
10. Label the current semantic hazard result advisory-only.

**Exit gate:** no stale or non-due model can produce a clear; all published geometry is derived from the active calibration and backend.

## P1 — Portable execution foundation

1. Add `ICaptureBackend`, `ITriggerBackend`, and native paired-frame metadata.
2. Add type-erased buffer storage and shared leases.
3. Convert rectification, stereo, and semantics to asynchronous jobs.
4. Add backend capability negotiation.
5. Add the central scheduler and deadline-aware degradation.
6. Create the native `depthd` executable.
7. Split core and platform packages.
8. Add CMake presets/toolchains for Pi, RK3588, Jetson, and SG200x.
9. Introduce IPC v2 while retaining a v1 compatibility adapter.
10. Create replay-based backend conformance tests.

**Exit gate:** the same recorded stereo sequence can run through CPU and at least one hardware backend without modifying application code.

## P2 — Productionize the Pi implementation

1. Native libcamera/PiSP capture.
2. Hardware-timestamped pair and trigger metadata.
3. Existing EGL DMA-BUF rectification with asynchronous output storage.
4. CPU SGBM confidence/edge repair or a better validated conventional backend.
5. HailoRT asynchronous semantic scheduling.
6. Runtime thermal, power, and deadline diagnostics.
7. Metric depth and confidence products.
8. Per-unit calibration tooling and persistent serial identity.
9. Long-duration camera restart, NPU reset, and thermal-throttle testing.
10. CM5 carrier decision for the commercial enclosure and transport.

**Exit gate:** deterministic operation through camera restarts and accelerator faults, with no silent CPU fallback that violates the profile deadline.

## P3 — Jetson reference and RK3588 cost port in parallel

### Jetson track

* Argus/NVMM capture.
* VPI/CUDA rectification.
* VPI or CUDA stereo with confidence.
* TensorRT semantics.
* Establish the depth-quality ceiling and canonical output metrics.

### RK3588 track

* V4L2/RKAIQ capture.
* RGA preprocessing.
* Mali dense remap.
* RKNN semantics.
* GPU SGM baseline.
* Compile-gated learned stereo experiment.

**Exit gate:** cross-platform output contracts remain identical, while backend-specific quality and latency are explicitly reported.

## P4 — Competitive product layer

1. Python SDK with zero-copy NumPy access where local.
2. Device discovery and capability query.
3. ROS 2 component and launch packages.
4. Dense and colorized point cloud.
5. Hardware-timestamped IMU and calibrated camera–IMU transform.
6. Viewer for depth, confidence, point cloud, calibration, and diagnostics.
7. Recorder/replay format with images, IMU, calibration, and model metadata.
8. Signed configuration/model/calibration manifests.
9. A/B updater and rollback.
10. Ethernet and USB device transports.
11. Factory calibration and end-of-line tester.
12. Enclosure, thermal, EMC, trigger, and vibration validation.

## P5 — Milk-V Lite go/no-go

Before software porting:

1. Confirm two IMX296 sensors can be electrically attached.
2. Confirm simultaneous capture through separate CSI receivers.
3. Bring up both sensor drivers and ISP tuning.
4. Verify shared external trigger.
5. Measure buffer-pool memory.
6. Measure CPU and TPU headroom.
7. Confirm a modern C++ toolchain.
8. Prove one viable low-resolution stereo method.

Only then implement the reduced fixed-point contract and compact obstacle product.

---

# Recommended product segmentation

## Depth Daemon Core

Common open-source C++ implementation:

* Contracts.
* Scheduler.
* Calibration.
* Obstacle cloud.
* Semantic fusion.
* Rings/control protocol.
* Recorder/replay.
* SDKs.

## Depth Camera Pi

* CM5/Pi 5.
* PiSP.
* EGL rectification.
* CPU or validated Hailo stereo.
* Hailo semantics.

## Depth Camera RK

* RK3588.
* Rockchip ISP/RGA.
* Mali stereo/rectification.
* RKNN semantics.

## Depth Camera Jetson

* CUDA/VPI/TensorRT.
* Highest depth quality and performance.
* Reference and premium product.

## Depth Camera Lite

* SG200x-class custom hardware.
* Lower resolution.
* Fixed-point disparity.
* Sparse/compact obstacle output.
* Small detector.
* No claim of ZED/OAK parity.

---

# Final assessment

The architecture has a **genuinely strong reusable nucleus**. In particular, keep the rectification registry, DMA-BUF cache/lifecycle policy, latest-only handoff, model provenance, compact obstacle cloud, restart-aware ring concept, and target-level build decomposition.

The major work is not “write four stereo backends.” It is to build the missing portable substrate:

* Native paired capture.
* Stable sensor and calibration identity.
* Type-erased accelerator memory.
* Asynchronous jobs and fences.
* Deadline/resource scheduling.
* Runtime depth metadata.
* Temporally correct safety semantics.
* Dynamic, hardened IPC.
* Product-facing SDK and transport.

The most dangerous mistake would be to create four platform forks around the current fixed 728×544 CPU-pointer contract. The correct evolution is one negotiated graph with platform plugins and a richer product contract.

**Ship order:** Pi/CM5 first, establish the quality ceiling on Jetson, make RK3588 the primary cost-optimized port, and treat Milk-V as a separately scoped Lite device only after dual-IMX296 hardware qualification.

[1]: https://milkv.io/docs/duo/getting-started/duo256m "https://milkv.io/docs/duo/getting-started/duo256m"
[2]: https://milkv.io/docs/duo/resources/opencv-mobile "https://milkv.io/docs/duo/resources/opencv-mobile"
[3]: https://github.com/hailo-ai/hailort/blob/master/hailort/hailo_firmware_eula "https://github.com/hailo-ai/hailort/blob/master/hailort/hailo_firmware_eula"
[4]: https://www.raspberrypi.com/news/compute-module-5-on-sale-now/ "https://www.raspberrypi.com/news/compute-module-5-on-sale-now/"
[5]: https://www.raspberrypi.com/documentation/computers/raspberry-pi.html "https://www.raspberrypi.com/documentation/computers/raspberry-pi.html"
[6]: https://docs.radxa.com/en/rock5/rock5b/getting-started/introduction "https://docs.radxa.com/en/rock5/rock5b/getting-started/introduction"
[7]: https://docs.radxa.com/en/rock5/rock5t/app-development/ai/rknn-install "https://docs.radxa.com/en/rock5/rock5t/app-development/ai/rknn-install"
[8]: https://github.com/airockchip/librga/blob/main/docs/Rockchip_FAQ_RGA_EN.md "https://github.com/airockchip/librga/blob/main/docs/Rockchip_FAQ_RGA_EN.md"
[9]: https://github.com/milkv-duo/duo-tdl-examples "https://github.com/milkv-duo/duo-tdl-examples"
[10]: https://docs.nvidia.com/vpi/index.html "https://docs.nvidia.com/vpi/index.html"
[11]: https://docs.nvidia.com/jetson/archives/r36.4.4/DeveloperGuide/SD/Multimedia/AcceleratedGstreamer.html "https://docs.nvidia.com/jetson/archives/r36.4.4/DeveloperGuide/SD/Multimedia/AcceleratedGstreamer.html"
[12]: https://docs.luxonis.com/software-v3/depthai/api/python "https://docs.luxonis.com/software-v3/depthai/api/python"
[13]: https://docs.stereolabs.com/docs/development/zed-sdk/modules/depth-sensing "https://docs.stereolabs.com/docs/development/zed-sdk/modules/depth-sensing"
[14]: https://docs.stereolabs.com/docs/development/zed-sdk/modules/camera/camera-calibration "https://docs.stereolabs.com/docs/development/zed-sdk/modules/camera/camera-calibration"
[15]: https://docs.stereolabs.com/docs/development/zed-sdk/modules/object-detection "https://docs.stereolabs.com/docs/development/zed-sdk/modules/object-detection"
