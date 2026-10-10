#include "depthd/capture.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

int main() {
  const depthd::CameraIdentity cam0{
      "/base/axi/pcie@1000120000/rp1/i2c@80000/imx296@1a", "imx296", "front", 1U};
  const depthd::CameraIdentity cam1{
      "/base/axi/pcie@1000120000/rp1/i2c@88000/imx296@1a", "imx296", "front", 0U};
  // Both observed probe orders must select the same sensor for left=1/right=0.
  for (bool reverse : {false, true}) {
    std::vector<depthd::CameraIdentity> cameras{cam0, cam1};
    if (reverse) std::reverse(cameras.begin(), cameras.end());
    depthd::canonicalize_camera_order(cameras);
    if (cameras[1].stable_id != cam1.stable_id ||
        cameras[0].stable_id != cam0.stable_id ||
        cameras[0].discovery_index != 0U || cameras[1].discovery_index != 1U) {
      return 1;
    }
  }
  for (auto invalid : {std::vector<depthd::CameraIdentity>{cam0, cam0},
                       std::vector<depthd::CameraIdentity>{{"", "imx296", "front", 0U}}}) {
    try {
      depthd::canonicalize_camera_order(invalid);
      return 2;
    } catch (const std::invalid_argument&) {
    }
  }
  std::vector<depthd::CameraIdentity> empty;
  depthd::canonicalize_camera_order(empty);
  return empty.empty() ? 0 : 3;
}
