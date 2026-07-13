# Semantic model candidates for the Nuvo robot

## Decision

Use the already compiled YOLOv8s + SegFormer-B0 pair as the first Hailo-8L
integration baseline. In parallel, export and compile the GOOSE
`ppliteseg_category_512` checkpoint as the first production-oriented dense
model experiment. Do not treat Cityscapes SegFormer as the final lawn model.

The native semantic backend, fusion, IPC, and ROS contracts are model-neutral,
so this comparison does not change the locked CPU stereo path.

## Ranked path

1. **Immediate hardware baseline:** YOLOv8s detection + SegFormer-B0.
2. **Best off-road checkpoint to compile next:** YOLOv8s + GOOSE
   PP-LiteSeg-B category-512.
3. **Lowest-risk custom retrain:** YOLOv8s + Hailo-supported
   DeepLabV3-MobileNetV2 without dilation.
4. **Long-term single network:** a custom Panoptic-DeepLab MobileNetV3 with
   semantic logits, center heatmap, and offset tensors; native C++ performs
   grouping and fusion.

## Immediate baseline

Hailo Model Zoo v2.17 publishes these Hailo-8L figures:

| Model | Input | Parameters | Operations | Hailo-8L FPS |
|---|---:|---:|---:|---:|
| YOLOv8s detection | 640x640 | 11.2M | 28.6G | 110 |
| SegFormer-B0 | 512x1024 | 3.72M | 35.76G | 11 |

Sources: [Hailo-8L detection models](https://raw.githubusercontent.com/hailo-ai/hailo_model_zoo/v2.17/docs/public_models/HAILO8L/HAILO8L_object_detection.rst),
[Hailo-8L semantic models](https://raw.githubusercontent.com/hailo-ai/hailo_model_zoo/v2.17/docs/public_models/HAILO8L/HAILO8L_semantic_segmentation.rst).

YOLO is filtered immediately to person, dog, cat, and bird. SegFormer keeps
only wall, fence, vegetation, and terrain; its Cityscapes person/rider labels
are deliberately discarded because living-obstacle safety belongs to the
instance detector. Its Cityscapes domain is
urban and its dataset/model redistribution terms require review; Cityscapes
prohibits commercial use without permission. See the
[Cityscapes license](https://www.cityscapes-dataset.com/license/).

## GOOSE PP-LiteSeg experiment

GOOSE supplies a 512x512 PP-LiteSeg-B checkpoint trained for field robotics.
The 12-category model reports 67.21% mIoU and groups the classes we need:

- animal and human;
- construction, including walls and fences;
- generic objects, poles, rocks, and obstacles;
- terrain and water;
- vegetation, including grass, crops, hedges, bushes, trunks, roots, and
  crowns.

Vehicle and sky groups can be discarded. Use the category checkpoint, not the
64-class checkpoint, because the smaller ontology matches the robot and has a
higher reported score. Sources: [GOOSE pretrained models](https://goose-dataset.de/docs/setup/),
[GOOSE ontology](https://goose-dataset.de/docs/class-definitions/).

The checkpoint is not a ready-made HEF. Expected DFC risk is moderate:
convolutional STDC layers and fixed 512 dimensions are favorable, while
adaptive/global pooling, reductions, sigmoid attention, and bilinear resize
must be checked during ONNX parse. PaddleSeg notes the adaptive-pooling export
constraint and recommends dimensions divisible by 128; 512 satisfies it.
See the [official PP-LiteSeg deployment notes](https://raw.githubusercontent.com/PaddlePaddle/PaddleSeg/release/2.10/configs/pp_liteseg/README.md)
and [PP-LiteSeg paper](https://arxiv.org/abs/2204.02681).

GOOSE data is CC BY-SA 4.0, its processing repository is MIT, and PaddleSeg is
Apache-2.0. Attribution/share-alike obligations for distributed weights still
need a release review. [GOOSE repository](https://github.com/FraunhoferIOSB/goose_dataset).

## Custom production retrain

The safest architecture is Hailo Model Zoo v2.17
`deeplab_v3_mobilenet_v2_wo_dilation`:

- input 513x513;
- 2.10M parameters;
- 3.21G operations;
- 60 FPS published on Hailo-8L;
- Apache-2.0 implementation path.

Its exact graph already passes DFC 3.33.0. The recommended compact ontology is:
person, companion animal, structure/barrier, solid obstacle,
high/non-traversable vegetation, low/traversable grass, traversable terrain,
water/unsafe terrain, and unknown. Train from GOOSE plus Nuvo indoor/outdoor
camera data. [Hailo DeepLab contract](https://raw.githubusercontent.com/hailo-ai/hailo_model_zoo/v2.17/hailo_model_zoo/cfg/networks/deeplab_v3_mobilenet_v2_wo_dilation.yaml).

## Long-term single network

Panoptic-DeepLab MobileNetV3 is the best conceptual fit because one graph can
produce semantic "stuff" regions and separated person/pet/obstacle "things".
Export only semantic logits, center heatmap, and pixel offsets; keep instance
grouping and safety fusion outside the graph. It needs a custom panoptic
dataset and carries medium DFC risk due to dual decoders and resize/offset
heads. Sources: [Panoptic-DeepLab](https://research.google/pubs/panoptic-deeplab-a-simple-strong-and-fast-baseline-for-bottom-up-panoptic-segmentation/),
[DeepLab2](https://github.com/google-research/deeplab2), and
[GOOSE annotation format](https://goose-dataset.de/docs/dataset-structure/).

## Candidates not selected

- GA-Nav is useful as an off-road navigability reference, but its transformer,
  attention/einsum, softmax, and PSA head are higher DFC risk and it does not
  provide separate person/pet instances. [GA-Nav](https://github.com/rayguan97/GANav-offroad).
- PIDNet-S and Fast-SCNN remain secondary lightweight experiments, but neither
  has the exact low-risk Hailo graph path of DeepLab-MobileNetV2.
- RELLIS-3D and WildScenes have non-commercial dataset terms; RUGD lacks a
  sufficiently clear commercial license for this production path.
- Query/deformable-attention panoptic models are too operator-heavy for the
  first Hailo-8L implementation.

Geometry remains authoritative for every model: semantics may add a safety
restriction or label an obstacle, but it never removes a stereo obstacle or
marks unknown geometry safe.

## Native Pi baseline

The initial YOLOv8s + SegFormer-B0 pair now runs in the native semantic worker
behind the capacity-one pointer/lease seam. A 60-second physical Pi 5 +
Hailo-8L gate preserved the locked CPU SGBM profile at 9.999 Hz while both
models were enabled at their 5 Hz configured cadence. The combined process used
13.89% normalized four-core CPU; semantic submission was 0.103 ms median and
0.132 ms maximum. There were no camera-pair drops, pending-frame replacements,
semantic errors, or outstanding leases. Preview remained disabled for the
production measurement.
