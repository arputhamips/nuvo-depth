# Oracle-guided Raspberry Pi 5 optimization report

Date: 2026-09-10  
Target: Raspberry Pi 5, two IMX296 cameras, native capture/rectification/depth pipeline  
Scope: capture, rectification, disparity/depth, bounded local publication, browser inspection. USB gadget and Ethernet transport changes were intentionally excluded.

## Oracle review

Oracle was updated from 0.18.0 to the latest published release, v0.20.0. Its install, build, and typecheck passed. The full Oracle test run passed 2,316 tests and skipped 53; six tests failed only because Windows denied test-created symbolic links with `EPERM`.

The review used Oracle's browser-search path with the requested `gpt-6-pro` alias. Oracle verified that the browser selected ChatGPT **Latest** with the **Pro** reasoning tier before submitting the prompt. The input bundle contained 191 first-party files (about 531,638 tokens / 1.6 MB), covering build files, all native source and headers, Pi platform code, applications, configuration, presets, web UI, benchmarks, tests, performance tools, and hardware-qualification material.

The full Oracle output is `C:\Users\Workstation\Documents\Oracle\nuvo-depth-pi5-fullcpu-optimization-oracle-20260910.md`.

## Recommendation-by-recommendation disposition

| # | Oracle recommendation | Result |
|---:|---|---|
| 1 | Shutdown-safe capture resource lifetime | Implemented. Scheduler destruction now precedes capture teardown, and processing-request move assignment drops mappings and views before the camera lease. A sentinel lifetime test covers the order. |
| 2 | Ownership-safe completion and DMA-BUF error paths | Implemented. Completion is validated before publication, requests are recycled on recoverable paths, DMA sync retries transient failures, START failures are fatal to that mapping, END failures are counted, and every mapping path has explicit ownership. |
| 3 | Apply pair tolerance and budget pending frames | Implemented. Runtime pair tolerance is applied and the ready-pair queue is a one-pair freshness slot. Capture reserves are no longer silently consumed by an unbounded pending queue. |
| 4 | Bounded overload/freshness policy | Implemented. The processing scheduler replaces the oldest pending item with the newest frame and reports replacements instead of forcing a restart. Tests exercise an in-flight frame plus pending replacement. |
| 5 | Make native configuration match applied capture controls | Implemented. Native capture now explicitly applies fixed exposure, analogue gain, shared colour gains, disabled AE/AWB, and the requested colour space. The production profile is locked to the effective values. |
| 6 | Prevent trigger catch-up bursts | Implemented in software. Missed deadlines are skipped rather than emitted in a burst; attempts, failures, and deadline overruns are reported. Electrical exposure verification with an oscilloscope remains a hardware qualification action. |
| 7 | Bind metric depth to per-unit calibration/range | Deferred pending physical calibration targets and metrology. The UI continues to mark the supplied authority-0 calibration as **Engineering only**, so provisional depth cannot be mistaken for qualified metric output. |
| 8 | Normalize disparity units and exclude unsupported regions | Partially implemented. Full- and working-resolution disparity values are named separately and scaled exactly once; validity uses OpenCV's exact disparity ROI in both axes. Rectification-map support masks still need calibration-specific qualification. |
| 9 | Explicit product semantics and browser freshness | Partially implemented. Diagnostics now expose disparity/depth validity and processing stages; the viewer keys frames by run, capture epoch, and sequence, renders disparity, reports polling errors, and visibly marks frames stale after two seconds. A future wire-schema revision can make every coordinate convention fully self-describing. |
| 10 | Bound local IPC and close descriptors on every error | Implemented. Request/response timeouts are bounded, all received descriptors enter RAII ownership before validation, and a stalled-client server-stop test passes. |
| 11 | Measure the complete native path | Implemented for this target. Native per-stage timings, validity, publication size, capture errors, scheduler statistics, and DMA mapping statistics are emitted. Sustained hardware results are recorded below; the matcher microbenchmark supplies p50/p95/p99 scaling data. |
| 12 | Benchmark OpenCV with 1/2/3/4 threads | Implemented and measured on the Pi. Four threads is the production choice. |
| 13 | Release camera requests after rectification; lazy CPU mapping | Implemented. A successful EGL/DMABUF path performs zero CPU DMA mappings. CPU mapping is acquired only when a CPU rectifier or GPU fallback actually needs it. |
| 14 | Pool native frame storage and scratch arrays | Deferred. The measured pipeline is already comfortably inside the 10 Hz budget; a pool should be introduced with allocator instrumentation rather than broad lifetime risk. |
| 15 | Remove redundant Gray8 and classified-product copies | Implemented. OpenCV resize consumes strided Gray8 views directly, and native classification writes directly into the destination products while counting validity. |
| 16 | Optimize first-party depth loops | Implemented. The common horizontal-Q path uses a constant numerator and linear denominator with row-pointer traversal; the general-Q path and exact invalid/NaN/range semantics remain. |
| 17 | Retain both rectifiers; optimize only a measured boundary | Implemented as a decision guard. Both backends remain, EGL/DMABUF is selected on the Pi, fallback is explicit, and rectification measures 4.37 ms. No speculative GPU readback rewrite was made. |
| 18 | Move publication off the critical path | Deferred. Local publication is bounded and the sustained run had no drops; asynchronous publication remains the next step if additional consumers create measured pressure. |
| 19 | Tune SGBM geometry/quality after overhead isolation | Deferred pending calibrated ground truth. Changing matcher quality without a target set would trade accuracy blindly. |
| 20 | Optional native auto-exposure | Intentionally deferred. The qualified profile uses fixed, shared stereo exposure/gain controls and explicitly disables AE/AWB. |
| 21 | Qualify independent higher-rate/detail presets | Partially complete. The balanced 728×544 → 364×272 Pi profile is tested; alternate presets still need separate thermal, accuracy, and sustained-run qualification. |

## Implemented code areas

- Capture and ownership: `include/depthd/capture.hpp`, `include/depthd/runtime.hpp`, `platform/pi5/capture_libcamera.cpp`, `platform/linux/capture_mapping.cpp`, `src/core/pairer.cpp`, `src/core/runtime.cpp`.
- Rectification and stereo: `include/depthd/rectification_backend.hpp`, `include/depthd/stereo_backend.hpp`, `src/egl_gl_dmabuf_rectification_backend.cpp`, `src/cpu_sgbm_backend.cpp`, `src/core/runtime_factory.cpp`.
- Depth and publication: `src/depth_conversion.cpp`, `include/depthd/local_ipc.hpp`, `platform/linux/local_ipc.cpp`, `platform/linux/local_frame_server.cpp`.
- Diagnostics and artifacts: `apps/depthd_main.cpp`, `tools/sgbm_benchmark.cpp`.
- Production profile and locks: `config/nuvo_pi5_depth_100mm.yaml`, its fixture/asset manifest, and profile/coherence tests.
- Viewer: `web/index.html` and `web/app.js`.
- Regression coverage: runtime, pairing, SGBM, rectification fallback/laziness, IPC, configuration, and production-profile tests.

## Raspberry Pi 5 results

### Correctness and sustained capture

- Native release build: passed.
- Pi native test suite: **39/39 passed**.
- Host development test suite: **38/38 passed**.
- Capture-only run: **100/100 pairs**, maximum sensor timestamp delta **21 μs**, zero pair rejections/drops, zero trigger failures, zero requeue failures, and zero outstanding leases.
- Sustained full-stack run: **1,000 frames at 10 Hz** with zero capture sequence gaps, frame-completion errors, trigger errors, trigger deadline overruns, scheduler failures, completed-result drops, requeue failures, and outstanding leases.
- The ready-pair freshness slot replaced three startup pairs; the processing scheduler replaced none and processed all 1,000 accepted frames.
- The EGL/DMABUF success path started **zero CPU DMA mappings**.
- End-of-run SoC temperature: **50.5 °C**; firmware throttle state: **0x0**.

### Native final-frame timing

| Stage | Time |
|---|---:|
| EGL/DMABUF rectification | 4.375 ms |
| SGBM preprocess | 0.159 ms |
| SGBM match | 8.339 ms |
| SGBM postprocess | 0.473 ms |
| Stereo total | 8.971 ms |
| Capture-to-products compute | **14.220 ms** |

The previous recorded final frame was 35.101 ms total with 28.897 ms in stereo. The optimized hardware run is therefore **59.5% faster end-to-end** and **69.0% faster in stereo** for the same production profile. The final output contained 71,309 valid depth pixels: 72.2% of the complete depth image and 89.8% of the searchable disparity region.

### OpenCV thread scaling on the Pi

| Threads | p50 | p95 | p99 | Throughput | Process CPU |
|---:|---:|---:|---:|---:|---:|
| 1 | 32.764 ms | 32.946 ms | 33.127 ms | 29.829 Hz | 99.9% |
| 2 | 22.629 ms | 22.762 ms | 23.122 ms | 42.734 Hz | 151.9% |
| 3 | 22.118 ms | 22.255 ms | 22.670 ms | 43.690 Hz | 161.9% |
| 4 | **18.026 ms** | **18.220 ms** | 22.912 ms | **52.770 Hz** | 215.5% |

The production profile now uses all four OpenCV threads, as requested.

## Gateway and visual verification

The staged HTTPS gateway was exercised on the Pi. The root page, authenticated session creation, health endpoint, and latest-frame endpoint all returned HTTP 200; the retrieved native frame was 1,783,186 bytes. Browser visual QA used the same staged web assets, status document, and captured native frame through a loopback-only fixture because the appliance certificate is intentionally private/self-signed and the test browser will not bypass certificate safety interstitials.

Visual artifacts and machine-readable results are in `deliverables/pi5-oracle-optimization-20260910/`. The Pi's previously installed system service was not overwritten; testing used the isolated staging tree `/home/cam/nuvo-depth-codex-20260910`.

## Remaining qualification work

The code path requested for this pass—capture, rectification, disparity/depth, and viewer usability—is operational on the Pi. The remaining work is deliberately evidence-dependent: per-unit stereo calibration and accepted metric range, oscilloscope validation of exposure/trigger timing, allocator profiling before pooling, asynchronous publication only if consumers create pressure, ground-truth SGBM quality tuning, optional shared AE, and independent qualification of additional presets.
