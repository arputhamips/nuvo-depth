#include "depthd/rectification_backend.hpp"

#include <stdexcept>
#include <utility>

namespace depthd {

namespace {

void add_stats(RectificationStats& destination,
               const RectificationStats& source) {
  destination.input_transfer_ms += source.input_transfer_ms;
  destination.compute_ms += source.compute_ms;
  destination.output_transfer_ms += source.output_transfer_ms;
  destination.total_ms += source.total_ms;
}

}  // namespace

void RectificationBackend::rectify_pair(
    const ImageView& left_input, const ImageView& right_input,
    const MutableImageView& left_output,
    const MutableImageView& right_output, RectificationStats& stats) {
  RectificationStats left_stats;
  RectificationStats right_stats;
  rectify(RectificationCamera::kLeft, left_input, left_output, left_stats);
  rectify(RectificationCamera::kRight, right_input, right_output, right_stats);
  stats = {};
  add_stats(stats, left_stats);
  add_stats(stats, right_stats);
}

void RectificationBackendRegistry::register_backend(
    std::string name, RectificationBackendFactory factory) {
  if (name.empty()) {
    throw std::invalid_argument("rectification backend name cannot be empty");
  }
  if (!factory) {
    throw std::invalid_argument("rectification backend factory is empty");
  }
  const auto [unused, inserted] =
      factories_.emplace(std::move(name), std::move(factory));
  static_cast<void>(unused);
  if (!inserted) {
    throw std::invalid_argument("rectification backend is already registered");
  }
}

bool RectificationBackendRegistry::contains(
    std::string_view name) const {
  return factories_.find(std::string(name)) != factories_.end();
}

std::unique_ptr<RectificationBackend> RectificationBackendRegistry::create(
    std::string_view requested_backend, std::string_view fallback_backend,
    bool allow_fallback, const RectificationBackendConfig& config,
    const StereoRectificationMaps& maps) const {
  const auto requested = factories_.find(std::string(requested_backend));
  if (requested != factories_.end()) {
    return requested->second(config, maps);
  }

  if (allow_fallback && requested_backend != fallback_backend) {
    const auto fallback = factories_.find(std::string(fallback_backend));
    if (fallback != factories_.end()) {
      return fallback->second(config, maps);
    }
  }

  throw std::runtime_error("requested rectification backend is not available");
}

bool is_valid(const RectificationMap& map) noexcept {
  if (map.width == 0 || map.height == 0) {
    return false;
  }
  const auto pixel_count = static_cast<std::size_t>(map.width) * map.height;
  return map.source_x.size() == pixel_count &&
         map.source_y.size() == pixel_count;
}

bool is_valid(const StereoRectificationMaps& maps) noexcept {
  return is_valid(maps.left) && is_valid(maps.right) &&
         maps.left.width == maps.right.width &&
         maps.left.height == maps.right.height;
}

}  // namespace depthd
