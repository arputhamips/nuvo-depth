# Photographic benchmark preview assets

The four JPEGs here are photographic examples from the [DIODE depth benchmark](https://diode-dataset.org/),
which publishes its dataset and code under the MIT license. They were downloaded
from the official benchmark site and renamed for use in the console:

| Local asset | Official source |
| --- | --- |
| `diode-indoor-photo.jpg` | `images/full/00019_00183_indoors_300_010.jpg` |
| `diode-indoor-depth.jpg` | `images/full/00019_00183_indoors_300_010_depth.jpg` |
| `diode-outdoor-photo.jpg` | `images/full/00013_00124_outdoor_240_010.jpg` |
| `diode-outdoor-depth.jpg` | `images/full/00013_00124_outdoor_240_010_depth.jpg` |

The depth JPEGs are DIODE *reference* visualizations, not Nuvo stereo-matcher
results, live depth, or a validation of the current hardware. The offline sample
gallery and encoded-preview fallback identify them accordingly. Do not crop the
source attribution or the non-live label from marketing screenshots.
