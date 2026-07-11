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
