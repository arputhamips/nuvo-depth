#include "depthd/contract.hpp"

int main() {
  depthd::FrameMetadata frame;
  frame.sequence = 1;
  frame.capture_time_ns = 1'000;
  frame.ready_time_ns = 2'000;
  frame.calibration_id = 42;
  frame.health = depthd::HealthState::kOk;
  if (!depthd::is_valid(frame)) {
    return 1;
  }

  frame.width = 640;
  if (depthd::is_valid(frame)) {
    return 2;
  }

  frame.width = depthd::kImageWidth;
  frame.ready_time_ns = 999;
  if (depthd::is_valid(frame)) {
    return 3;
  }
  return 0;
}
