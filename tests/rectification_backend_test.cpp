#include "depthd/rectification_backend.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {

class FakeBackend final : public depthd::RectificationBackend {
 public:
  [[nodiscard]] std::string_view name() const noexcept override {
    return "fake";
  }

  void rectify(depthd::RectificationCamera, const depthd::ImageView&,
               const depthd::MutableImageView&,
               depthd::RectificationStats& stats) override {
    stats = {};
  }
};

depthd::RectificationMap identity_map(std::uint32_t width,
                                     std::uint32_t height) {
  depthd::RectificationMap map;
  map.width = width;
  map.height = height;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      map.source_x.push_back(static_cast<float>(x));
      map.source_y.push_back(static_cast<float>(y));
    }
  }
  return map;
}

}  // namespace

int main() {
  depthd::StereoRectificationMaps maps;
  maps.left = identity_map(4, 3);
  maps.right = identity_map(4, 3);
  if (!depthd::is_valid(maps)) {
    return 1;
  }

  depthd::RectificationBackendRegistry registry;
  registry.register_backend(
      "fake", [](const depthd::RectificationBackendConfig&,
                 const depthd::StereoRectificationMaps&) {
        return std::make_unique<FakeBackend>();
      });
  if (!registry.contains("fake") || registry.contains("missing")) {
    return 2;
  }

  depthd::RectificationBackendConfig config;
  auto selected = registry.create("fake", "", false, config, maps);
  if (selected->name() != "fake") {
    return 3;
  }

  auto fallback = registry.create("missing", "fake", true, config, maps);
  if (fallback->name() != "fake") {
    return 4;
  }

  try {
    static_cast<void>(registry.create("missing", "fake", false, config, maps));
    return 5;
  } catch (const std::runtime_error&) {
  }

  try {
    registry.register_backend(
        "fake", [](const depthd::RectificationBackendConfig&,
                   const depthd::StereoRectificationMaps&) {
          return std::make_unique<FakeBackend>();
        });
    return 6;
  } catch (const std::invalid_argument&) {
  }

  maps.right.source_y.pop_back();
  if (depthd::is_valid(maps)) {
    return 7;
  }
  return 0;
}
