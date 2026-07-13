# Hailo compilation toolchain on this workstation

The isolated Hailo compiler environment is installed in the Ubuntu 24.04 WSL
distribution. It is not required on the Raspberry Pi and does not modify the
robot runtime.

## Pinned versions

- Python virtual environment: `/home/workstation/.venvs/hailo-dfc-3.33.0`
- Hailo Dataflow Compiler: `3.33.0`
- Hailo Model Zoo: `2.17.0`
- Model Zoo checkout:
  `/home/workstation/.local/share/depthd-hailo/hailo_model_zoo`
- Model Zoo revision: `1bde1102a7706e43688c863330a93acbc0f4db90`
- TensorFlow: `2.18.0`
- CUDA user-space libraries: `12.5`
- GPU: NVIDIA RTX 4070 Ti, 12 GiB
- Runtime compatibility target: HailoRT `4.23.0`, Hailo-8L

The Model Zoo is installed editable with compatible Python 3.12 wheels for
its host utilities. Its historical Pillow, ImageIO, and scikit-image pins are
not forced because those releases do not provide usable Python 3.12 wheels.
`hailomz info segformer_b0_bn` and the DFC import have been verified.

## Enter the environment

From PowerShell:

```powershell
wsl.exe -d Ubuntu-24.04
source /home/workstation/.venvs/hailo-dfc-3.33.0/bin/activate
export CUDA_VISIBLE_DEVICES=0
```

Set `CUDA_VISIBLE_DEVICES=0` explicitly. DFC 3.33.0 automatically ignores a
GPU when more than five percent of its memory is already in use; the Windows
desktop normally exceeds that threshold even though TensorFlow can use the
device. Explicit selection has been verified.

## Verification

```bash
python -c "import hailo_sdk_client; print('DFC import OK')"
python -c "import tensorflow as tf; print(tf.config.list_physical_devices('GPU'))"
hailo --version
hailomz info segformer_b0_bn
```

Expected compiler version is `3.33.0`; TensorFlow should list GPU device 0.
HailoRT and a PCIe Hailo device are intentionally absent from this workstation:
compilation happens here and HEF execution is validated on the Pi.

## Current stock semantic artifact

The official Model Zoo v2.17 Hailo-8L `segformer_b0_bn.hef` is cached outside
Git at:

```text
artifacts/semantic_models/v2.17.0/hailo8l/segformer_b0_bn.hef
```

Verification:

```text
bytes:  16738258
sha256: 2093951835BAFDB0585F93E3EDDFB13FAC2AA926C45475CCEDABE5B2DAA7A674
input:  UINT8 NHWC 512x1024x3
output: UINT8 NHW 512x1024 (Cityscapes class IDs)
target: HAILO8L
```

This stock HEF does not need to be recompiled. DFC is retained for a later
custom lawn/robot dataset, graph changes, quantization, and HEF generation.
