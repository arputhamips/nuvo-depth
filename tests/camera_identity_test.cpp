#include "depthd/capture.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

int main() {
  const depthd::CameraIdentity cam0{
      "/base/axi/pcie@1000120000/rp1/i2c@80000/imx296@1a", "imx296", "front", 1U};
  const depthd::CameraIdentity cam1{
      "/base/axi/pcie@1000120000/rp1/i2c@88000/imx296@1a", "imx296", "front", 0U};
  // Stable enumeration and physical roles are separate: CSI ports define roles.
  for (bool reverse : {false, true}) {
    std::vector<depthd::CameraIdentity> cameras{cam0, cam1};
    if (reverse) std::reverse(cameras.begin(), cameras.end());
    depthd::canonicalize_camera_order(cameras);
    if (cameras[1].stable_id != cam1.stable_id ||
        cameras[0].stable_id != cam0.stable_id ||
        cameras[0].discovery_index != 0U || cameras[1].discovery_index != 1U) {
      return 1;
    }
    // Extra cameras and discovery order must never displace the fixed ports.
    cameras.insert(cameras.begin(), {"/usb/camera", "usb", "external", 9U});
    if (reverse) std::reverse(cameras.begin(), cameras.end());
    const auto bindings = depthd::bind_pi5_fixed_csi_ports(cameras);
    if (bindings[0].role != depthd::CameraRole::kLogicalLeft ||
        bindings[0].camera.stable_id != cam0.stable_id ||
        bindings[1].role != depthd::CameraRole::kLogicalRight ||
        bindings[1].camera.stable_id != cam1.stable_id) return 4;
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
  // Missing ports, wrong sensor paths and ambiguous ports must fail closed.
  auto alias = cam0;
  alias.stable_id = "/alternate/rp1/i2c@80000/imx296@1a";
  auto wrong_sensor = cam0;
  wrong_sensor.stable_id = "/base/axi/rp1/i2c@80000/imx219@10";
  for (const auto& cameras : {empty, std::vector<depthd::CameraIdentity>{cam0},
                             std::vector<depthd::CameraIdentity>{cam1},
                             std::vector<depthd::CameraIdentity>{wrong_sensor, cam1},
                             std::vector<depthd::CameraIdentity>{cam0, alias, cam1}}) {
    try {
      (void)depthd::bind_pi5_fixed_csi_ports(cameras);
      return 5;
    } catch (const std::runtime_error&) {
    }
  }
  return empty.empty() ? 0 : 3;
}
