# Lite Any Stereo V2 to Hailo-8L

## Baseline

The upstream source is pinned as a git submodule. As audited on 2026-07-10, the
feed-forward LAS2-S/M/L models use a FasterNet feature pyramid, a fixed disparity
correlation volume, 2D aggregation, softmax/regression, and learned context
upsampling. LAS2-H adds iterative correlation, `einsum`, and `grid_sample` and is
not an initial Hailo-8L target.

The official Hailo StereoNet C++ example explicitly supports Hailo-8 only, not
Hailo-8L. There is no upstream LAS2 ONNX or HEF. Therefore HEF generation is a
feasibility deliverable, not an assumed build step.

Primary target: **LAS2-S**, fixed two-input RGB shape `1x3x544x736`, max disparity
192, cropped back to 728 x 544. The released checkpoint is structurally fixed at
48 quarter-resolution disparity channels; setting max disparity to 96 produces
only 24 channels and is incompatible with the trained aggregation layers. LAS2
requires dimensions divisible by 32; the
existing 728 x 544 image is symmetrically padded by four columns on each side.
LAS2-M is the second target only after S compiles and its Hailo-8L FPS is known.

## Reproducible gates

1. **Upstream parity** — download the LAS2-S checkpoint and record checksum;
   compare PyTorch output on real rectified Nuvo pairs.
2. **Static export** — replace dynamic/unfriendly correlation construction with
   fixed-shape slices/concatenation if required, export ONNX, run ONNX checker,
   and compare output against PyTorch.
3. **DFC parse** — parse for `hailo8l` with the pinned Hailo-8/8L toolchain. Save
   the HAR and compiler report. Do not move a failed block silently to Pi CPU.
4. **Quantization** — build a representative set from the actual IMX296 rig
   across grass, hard edges, shade, sunlight, low texture, and motion. Compare
   float and quantized disparity metrics and failure masks.
5. **Compile** — create the HEF and record DFC version, model commit, checkpoint
   hash, calibration-set hash, input shape, and compiler script hash.
6. **Device benchmark** — use HailoRT on the Hailo-8L to measure device FPS,
   host CPU, PCIe throughput, and thermally stable performance.
7. **End-to-end acceptance** — live 728 x 544 output at 6-10 Hz with bounded
   latency and no queue growth.

## Pass/fail policy

- Correlation/cost aggregation must run on Hailo for the low-CPU goal.
- Cheap final masking, crop, timestamping, and ROS packing may stay on CPU.
- A split that leaves PyTorch or ONNX Runtime in the Pi frame loop fails.
- LAS2-S failure is recorded before LAS2-M is attempted.
- If both fail, keep the daemon/bridge architecture and evaluate a Hailo-native
  stereo network; classical CPU block matching remains a bring-up/replay
  backend, not the production low-CPU answer.

## Toolchain pinning

Hailo-8/8L uses the Hailo Model Zoo 2.x / Dataflow Compiler 3.x line; current
Model Zoo main targets newer Hailo devices. Compiler and HailoRT versions must be
captured in every artifact manifest. HEF files are build artifacts and are not
committed by default.

The first hardware-matched toolchain is:

- Ubuntu 22.04 WSL2, Python 3.10;
- Dataflow Compiler 3.33.0;
- Hailo Model Zoo 2.17.0;
- Pi HailoRT and firmware 4.23.0;
- target architecture `hailo8l`.

## Sources

- [Lite Any Stereo official repository](https://github.com/TomTomTommi/LiteAnyStereo)
- [LAS2 checkpoints](https://huggingface.co/tomtomtommi/LiteAnyStereoV2)
- [LAS2 paper](https://arxiv.org/abs/2606.24457)
- [Hailo stereo C++ example](https://github.com/hailo-ai/hailo-apps/blob/main/hailo_apps/cpp/depth_estimation_stereo/README.md)
- [Hailo Model Zoo](https://github.com/hailo-ai/hailo_model_zoo)
