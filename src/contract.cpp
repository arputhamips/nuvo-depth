#include "depthd/contract.hpp"

namespace depthd {

bool is_valid(const FrameMetadata& metadata) noexcept {
  if (metadata.sequence == 0 || metadata.capture_time_ns <= 0) {
    return false;
  }
  if (metadata.ready_time_ns < metadata.capture_time_ns) {
    return false;
  }
  if (metadata.width != kImageWidth || metadata.height != kImageHeight) {
    return false;
  }
  if (metadata.encoding == PixelEncoding::kDisparityFloat32 &&
      metadata.stride_bytes < metadata.width * sizeof(float)) {
    return false;
  }
  return metadata.calibration_id != 0;
}

}  // namespace depthd
