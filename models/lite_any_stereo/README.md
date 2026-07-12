# Lite Any Stereo provenance

Upstream source lives at `third_party/LiteAnyStereo` as a pinned git submodule.
The initial audited commit is:

```text
ccf4712b843dcb60950bf88ec7f9ba3f936ae7c2
```

Do not commit checkpoints or generated ONNX/HAR/HEF files. Each generated model
must have a small text/JSON manifest containing upstream commit, checkpoint
SHA-256, input shape, max disparity, export script commit, DFC version, target
architecture, calibration-dataset hash, and output quantization metadata.

The future conversion scripts belong here; the native runtime must consume only
the final manifest and HEF, never import upstream Python.

## Export and compile

The exporter replaces the upstream rank-5/rank-6 correlation and convex
upsampling expressions with static rank-4 equivalents, simplifies the graph,
rewrites wide concatenations to a maximum fan-in of four, and checks PyTorch and
ONNX Runtime parity.

From the repository mounted in the pinned Ubuntu 22.04 WSL environment:

```bash
/opt/hailo/export-venv/bin/python models/lite_any_stereo/export_hailo.py \
  --upstream third_party/LiteAnyStereo \
  --checkpoint /opt/hailo/models/LAS2_S.pth \
  --output artifacts/las2_s/las2_s_hailo.onnx \
  --manifest artifacts/las2_s/export-manifest.json

bash scripts/hailo/build_hef.sh \
  artifacts/las2_s/las2_s_hailo.onnx \
  artifacts/las2_s
```

If the intact full-resolution graph does not allocate on Hailo-8L, generate the
parity-preserving host-postprocess boundary:

```bash
/opt/hailo/export-venv/bin/python models/lite_any_stereo/split_hailo_postprocess.py \
  --input artifacts/las2_s/las2_s_hailo.onnx \
  --output artifacts/las2_s/las2_s_hailo_split.onnx \
  --manifest artifacts/las2_s/split-manifest.json
```

The split keeps every learned operation on Hailo, including refinement and the
9-way softmax. The host performs only the fixed 9-tap convex weighted sum. The
split tool reconstructs the full ONNX output and fails unless parity is within
its strict tolerance.

The no-calibration invocation uses random RGB data only to prove parser,
optimizer, and allocator feasibility. A HEF intended for depth validation must
be rebuilt from synchronized, rectified images captured on the Nuvo camera rig.
The feasibility model script uses optimization level 0, the Hailo Model Zoo's
uniform-precision LayerNorm decomposition, and disables QAT and bias correction.
The default level-2 QAT pass exceeds a 12 GB compiler GPU at this resolution,
while level 1 spends substantial CPU time on bias correction. This profile is
for graph allocation and device execution only, never accuracy acceptance.

## Current Hailo-8L status

The July 2026 feasibility campaign did not produce a HEF. The closest 512 x 384
split graph reached an estimated 44 FPS but required 65 LCUs on a 64-LCU
Hailo-8L. See `docs/hailo8l-feasibility-results.md` before running additional
compiler experiments; the saved HAR is evidence for a Hailo support case, not a
deployable model.
