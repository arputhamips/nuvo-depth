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
