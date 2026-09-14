# Sintel synthetic stereo benchmark

This is an offline matcher benchmark, not camera, rectification, or metric-depth
qualification for the Nuvo rig. It uses the native `NativeDisparityBackend`
directly on MPI Sintel stereo training frames. No camera leases, Nuvo calibration
maps, EGL path, depth conversion, or gateway are exercised.

Source: [MPI Sintel Stereo Training Data](https://sintel.is.tue.mpg.de/stereo)
(`MPI-Sintel-stereo-training-20150305.zip`, 2.5 GB). The official archive
contains clean/final left/right image sequences, RGB-encoded disparity, and
occlusion/out-of-frame masks. The data are kept under the Git-ignored `data/`
directory; do not commit or redistribute the archive or extracted frames.
The local archive passed a full ZIP CRC test. Its SHA-256 is
`397506a0ab193189fb367f77ea772ef75435aacbc0a9f950c484df9a0ea3673f`.
Both passes contain 1,064 complete frames and pass preflight.

## Protocol

- Each 1024 x 436 source pair is center-cropped to 584 x 436, then both eyes are
  resized to Nuvo's 728 x 544 grayscale matcher input. This preserves image
  aspect ratio but sacrifices source horizontal field of view. Results are
  therefore **Nuvo-geometry synthetic scores**, not official Sintel scores.
- Disparity magnitude is decoded from the official RGB packing and multiplied
  by the horizontal resize factor. A preflight photometric check confirms that
  right-image matches lie near `x - disparity`, not `x + disparity`. Do not
  blindly negate the encoded RGB value based on the website's signed-disparity
  wording.
- Scoring excludes Sintel occlusions and out-of-frame pixels, pixels whose
  right match leaves the common crop, ground truth outside the active matcher
  disparity interval, and the matcher's valid-disparity ROI.
- `coverage` is valid predictions / eligible ground-truth pixels.
  `bad_2_including_invalid` counts both errors greater than 2 px and invalid
  predictions as failures over the same denominator. `mae_valid_only_px`
  describes accuracy *conditional* on a valid prediction. Report all three;
  MAE alone can be improved by dropping hard pixels.
- Timing includes only the native matcher call (its own grayscale preparation,
  matching, and disparity classification), not Sintel PNG decoding or the
  synthetic resize. `matcher_over_budget_frames` counts calls slower than
  `1000 / target_hz` ms. It is not a hardware 10 Hz throughput claim.
- Run clean and final separately; keep the product profile and stereo-control
  file fixed for both. `summary.json` is pixel-weighted over all scored frames,
  and `frames.csv` keeps per-frame values.

## Prepared local data and preflight

The official zip is staged at
`data/sintel/MPI-Sintel-stereo-training-20150305.zip`, extracted under
`data/sintel/`. A minimal Release build is already configured locally at
`build/sintel-release`. To rebuild on an Ubuntu/WSL host with OpenCV 4 and
yaml-cpp:

```bash
cmake -S . -B build/sintel-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDEPTHD_WITH_CONFIG=ON -DDEPTHD_WITH_OPENCV=ON
cmake --build build/sintel-release --target depthd_sintel_benchmark -j4
ctest --test-dir build/sintel-release -R depthd_sintel_benchmark_self_test --output-on-failure
./build/sintel-release/depthd_sintel_benchmark \
  --dataset-root data/sintel/training \
  --config config/nuvo_pi5_depth_100mm.yaml \
  --stereo-control config/stereo.json \
  --pass final
```

The last command is preflight-only; it does not call the matcher. It verifies
complete frame paths and decodes one sample. Both the `clean` and `final`
passes have preflighted successfully. On the first frame, the mean grayscale
residual is 2.29 (clean) / 2.60 (final) at `x - d`, versus 51.63 / 47.15 at
`x + d`; this confirms the decoded magnitude's correspondence direction.

## Full benchmark commands (hold until authorized)

When ready, run each pass with a new, empty output directory:

```bash
./build/sintel-release/depthd_sintel_benchmark \
  --dataset-root data/sintel/training \
  --config config/nuvo_pi5_depth_100mm.yaml \
  --stereo-control config/stereo.json \
  --pass clean --run --output-dir out/sintel-clean

./build/sintel-release/depthd_sintel_benchmark \
  --dataset-root data/sintel/training \
  --config config/nuvo_pi5_depth_100mm.yaml \
  --stereo-control config/stereo.json \
  --pass final --run --output-dir out/sintel-final
```

For Pi 5 matcher latency, compile the same target in a **Release** build on
the Pi, transfer the Sintel `training/` data there, and run the same commands.
Host latency is not a Pi performance measurement. Neither
run replaces hardware-trigger, capture-pairing, calibration, or thermal tests.
