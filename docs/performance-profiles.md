# Pi 5 matcher candidates

The default is full-grid Balanced/3-way with 128 disparities and two CPU threads,
targeting 10 Hz. None of the current profiles has a
qualified sustained depth-rate guarantee. Capture targets and output cadence
requests are upper bounds; they do not make the matcher finish faster.

`web/depth-profiles.json` is the authoritative catalog for browser profile
selection. It is installed with the web console. The native YAML fallback and
`config/stereo.json` agree with its balanced default, including aggregation,
search range, smoothing and CPU threads. A regression check compares these
sources through the typed configuration loader. The historical calibration ID
`balanced_728x544_10` stays unchanged; the name does not select the Balanced
matcher or promise 10 depth frames/s.

All browser profiles use a 728 × 544 capture and matching grid, block size 5,
two OpenCV threads, speckle window 30, speckle range 2, left-right consistency
1, pre-filter cap 31 and no saturated-pixel rejection. Median smoothing
preserves the invalid mask.

| Profile | Aggregation | Search / accepted interval | Median | Uniqueness | Nominal near range |
|---|---|---|---|---|---|
| Quality | HH, eight paths | 96 / [0, 96) px | 5 | 0 | 0.514 m |
| Balanced, default | 3-way | 128 / [0, 128) px | Off | 0 | 0.386 m |
| Fast | 3-way | 64 / [0, 64) px | Off | 1 | 0.771 m |
| Wide | Standard, five paths | 128 / [0, 128) px | 3 | 1 | 0.386 m |

These ranges follow the nominal seed's `fB = 49.3542046188 px*m`; validated
per-unit calibration supplies actual metric geometry. Fast deliberately trades
near range, smoothing and directional streak rejection for lower compute
cost. Balanced and Wide also require scene-quality review. Selecting a
candidate changes matcher controls and keeps the capture/calibration geometry.

The retained September 15 Quality/HH Pi run measured 342.01 ms median compute
and 2.84 depth frames/s across 60 live frames at a 60 Hz capture target. Its
median HH match stage was 325.58 ms. The evidence is in
[the retained trace report](pi5-fullres-stereo-flicker-trace.md#september-15-deployed-qualityhh-verification).
This is historical evidence for those controls and hardware, not a throughput
pass for the current code or the default 10 Hz capture target. Older SGBM
measurements used different settings and are not qualification for the catalog.

The YAML manifests `fast-728x544-15.yaml` and `quality-1456x1088-5.yaml` are
retained historical geometry proposals. They are not the browser Fast and
Quality controls, and they are not selected by the packaged daemon. Their
different grids require an explicit daemon configuration, restart and matching
calibration. The separate half-resolution benchmark YAML retains the earlier
3-way/64-working-disparity path; it is not a silent replacement for Quality.

To qualify a candidate, record its exact controls, code/build identity,
calibration, capture target, encoding and consumer load on the shipping Pi
cohort. Measure sustained published depth rate, end-to-end latency, thermal and
memory behavior, dropped pairs and scene/metric accuracy over the required
soak. Update the qualification report only after those gates pass. Copy and
scheduling fixes cannot establish a new matcher throughput claim by themselves.

The default rectifier stages camera luma into reused cached RAM before CPU
remapping. On the tested Pi5 this reduced rectification from about 16 ms to
3.7 ms, slightly below the GPU path at about 4.7 ms. Raw products and previews
are driven by consumer demand; an image-only request does not run the matcher.

## October 9 live verification

The combined 128-disparity, two-thread build was tested on the dual-IMX296 Pi 5
using RP1 PIO hardware triggering and selected depth plus validity output.

| Test | Observed output rate | Median processing | Median rectification |
|---|---|---|---|
| CPU, 10 Hz capture target, 30 seconds | 10.00 Hz | 78.07 ms | 3.50 ms |
| CPU capacity, 20 Hz capture target, 15 seconds | 12.12 Hz | 81.61 ms | 4.68 ms |
| GPU capacity, 20 Hz capture target, 15 seconds | 12.00 Hz | 82.27 ms | 5.45 ms |

The higher capture target also increases ISP and memory traffic; capacity rows
are measured complete-pipeline output rates rather than inverse matcher times.
CPU remains the default. The 10 Hz run's local capture-to-receive latency was
100.17 ms median and 111.91 ms p95. Mono raw output at 10 Hz with a depth preview
at 1 Hz produced ten numeric admissions and ten encoded frames over ten seconds.
All fourteen raw selections and ten image preview variants passed live checks;
unused processing stages stayed idle without consumers. Hardware exposure
updates and forced-crash waveform cleanup passed. Throttling flags were zero
and the end temperature was 70.8 °C. These bounded device checks do not replace
the sustained throughput, scene accuracy and cohort qualification gates above.

The final installed build was also exercised from a WiFi peer using verified
TLS on `wlan0`. All fourteen selectable raw products passed, as did JPEG
rectified stereo RGB and H.264 rectified mono decoding. Mono-only delivery
followed the 10 Hz capture cadence without running rectification or matching.
For 600 depth-plus-validity frames, WiFi delivery averaged 9.82 frames/s by
arrival time and 9.58 frames/s across capture timestamps, with 26 skipped
capture sequences. The daemon processed 637 of 638 captured pairs across that
consumer interval, with no pending scheduler replacements. Network delivery
remains best effort; these results do not establish exact WiFi delivery.

Depth plus validity uses about 1.98 MB per frame, or 158.5 Mbit/s at 10 Hz
before network overhead. This appliance's measured-link WiFi budget is
24 MiB/s; the packaged conservative default remains 8 MiB/s. The gateway
rejects the legacy six-product 10 Hz WiFi subscription, permits a bounded
one-shot snapshot, and exposes the receiving interface and byte budget to
clients. Users explicitly select products, cadence and delivery policy. Wired
exact delivery still requires matching qualification evidence; no Ethernet
throughput measurement was performed in this run because its link was down.

## October 16 performance and production RAM assessment

Use a **2GB Pi 5 as the production candidate** for the current headless
appliance. The measured footprint does not justify 4GB or 8GB. A physical 2GB
pilot must pass the shipping workloads before ordering the production batch;
these tests ran on a physical 16GB Pi 5 with both firmware memory limits and a
separate reserved-memory experiment. Neither method reproduces a smaller SKU's
memory bandwidth or topology.

The installed application was commit
`96dfdf78ebed6f5d97e1bc94129b64c122a71b7e`, with the fixed physical CSI roles,
dual IMX296 cameras, 728 × 544 capture, stock 2.4GHz CPU and active cooling.
OpenCV was 4.10.0 and Linux was 6.12.75+rpt-rpi-2712. The browser ran on the
viewer's computer. No desktop browser, recording cache or inference workload
was included on the Pi. Calibration remained the provisional fixed-CSI seed
with SHA-256
`d0ade4d6821d9fd0c4d47ab3a1154dd11a27947df51eab5ff7bb2e180ffba4b0`;
these measurements do not qualify scene quality or metric depth accuracy.

Unrestricted-board capacity runs used depth plus validity, two matcher threads
unless specified, CPU rectification unless specified, and 30 seconds per case
after warmup. Rates count received unique native frames through the complete
pipeline, rather than taking the inverse of matcher time.

| Configuration | Depth frames/s | Median rectification | Median processing |
|---|---:|---:|---:|
| 128 disparities, 1 thread, capture 20 Hz | 7.08 | 6.61 ms | 140.89 ms |
| 128 disparities, 2 threads, capture 20 Hz | 12.11 | 4.41 ms | 80.32 ms |
| 128 disparities, 3 threads, capture 20 Hz | 7.78 | 4.08 ms | 124.22 ms |
| 128 disparities, 4 threads, capture 20 Hz | 5.52 | 4.06 ms | 182.78 ms |
| 128 disparities, GPU rectification, capture 20 Hz | 11.89 | 5.43 ms | 81.81 ms |
| 96 disparities, capture 25 Hz | 14.25 | 4.49 ms | 69.01 ms |
| 64 disparities, Fast, capture 30 Hz | 16.56 | 4.49 ms | 60.00 ms |

Keep two OpenCV threads and CPU rectification for this workload. The GPU
backend was verified active; it reduced native CPU use slightly but added
about 59 MiB of native RSS and did not improve depth rate. Four threads used
about 94% of total four-core CPU capacity while producing less than half the
two-thread throughput. More threads are not an effective tuning option here.

At 128 disparities, capture targets of 15 / 20 / 30 / 60 Hz delivered
12.10 / 12.11 / 11.89 / 11.31 depth frames/s. Raising capture to 60 Hz increased
ISP traffic without improving depth throughput. Retain the 10 Hz capture
target for the requested 10 Hz operating profile; use 15–20 Hz only for a
capacity measurement or a workload that actually needs the extra images.

Depth at 128 disparities and a rectified stereo RGB H.264 preview both reached
10.00 frames/s in a 30-second unrestricted-board run. A 5 Hz RGB preview also
maintained 10.00 depth frames/s. The `ultrafast` encoder preset reduced CPU
slightly without a useful depth-rate improvement at that target. Rectified
stereo RGB encoding alone reached 50.23 encoded frames/s at a 60 Hz capture
target. That last rate comes from encoded sequence progression, not WiFi
delivery or a decoder test at 50 Hz. Corrected H.264 runs used one encoder
thread, `veryfast`, 4 Mbit/s and `keyframe_interval=1` for latest-frame delivery.

Whole-system RAM was measured as `MemTotal - MemAvailable`, sampled every
0.5 seconds, with swap disabled. This includes the OS, kernel/DMA allocations
and the test clients running on the Pi; reclaimable cache is excluded. Process
RSS is also retained but is not summed as a substitute for system footprint.

The original kernel disables the memory cgroup controller, so the installed
units' `MemoryMax` settings were not enforced on that boot. A separate
controller-enabled boot recorded no memory-limit or OOM events in the tested
loads. A shipping image that relies on those limits must enable and validate
the controller; these tests restored the original boot configuration afterward.

| Workload with a 1GB firmware limit | Peak system footprint | Minimum available |
|---|---:|---:|
| 128-disparity depth at a 10 Hz target | 356.69 MiB | 634.48 MiB |
| Stereo RGB H.264 at 15 Hz | 353.70 MiB | 637.47 MiB |
| All 14 raw outputs, RGB preview, five admitted TLS clients | 564.20 MiB | 426.97 MiB |
| HH/128, all outputs, large calibration attempt | 793.53 MiB | 197.64 MiB |

The large calibration row is a partial stress observation: 400 capture attempts
yielded 256 accepted synthetic pairs and 144 rejected pairs. The solve did not
finish within the 180-second measurement plus 30-second completion allowance,
and one raw viewer timed out. There was no OOM, swap activity or thermal
throttling. This is evidence that bounded runtime can fit in 1GB, not a pass
for every feature on 1GB. Its roughly 198 MiB remaining margin is too narrow
for the production choice given OS, update and calibration headroom. The
smaller 27-attempt synthetic calibration workflow did complete under load;
the five-viewer stress variants sometimes admitted only the encoded viewers.
They are explicitly marked partial in the evidence.

Firmware `total_mem=2048` reduced 128-disparity capacity to about 6.8 frames/s
despite substantial available RAM. Disabling memory cgroups did not recover
the unrestricted rate. An attempted NUMA binding control still showed
interleaving across eight nodes, so it did not establish the cause. These
restricted-board rates must not be treated as measurements of physical 2GB
SKU performance. A separate experiment retained the original memory address
span while reserving touched anonymous RAM, leaving about 2012 MiB usable.
Its effective footprint subtracts only those reserved bytes; the original
global memory readings are retained unchanged.

That separate 2GB-available experiment completed a clean 600-second soak with
128 disparities, two matcher threads, a 10 Hz capture target, depth plus
validity and a 10 Hz disparity preview. It delivered 5,995 fresh numeric frames
at 9.99 frames/s, with seven skipped capture sequences. All five local TLS
viewers stayed connected: one depth/validity stream and four encoded streams,
each requesting 1 Hz. A WiFi peer also requested depth/validity at 10 Hz and
encoded preview at 1 Hz for 90 seconds during the soak.

Excluding the reserved test RAM, peak whole-system footprint was 540.41 MiB and
minimum available RAM was 1,471.50 MiB. Native RSS settled at 153.02 MiB and
gateway RSS stayed at 43.98 MiB; the observer itself peaked at 79.06 MiB.
Total CPU use averaged about 50% of four-core capacity, with native processing
using about 1.73 cores. Median processing was 78.56 ms, p95 was 96.74 ms;
local capture-to-receive latency was 102.81 ms median and 156.75 ms p95.
Temperature peaked at 61.15 °C, with zero throttling, swap or OOM events.
This is a bounded sustained throughput pass for those controls, not scene
accuracy, long-duration reliability or physical 2GB SKU qualification.

The concurrent WiFi peer received 881 depth/validity frames at 9.80 arrivals/s,
with 18 skipped capture sequences and no disconnect. Arrival intervals were
101.10 ms median, 119.10 ms p95 and 184.92 ms maximum. Its encoded stream
received 91 frames at 1.00 arrivals/s without error. Those are best-effort
radio measurements, not exact delivery; encoded capture-sequence gaps at a
1 Hz request are intentional. No Ethernet throughput claim is made.

The final 60-second combined stress case completed HH/128 matching, all 14
raw outputs, a 1 Hz RGB preview, five TLS viewers and the 27-attempt synthetic
calibration workflow (18 accepted pairs). All five viewers received frames
without errors and all six calibration fixture tests passed. Effective peak
whole-system footprint was 1,073.58 MiB, leaving 938.33 MiB available within
the same 2GB-available experiment, with no swap, OOM or throttling. Numeric
output averaged 1.95 frames/s under this extreme load; this is a capacity and
stability result, not a 10 Hz depth profile. Observer RSS reached 158.53 MiB,
so this includes substantial test-client overhead. Both the reserved RAM and
all temporary service overrides were released after the tests.

The first 600-second firmware-limited run was invalidated as a fixed-profile
comparison by changes to the matcher and encoder controls from the UI. Later
runs isolated the gateway on a temporary port and checked the controls during
measurement. Early attempts with an invalid H.264 GOP or loopback-rejected
viewers were rerun and are excluded from completed-workload claims.

Two reliability findings remain separate from the RAM decision. Several cold
boots exceeded the existing 20-second trigger preparation timeout and needed
a manual service start. A large combined-load calibration attempt on the
2GB-limited boot ended after about 441 seconds with a native stereo-pair
capture timeout; no OOM evidence was recorded. The large calibration solve did
not complete. These require investigation and cold-boot/load qualification
before a production release; buying additional RAM is not an evidenced fix.

The final restored 16GB boot started the trigger preparation, native daemon
and gateway successfully without manual intervention. All six backed-up boot
and application files matched their original hashes, all four device identity
files were unchanged, the original zram swap was active and unused, and no
profiling overrides or rollback timer remained. Verified TLS and a live
encoded stream on the normal port 7443 passed after the reboot.

The local evidence directory is `output/pi5-production-profile-20261016/`.
It contains per-case controls, source snapshots, samples, validation failures,
an HTML report, WiFi peer timing and final restoration receipts. It is ignored
by Git and contains no committed device credentials. The installed application
binary was unchanged by these profiling tests.

`tools/pi/profile_production_resources.py` repeats bounded cases on the Pi.
Copy the repository tools, SDK and tests together, prepare a JSON case list,
and run as root against the installed appliance, for example:

```json
[
  {"name":"capacity128","capture_hz":20,"disparities":128,"threads":2,"seconds":30},
  {"name":"depth_preview_soak","capture_hz":10,"disparities":128,"preview_hz":10,
   "preview_source":"disparity","gateway_clients":5,"gateway_products":["depth","validity"],
   "seconds":600,"warmup":10}
]
```

```sh
sudo python3 tools/pi/profile_production_resources.py \
  --cases /path/to/cases.json --output /path/to/fresh-evidence-directory
```

The profiler temporarily changes capture, matcher and encoding controls,
isolates the gateway on port 17443, rejects concurrent profiler runs, checks
for control changes, and restores original configuration bytes and service
states on exit. Calibration stress uses temporary synthetic fixtures and
never activates them. Boot memory limits, swap changes and the anonymous RAM
reservation were separate test operations and are not performed by this tool.
