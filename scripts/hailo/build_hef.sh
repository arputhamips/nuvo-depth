#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
  echo "usage: $0 <model.onnx> <artifact-dir> [calibration.npy]" >&2
  exit 2
fi

MODEL_ONNX="$(realpath "$1")"
ARTIFACT_DIR="$(realpath -m "$2")"
CALIBRATION_SET="${3:-}"
HAILO_ENV="${HAILO_ENV:-/opt/hailo/venv}"
NET_NAME="las2_s_544x736_d192"
WORK_BASE="${HAILO_WORK_ROOT:-/opt/hailo/work}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
MODEL_SCRIPT="${MODEL_SCRIPT:-$SCRIPT_DIR/../../models/lite_any_stereo/las2_s_feasibility.alls}"
COMPILE_SCRIPT="${COMPILE_SCRIPT:-$SCRIPT_DIR/../../models/lite_any_stereo/las2_s_hailo8l_compile.alls}"

mkdir -p "$ARTIFACT_DIR"
mkdir -p "$WORK_BASE"
WORK_ROOT="$(mktemp -d "$WORK_BASE/${NET_NAME}.XXXXXX")"
mkdir -p "$WORK_ROOT/optimization"
export CUDA_VISIBLE_DEVICES="${CUDA_VISIBLE_DEVICES:-0}"

"$HAILO_ENV/bin/hailo" parser onnx "$MODEL_ONNX" \
  --net-name "$NET_NAME" \
  --har-path "$ARTIFACT_DIR/${NET_NAME}_native.har" \
  --parsing-report-path "$ARTIFACT_DIR/${NET_NAME}_parse.json" \
  --hw-arch hailo8l \
  --input-format left=NCHW right=NCHW \
  -y

if [[ -n "$CALIBRATION_SET" ]]; then
  "$HAILO_ENV/bin/hailo" optimize "$ARTIFACT_DIR/${NET_NAME}_native.har" \
    --hw-arch hailo8l \
    --calib-set-path "$(realpath "$CALIBRATION_SET")" \
    --model-script "$MODEL_SCRIPT" \
    --output-har-path "$ARTIFACT_DIR/${NET_NAME}_optimized.har" \
    --work-dir "$WORK_ROOT/optimization"
else
  echo "No real calibration set supplied; using random data for compiler feasibility only." >&2
  "$HAILO_ENV/bin/hailo" optimize "$ARTIFACT_DIR/${NET_NAME}_native.har" \
    --hw-arch hailo8l \
    --use-random-calib-set \
    --calib-random-max 255 \
    --model-script "$MODEL_SCRIPT" \
    --output-har-path "$ARTIFACT_DIR/${NET_NAME}_optimized.har" \
    --work-dir "$WORK_ROOT/optimization"
fi

"$HAILO_ENV/bin/hailo" compiler "$ARTIFACT_DIR/${NET_NAME}_optimized.har" \
  --hw-arch hailo8l \
  --model-script "$COMPILE_SCRIPT" \
  --output-dir "$ARTIFACT_DIR" \
  --auto-model-script "$ARTIFACT_DIR/${NET_NAME}_auto.alls" \
  --output-har-path "$ARTIFACT_DIR/${NET_NAME}_compiled.har"

echo "Compiler scratch retained at: $WORK_ROOT" >&2
