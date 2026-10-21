# Nuvo Depth repository and stereo image-pipeline map

Source audit updated: 2026-10-09. These diagrams describe the **arm64 native Pi 5 build and the code paths actually called by its services**. Solid arrows are active data/control paths; dashed arrows are conditional, alternative, or disconnected paths as labeled. The checked-in service uses 728 × 544 YUV420 capture at 10 Hz with the provisional 100 mm seed unless an active calibration bundle supersedes it. Both daemon YAML and `stereo.json` select 3-way SGBM with 128 disparities and two OpenCV threads. `encoding.json` defaults to raw-only; processing and delivery occur only for requested products. A 60 Hz experimental profile and half-resolution benchmark profile exist but are not the installed default.

## 1. Physical capture to associated depth products

```mermaid
flowchart LR
  subgraph Startup["Startup and configuration"]
    SYS["systemd nuvo-depth target"] --> DAEMON["depthd native daemon"]
    YAML["daemon.yaml: rig, capture, geometry, fallback"] --> DAEMON
    ID["provisioned device UUID"] --> DAEMON
    CAL["factory active, else user active, else provisional seed"] --> MAPS["validate hashes, identity and authority; prepare OpenCV maps, P and Q"]
    EXP["exposure.json: manual or shared auto"] --> AE["exposure controller"]
    ST["stereo.json: SGBM or StereoBM settings"] --> MATCH
  end

  subgraph Cameras["Pi 5 cameras and synchronization"]
    AE --> CTRL["shared exposure, analogue and colour-gain command"]
    CTRL --> XTR["RP1 PIO PWM on GPIO17; hardware-timed low pulse is exposure"]
    CTRL --> LC["libcamera left request controls; AE and AWB off"]
    CTRL --> RC["libcamera right request controls; AE and AWB off"]
    XTR --> L["logical-left IMX296"]
    XTR --> R["logical-right IMX296"]
    L --> LC --> ISP_L["Pi ISP: YUV420, luma DMA-BUF and metadata"]
    R --> RC --> ISP_R["Pi ISP: YUV420, luma DMA-BUF and metadata"]
    ISP_L --> PAIR["timestamp and identity pairer: monotonic epoch, tolerance, newest pair"]
    ISP_R --> PAIR
  end

  subgraph Processing["One native processing worker; owned outputs"]
    PAIR --> REQ["leased stereo processing request"]
    REQ -. "selected GPU backend" .-> GPU["EGL import of both R8 DMA-BUF luma planes; GLES3 map shader; readback"]
    REQ --> CPU["bounded persistent mmap; per-capture DMA sync; cached CPU remap"]
    MAPS --> GPU
    MAPS --> CPU
    GPU --> RECT["owned left and right rectified Gray8 images"]
    CPU --> RECT
    RECT --> RELEASE["release mappings and camera leases; recycle requests"]
    RELEASE --> LC
    RELEASE --> RC
    RECT --> PRE["copy or area-resize to matcher grid; grayscale if needed"]
    PRE --> MATCH["OpenCV CPU SGBM: 3-way, 5-path, HH4, HH; or StereoBM"]
    MATCH --> FIXED["signed 16-bit disparity at 1/16 px"]
    FIXED --> POST["optional median; ROI, range and bright-pixel validity; NaN invalid; binary confidence"]
    POST --> DISP["native-grid float32 disparity, uint8 confidence and validity"]
    DISP -. "only depth or metric validity requested" .-> DEPTH["Q reprojection; depth bounds and validity codes"]
    DEPTH --> SET["immutable associated FrameSet: only requested products, metadata and timings"]
    RECT --> SET
    DISP --> SET
    MAPS --> SET
  end

  REQ -. "only scheduled auto observations: map both luma planes; meter, settle and confirm" .-> AE
  REQ -. "advanced calibration capture: synchronized unrectified ISP luma pair" .-> CALRAW["PNG pair and sensor/exposure metadata"]
  REQ -. "optional flicker trace" .-> ISPTRACE["ISP PNG pair and metadata"]
  SET -. "optional flicker trace" .-> DEPTH_TRACE["post-depth frame trace"]
```

Key details: libcamera configures **one VideoRecording stream per camera** in this backend; the main format is YUV420. The selected colour-preview side is copied from that ISP YUV420 buffer before its lease is released. The `left_color_stream: lores` / `RGB888` descriptors in the base YAML do not create a separate lores stream in the native capture code. GPU acceleration covers rectification only: matching, filtering, depth conversion, and FFmpeg encoding are CPU work. Cached CPU rectification is the measured default; GPU remains selectable. Raw image-only demand bypasses matching and metric conversion, and unrectified image demand bypasses rectification. Mono/RGB conversion and each camera remap are performed only when required by requested outputs. The scheduler has one replaceable pending request and one latest completed result; the pairer retains at most one unmatched frame per side.

The matcher works at the configured working grid. The installed profile is full-resolution 728 × 544; a separate half-resolution benchmark profile exists. Invalid disparity remains invalid through median filtering, and metric depth is computed from the matching-grid calibration rather than an image-only scale factor.

## 2. Demand, delivery and independent previews

```mermaid
flowchart LR
  CLIENT["authenticated raw and preview consumers"] --> GW["TLS gateway: actual receiving NIC and bounded admission"]
  GW --> DEMAND["persistent local subscriptions: union products and maximum cadence"]
  DEMAND --> CAPTURE["associated camera captures; cadence admission"]
  CAPTURE --> RAW["processing worker: only requested raw products and dependencies"]
  RAW --> LOCAL["immutable owning frame; sealed memfd serialized directly"]
  LOCAL --> GW
  CAPTURE --> PREVIEW["admitted image copied once into bounded preview pool"]
  PREVIEW --> TRANSFORM["private worker rectification, RGB conversion or stereo stitching as needed"]
  TRANSFORM --> CODEC["FFmpeg JPEG or software H.264"]
  CODEC --> NVE["independent NVE1 preview and configuration metadata"]
  NVE --> GW
  RAW -. "only disparity/depth visualization preview" .-> CODEC
  CAPTURE --> STATUS["fresh capture and demand status; separate processing sample identity"]
  RAW --> STATUS
  CODEC --> STATUS
  GW --> SDK["SDK, recorder and browser: selected products only"]
```

Raw and preview consumers hold separate persistent subscriptions. No active
consumer means no raw processing, serialization or encoding; capture stays
prepared for fresh status, exposure and calibration requests. Disconnect removes
the consumer's demand. First-frame requests wait for matching selected products
within a bounded deadline. Images are admitted for preview independently of SGBM,
so a slow matcher cannot limit an image-only monitor to the depth frame rate.

Supported raw selections are each camera's unrectified mono/RGB and rectified
mono/RGB, rectified stereo side-by-side mono/RGB, disparity, depth, confidence and
validity. Legacy six-product subscriptions retain their original wire selections.
The gateway shares immutable fetched bytes and copies only selected subsets for
consumers. Atomic raw snapshots are an explicit diagnostic mode; normal delivery
uses the local socket rather than continuously writing full raw frames to disk.

The actual socket's receiving interface selects WiFi, Ethernet or USB Ethernet
policy. WiFi uses bounded best-effort delivery and an explicit byte budget;
qualified exact delivery requires matching recorded wired-link evidence. The
browser shows estimated product bandwidth and lets users choose products and
cadence. It never silently changes requested products or formats. Encoded depth
is a visualization; raw float depth retains the metric data.

## 3. Settings, exposure feedback and advanced calibration

```mermaid
flowchart LR
  UI["web UI or SDK"] --> AUTH["gateway claim token, session cookie, CSRF and API validation"]
  CLI["nuvoctl on device"] --> FILES
  AUTH --> AGENT["root socket-activated nuvo-system-agent allowlist"]
  AGENT --> NCTL["nuvoctl validated atomic writes"]
  NCTL --> FILES["encoding.json, stereo.json, exposure.json, transport.json"]
  FILES --> ENC["encoder refreshes mode, source, codec and cadence"]
  FILES --> MATCH["matcher refreshes algorithm, range and filters"]
  FILES --> AE["exposure controller refreshes manual or shared auto command"]
  AE --> CAM["both camera requests plus trigger pulse width"]
  CAM --> ISP["paired ISP luma and applied exposure/gain metadata"]
  ISP -. "auto mode only" .-> AE
  FILES --> POLICY["gateway transport preference and 60 s rollback watchdog"]

  UI --> PREVIEW["calibration positioning via optional encoded ISP colour preview"]
  PREVIEW --> AUTH
  UI --> CALAPI["create, capture, solve, activate or rollback calibration session"]
  CALAPI --> AUTH
  AGENT --> CALTOOL["nuvo-calibration web workflow"]
  CALTOOL --> REQUEST["write one capture request in /run/nuvo-depth/calibration/requests"]
  REQUEST --> DAEMON["depthd consumes request at next paired capture"]
  DAEMON --> RAW["both full-profile unrectified ISP luma PNGs and metadata"]
  RAW --> QUAL["ChArUco detection, shared corners, blur, clipping, pose uniqueness and coverage"]
  QUAL --> ANNOT["annotated accept or retake preview JPEGs"]
  ANNOT --> CALAPI
  QUAL --> SOLVE["camera intrinsics, robust outlier rejection, stereo solve, rectification"]
  SOLVE --> GATE["training and holdout RMS, vertical epipolar p95, measured baseline gates"]
  GATE --> BUNDLE["hashed YAML pair, validation.json and manifest; user preset"]
  BUNDLE --> ACT["atomic activate or rollback; systemd restarts depthd"]
  ACT --> MAPS["startup verifies authority, unit, camera IDs and geometry; prepares maps and Q"]
```

The positioning preview is encoded; solver input is **not**. The daemon captures a synchronized raw ISP luma pair, and the calibration tool accepts/rejects it and solves from the saved raw PNGs. A passing web solve yields `user_validated` authority; factory activation has a separate signed trust path. Activation restarts depth processing so the new maps and Q are prepared together. Manual exposure uses one fixed exposure/gain command; shared auto samples both luma planes, observes clipping and median deadband, waits for confirmation/settling, then updates both requests and the XTR pulse.

## 4. What else lives in this repository

| Area | Role and production status |
| --- | --- |
| `apps/`, `src/core/`, `platform/pi5/`, `platform/linux/`, `include/depthd/` | Native Pi capture, pairing, processing, encoder, IPC and gateway described above. |
| `web/` | Claimed browser UI, raw canvas viewer, encoded monitor, exposure/matcher/transport editors, advanced calibration UI. Waiting/disconnected states show no photographic substitutes for live products. |
| `sdk/python/`, `include/nuvo/`, `src/sdk_c.cpp` | TLS snapshot/configuration clients, incremental C NUV1 decoder, Python NUV1/NVE1 decoders, NVR1 recording/replay. The local socket client helper is C++, not a Python SDK transport. |
| `tools/calibration/`, `src/calibration_store.cpp`, `config/calibration/` | ChArUco/chessboard solving, bundle validation/activation and startup rectification-map preparation. Provisional seed and sample matrix coexist with per-unit active bundles. |
| `tools/nuvoctl.py`, `tools/nuvo_system_agent.py`, `deployment/`, `debian/`, `qualification/`, `config/pi/` | Provisioning, boot overlay/trigger preparation, privileged controls, systemd, APT packaging and qualification gates. No package install auto-enables the camera target. |
| `CMakeLists.txt`, `CMakePresets.json`, `tools/release/` | Build variants and feature gates, source/SPDX generation, and vendor APT staging. The arm64 production build requires Pi capture, both CPU and GPU rectification support, OpenCV, FFmpeg, gateway and systemd. |
| `tools/pi/`, `tests/`, `tools/*benchmark*`, `docs/`, `presets/` | HIL checks, CPU/GPU/encoder profiling, flicker traces, unit/integration tests, validation reports, reference presets and operating documentation. These are not extra production frame stages. |
| `protocol/nuvo_stream_v1.proto`, `src/gateway.cpp` | Protocol schema and tested transport-admission core. The shipping gateway uses JSON wire metadata and persistent demand-driven local IPC, actual receiving-interface detection and bounded admission. |
| `src/depth_conversion.cpp` | Runtime Q-based float-metre depth conversion plus a separate optional float-depth to uint16-millimetre quantizer; the native frame set publishes float metres, not the quantized variant. |

## Source-verified boundaries

- Software FFmpeg JPEG/H.264 encoding consumes CPU; the Pi5 has no hardware
  video encoder in this path. Preview copies and transforms run only at admitted
  cadence and while a consumer needs them.
- The native backend creates one YUV420 main stream per camera. Legacy YAML
  lores/ROS/cloud descriptors do not create extra native processing stages.
  Legacy obstacle-cloud/result-ring processing has been retired.
- RP1 PIO PWM eliminates userspace pulse scheduling on the current GPIO17 wiring.
  Timing status distinguishes hardware commands from measurements. The kernel
  driver holds its last pin level when disabled; both cameras stop first.
- The checked-in calibration remains provisional. Output authority is preserved
  in raw and encoded metadata; throughput measurements do not establish metric
  accuracy for the final mechanical rig.

## Code anchors

- Capture/trigger/ISP and leases: [`platform/pi5/capture_libcamera.cpp`](../platform/pi5/capture_libcamera.cpp), [`platform/linux/capture_mapping.cpp`](../platform/linux/capture_mapping.cpp), [`src/core/pairer.cpp`](../src/core/pairer.cpp).
- Calibration/map preparation and runtime factory: [`src/calibration_store.cpp`](../src/calibration_store.cpp), [`src/core/runtime_factory.cpp`](../src/core/runtime_factory.cpp).
- Rectification, matching and depth: [`src/egl_gl_dmabuf_rectification_backend.cpp`](../src/egl_gl_dmabuf_rectification_backend.cpp), [`src/egl_gl_dmabuf_platform.cpp`](../src/egl_gl_dmabuf_platform.cpp), [`src/opencv_rectification_backend.cpp`](../src/opencv_rectification_backend.cpp), [`src/cpu_sgbm_backend.cpp`](../src/cpu_sgbm_backend.cpp), [`src/depth_conversion.cpp`](../src/depth_conversion.cpp), [`src/core/runtime.cpp`](../src/core/runtime.cpp).
- Publication/encoding/delivery: [`apps/depthd_main.cpp`](../apps/depthd_main.cpp), [`src/ipc_v2.cpp`](../src/ipc_v2.cpp), [`platform/linux/local_frame_server.cpp`](../platform/linux/local_frame_server.cpp), [`src/frame_wire.cpp`](../src/frame_wire.cpp), [`src/image_encoder.cpp`](../src/image_encoder.cpp), [`apps/nuvo_gateway_main.cpp`](../apps/nuvo_gateway_main.cpp), [`web/app.js`](../web/app.js).
- Controls/calibration/deployment: [`src/exposure_control.cpp`](../src/exposure_control.cpp), [`tools/nuvo_system_agent.py`](../tools/nuvo_system_agent.py), [`tools/calibration/nuvo_calibration.py`](../tools/calibration/nuvo_calibration.py), [`deployment/systemd/nuvo-depth.target`](../deployment/systemd/nuvo-depth.target), [`debian/rules`](../debian/rules).
