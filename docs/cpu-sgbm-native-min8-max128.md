# Native-resolution CPU SGBM: disparity 8 through 128

## Configuration

This experiment uses the final auto-exposed 728 x 544 stereo pair and the live
camera pipeline with no working-image downscale.

- working image: 728 x 544;
- minimum disparity: 8 pixels;
- OpenCV search width: 128 bins;
- accepted upper disparity limit: 128 pixels;
- block size: 7 x 7, the closest legal odd size to the requested 6 x 6;
- uniqueness: 1;
- speckle window/range: 30/2;
- `SGBM_3WAY`, one OpenCV thread.

OpenCV requires the search width to be a multiple of 16. It therefore searches
128 candidates beginning at 8 and the output mask rejects results at or above
128. The deterministic left search boundary is 136 pixels. With the calibrated
`f*b` of 79.6867, the accepted near limit is approximately 0.623 m.

## Recorded-pair replay

| Metric | Result |
| --- | ---: |
| Achieved rate | 6.966 Hz |
| Compute p50 / p99 | 143.446 / 143.673 ms |
| Match p50 | 140.765 ms |
| Normalized four-core CPU | 24.992% |
| Full-frame valid | 60.5% |
| Searchable-region valid | 74.3% |
| 10 Hz deadline misses | 100 / 100 |

The map retains substantially finer spatial structure than the half-resolution
backend, but it cannot replay at 10 Hz on the Pi.

## Live result

The cameras continued to trigger at 10 Hz while the latest-pair slot discarded
superseded pairs. The measured depth output remained above the project's 6 Hz
lower bound.

| Metric | Native 8..128 | Current half-resolution backend |
| --- | ---: | ---: |
| Depth output rate | 6.466 Hz | 9.9999 Hz |
| Process CPU, normalized | 25.80% | 9.84% |
| Whole-SoC CPU | 25.94% | 9.87% |
| SGBM p50 | 141.55 ms | 23.58 ms |
| Depth-ready p50 | 227.77 ms | 53.99 ms |
| Median valid fraction | 60.85% | 65.25% |
| Ready-pair replacements | 70 / 201 | 0 / 610 |
| Effective JPEG rate | 1.29 Hz | 2.00 Hz |
| Sensor delta p50 / max | 18 / 20 us | 17 / 22 us |
| Applied left/right gain delta | 0.0 | 0.0 |

Auto exposure remained stable at 8000 us / 2.0x with a final metering median of
70 and no exposure updates.

## Assessment

The native mode succeeds at its primary goal: the disparity image shows finer
object boundaries and texture than the upsampled half-resolution result. It
also meets a 6 Hz minimum output rate. It should not replace the current robot
default without explicitly accepting these costs:

- approximately 2.6 times the normalized CPU;
- approximately 4.2 times the frame age;
- 35% of captured ready pairs replaced by newer pairs;
- compressed-image output below its 2 Hz contract;
- lower accepted density despite the finer spatial structure.

The live report and inputs are stored outside Git under
`artifacts/cpu_sgbm/native-min8-max128-b7-live-20260712/` and on the Pi under
`/home/cam/cpu-sgbm-native-min8-max128-b7-live-20260712/`.
