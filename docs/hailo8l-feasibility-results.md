# LAS2-S / Hailo-8L feasibility results

Tested on 2026-07-11 with DFC 3.33.0, Model Zoo 2.17.0, and target
`hailo8l`. The Raspberry Pi device has HailoRT/firmware 4.23.0.

## Result

No HEF was produced. DFC parses and quantizes the LAS2-S graph, but its
automatic allocator cannot create a legal Hailo-8L schedule. Do not treat any
generated feasibility HAR as a deployable or accuracy-qualified model.

The closest feasibility result used a 512 x 384 internal model, frozen
calibration-pair normalization, bounded correlation fan-out, learned refinement
on Hailo, and host-only final 9-tap convex upsampling. DFC estimated 44 FPS but
rejected the partition at 65 LCUs versus the Hailo-8L limit of 64. Other legal
graph forms failed at context portal counts (`10 inputs + 7 outputs`) or dynamic
normalization agents. The full 736 x 544 graph failed earlier in allocation.

## Proven gates

- Official LAS2-S checkpoint loaded at max disparity 192.
- Static ONNX export is rank 4, opset 17, with correlation and convex upsample
  rewrites checked against PyTorch and ONNX Runtime.
- Full 736 x 544 ONNX parses with no unsupported operators.
- Full and split graphs reach Hailo quantization.
- The split postprocess reconstructs the full ONNX output bit-for-bit.
- A bounded-fan-out correlation topology parses and retains numerical parity.
- Calibration-pair-frozen normalization can be folded into preceding Conv
  weights with approximately `3.6e-5 px` reference-to-ONNX MAE on that pair.

The frozen-normalization result is only a compiler experiment. It must be
validated on a representative capture set before it can be considered as a
production substitution for dynamic InstanceNorm.

The saved `verification-pending` bundle contains the rectified left/right input
images and the floating-point ONNX reference disparity. It intentionally has no
Hailo output: producing one without a valid HEF would make the comparison
misleading. Once a legal HEF exists, `tools/pi/validate_hef.py` will add the raw
Hailo disparity, visualizations, difference map, and error metrics.

## Hardware findings

The temporary native Pi capture path successfully opened both IMX296 cameras at
728 x 544 RGB888, rectified the images, and saved a stereo pair. However:

- selected sensor timestamps were 43.978 ms apart and both frames reported
  `SyncReady=false`;
- the nominal left camera image was severely out of focus while the right image
  was sharp.

These issues block meaningful depth-accuracy acceptance independently of the
Hailo compiler result.

## Recommended next step

Open a Hailo support case with the optimized HAR and allocator logs and request
a manual placement/cut script for the 65-LCU 512 x 384 partition. In parallel,
evaluate a Hailo-native stereo network with the same daemon contract. Do not
spend more time on QAT or a larger compiler GPU until a legal 8L allocation is
demonstrated; GPU memory was not the final blocker.

After allocation succeeds, rebuild with synchronized real calibration images,
then run `tools/pi/validate_hef.py` against the saved ONNX reference. The tool
emits raw reference/Hailo arrays, color maps, a difference map, error percentiles,
and device/host timing.
