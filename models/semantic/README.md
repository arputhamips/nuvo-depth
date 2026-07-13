# Semantic model artifacts

`manifest.json` is the authority for model identity, geometry, cadence, class
mapping, and artifact verification. Model binaries are intentionally not
checked into this repository.

The existing, robot-validated `yolov8s` HEF remains the first instance backend.
The new dense-region candidate is Hailo Model Zoo **v2.17.0**
`segformer_b0_bn`, which matches the installed DFC 3.33.0 / HailoRT 4.23.0
toolchain generation. The pinned artifact is 16,738,258 bytes with SHA-256
`2093951835BAFDB0585F93E3EDDFB13FAC2AA926C45475CCEDABE5B2DAA7A674` and an
`NHW 512x1024` class-id output. Re-verify those values with
`hailortcli parse-hef` on every deployed copy.

Runtime dependencies and HEFs stay outside the portable semantic libraries.
The core unit tests therefore run on development hosts without Hailo software.

Acceptance before `semantic.enabled` becomes true:

1. Verify both artifact hashes and parsed input/output tensor geometry.
2. Run each model alone on known images and preserve its decoded result.
3. Run both models under the single Hailo owner at 5 Hz each.
4. Confirm depth remains 10 Hz with no additional result-ring drops.
5. Validate humans, pets, vegetation, walls, fences, and terrain on recorded
   indoor/outdoor robot scenes; unknown geometry must remain an obstacle.
6. Complete the listed license reviews.
