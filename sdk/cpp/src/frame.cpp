#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

#include "depthd/wire_protocol.hpp"
#include "nuvo/client.hpp"

namespace nuvo::client {
namespace {
using Json = nlohmann::json;
constexpr std::array<std::string_view, 14> names{
    "left_rectified",     "right_rectified",
    "disparity",          "depth",
    "confidence",         "validity",
    "left_mono",          "right_mono",
    "left_rgb",           "right_rgb",
    "left_rectified_rgb", "right_rectified_rgb",
    "rectified_stereo",   "rectified_stereo_rgb"};
void require(bool condition, const char* message) {
  if (!condition) throw std::invalid_argument(message);
}
Json parse(std::string_view text) {
  std::vector<std::set<std::string>> keys(26);
  return Json::parse(text,
                     [&](int depth, Json::parse_event_t event, Json& value) {
                       require(depth < 24, "metadata nesting exceeds bound");
                       if (event == Json::parse_event_t::object_start)
                         keys.at(static_cast<std::size_t>(depth + 1)).clear();
                       if (event == Json::parse_event_t::key)
                         require(keys.at(static_cast<std::size_t>(depth))
                                     .insert(value.get<std::string>())
                                     .second,
                                 "duplicate metadata key");
                       return true;
                     });
}
std::uint64_t integer(const Json& object, const char* key,
                      std::uint64_t maximum, bool nonzero = true) {
  const auto& value = object.at(key);
  require(value.is_number_unsigned() ||
              (value.is_number_integer() && value.get<std::int64_t>() >= 0),
          "invalid unsigned integer");
  const auto result = value.get<std::uint64_t>();
  require(result <= maximum && (!nonzero || result != 0),
          "integer outside contract bounds");
  return result;
}
double number(const Json& object, const char* key) {
  const auto& value = object.at(key);
  require(value.is_number(), "metadata value must be numeric");
  const auto result = value.get<double>();
  require(std::isfinite(result), "metadata number must be finite");
  return result;
}
std::string hex(const Json& object, const char* key, std::size_t size) {
  auto value = object.at(key).get<std::string>();
  require(value.size() == size &&
              value.find_first_not_of("0123456789abcdef") == std::string::npos,
          "invalid identity hash or UUID");
  require(value.find_first_not_of('0') != std::string::npos,
          "zero identity hash or UUID");
  return value;
}
Grid grid(const Json& value) {
  return {static_cast<std::uint32_t>(integer(value, "width", 16384)),
          static_cast<std::uint32_t>(integer(value, "height", 16384))};
}
template <std::size_t N>
std::array<double, N> matrix(const Json& value) {
  require(value.is_array() && value.size() == N,
          "invalid geometry matrix shape");
  std::array<double, N> result{};
  for (std::size_t i = 0; i < N; ++i) {
    require(value[i].is_number(), "geometry must be numeric");
    result[i] = value[i].get<double>();
    require(std::isfinite(result[i]), "nonfinite geometry");
  }
  return result;
}
bool near(double a, double b) {
  return std::isfinite(a) && std::isfinite(b) &&
         std::abs(a - b) <= 1e-7 * std::max({1.0, std::abs(a), std::abs(b)});
}
bool near_relative(double a, double b) {
  return std::isfinite(a) && std::isfinite(b) &&
         std::abs(a - b) <= 1e-7 * std::max(std::abs(a), std::abs(b));
}
struct GridMapping {
  Grid dimensions;
  std::uint32_t reference_width{}, reference_height{};
  double sx{}, sy{}, ox{}, oy{};
};
GridMapping grid_mapping(const Json& j) {
  GridMapping result{
      grid(j),
      static_cast<std::uint32_t>(integer(j, "reference_width", 16384)),
      static_cast<std::uint32_t>(integer(j, "reference_height", 16384)),
      number(j, "x_scale_to_reference"), number(j, "y_scale_to_reference"),
      number(j, "x_offset_to_reference"), number(j, "y_offset_to_reference")};
  require(std::isfinite(result.sx) && std::isfinite(result.sy) &&
              std::isfinite(result.ox) && std::isfinite(result.oy) &&
              result.sx > 0 && result.sy > 0,
          "invalid pixel-grid mapping");
  const auto last_x =
      result.ox + static_cast<double>(result.dimensions.width - 1U) * result.sx;
  const auto last_y =
      result.oy +
      static_cast<double>(result.dimensions.height - 1U) * result.sy;
  require(result.ox >= -.5 - 1e-9 && result.oy >= -.5 - 1e-9 &&
              last_x <= result.reference_width - .5 + 1e-9 &&
              last_y <= result.reference_height - .5 + 1e-9,
          "pixel-grid mapping extends beyond reference image");
  return result;
}
Geometry geometry(const Json& value, Grid dimensions) {
  Geometry result{dimensions, matrix<12>(value.at("left_projection")),
                  matrix<12>(value.at("right_projection")),
                  matrix<16>(value.at("reprojection")),
                  number(value, "baseline_m")};
  const auto& l = result.left_projection;
  const auto& r = result.right_projection;
  require(std::isfinite(result.baseline_m) && result.baseline_m > 0 &&
              l[0] > 0 && l[5] > 0,
          "invalid calibrated focal length or baseline");
  for (const auto* p : {&l, &r})
    require(near((*p)[1], 0) && near((*p)[4], 0) && near((*p)[7], 0) &&
                near((*p)[8], 0) && near((*p)[9], 0) && near((*p)[10], 1) &&
                near((*p)[11], 0),
            "unsupported noncanonical rectified geometry");
  require(near(l[3], 0) && near_relative(l[0], r[0]) &&
              near_relative(l[5], r[5]) &&
              near(l[6], r[6]) && r[3] < 0 &&
              near_relative(-r[3] / r[0], result.baseline_m),
          "inconsistent stereo projections");
  const auto b = result.baseline_m;
  const std::array<double, 16> q{
      1, 0, 0, -l[2], 0, 1, 0,     -l[6],
      0, 0, 0, l[0],  0, 0, 1 / b, -(l[2] - r[2]) / b};
  for (std::size_t i = 0; i < q.size(); ++i)
    require(near(q[i], result.reprojection[i]), "Q disagrees with projections");
  return result;
}

// One bounded pass over the selected matching products, without allocating or
// rewriting samples. Invalid depth is NaN; invalid disparity may remain finite
// when a later depth-range check rejected an otherwise valid matcher result.
void validate_measurements(const Frame& frame) {
  const auto* depth = frame.find("depth");
  const auto* disparity = frame.find("disparity");
  const auto* validity = frame.find("validity");
  const auto* confidence = frame.find("confidence");
  const auto* reference = depth ? depth : disparity ? disparity : validity ? validity : confidence;
  if (!reference) return;
  for (const auto* product : {depth, disparity, validity, confidence})
    if (product)
      require(product->grid.width == reference->grid.width &&
                  product->grid.height == reference->grid.height,
              "matching product grids differ");
  if (frame.matching_geometry)
    require(reference->grid.width == frame.matching_geometry->grid.width &&
                reference->grid.height == frame.matching_geometry->grid.height,
            "measurement grid differs from calibration");
  const auto count = static_cast<std::size_t>(reference->grid.width) * reference->grid.height;
  const auto nan = std::numeric_limits<float>::quiet_NaN();
  const auto offset = frame.matching_geometry
                          ? frame.matching_geometry->left_projection[2] -
                                frame.matching_geometry->right_projection[2]
                          : 0.0;
  const auto focal_baseline = frame.matching_geometry
                                  ? frame.matching_geometry->left_projection[0] *
                                        frame.matching_geometry->baseline_m
                                  : 0.0;
  if (frame.matching_geometry && (depth || disparity))
    require(std::isfinite(offset) && std::isfinite(focal_baseline) && focal_baseline > 0,
            "unrepresentable stereo focal length, baseline or principal offset");
  for (std::size_t i = 0; i < count; ++i) {
    const auto z = depth ? float32_le(depth->bytes.data() + 4 * i) : nan;
    const auto d = disparity ? float32_le(disparity->bytes.data() + 4 * i) : nan;
    require(!std::isinf(z) && !std::isinf(d), "infinite measurement");
    const auto code = validity ? std::to_integer<unsigned>(validity->bytes[i]) : 0U;
    require(code <= 11, "unknown validity code");
    const bool accepted = validity ? code == 1 : depth ? std::isfinite(z) : std::isfinite(d);
    if (!accepted) {
      if (depth) require(std::isnan(z), "invalid depth must be NaN");
      continue;
    }
    if (depth) {
      require(std::isfinite(z) && z > 0, "accepted depth must be finite and positive");
      if (frame.depth_constraints)
        require(z >= frame.depth_constraints->minimum_m && z <= frame.depth_constraints->maximum_m,
                "accepted depth exceeds declared range");
    }
    if (disparity)
      require(std::isfinite(d), "accepted disparity must be finite");
    if (frame.disparity_constraints) {
      const auto& constraints = *frame.disparity_constraints;
      const auto x = i % reference->grid.width, y = i / reference->grid.width;
      const auto& roi = constraints.valid_roi;
      require(x >= roi.x && x < roi.x + roi.width && y >= roi.y && y < roi.y + roi.height,
              "accepted measurement exceeds valid ROI");
      if (disparity)
        require(d >= constraints.accepted_min_px && d < constraints.accepted_max_px,
                "accepted disparity exceeds declared range");
    }
    if (frame.matching_geometry && (depth || disparity)) {
      if (disparity) {
        const auto effective = static_cast<double>(d) - offset;
        require(effective > 0, "accepted effective disparity must be positive");
        if (depth) {
          const auto expected = focal_baseline / effective;
          require(std::isfinite(expected) &&
                      std::abs(z - expected) <= 1e-4 * std::max(1.0, expected),
                  "accepted depth/disparity/calibration disagree");
        }
      } else if (depth && frame.disparity_constraints) {
        const auto inferred = focal_baseline / z + offset;
        const auto& constraints = *frame.disparity_constraints;
        // Depth was rounded to float32 by the producer. Its inferred disparity
        // can straddle an interval endpoint by that rounding error.
        const auto tolerance = 1e-6 * std::max({1.0, std::abs(inferred), std::abs(offset)});
        require(std::isfinite(inferred) && inferred >= constraints.accepted_min_px - tolerance &&
                    inferred < constraints.accepted_max_px + tolerance,
                "accepted depth contradicts declared disparity range");
      }
    }
  }
}
}  // namespace
const Product* Frame::find(std::string_view kind) const noexcept {
  const auto it = std::find_if(products.begin(), products.end(),
                               [&](const auto& p) { return p.kind == kind; });
  return it == products.end() ? nullptr : &*it;
}
float float32_le(const std::byte* data) noexcept {
  std::uint32_t value{};
  for (unsigned i = 0; i < 4; ++i)
    value |= std::to_integer<std::uint32_t>(data[i]) << (8U * i);
  return std::bit_cast<float>(value);
}
Frame decode_frame(std::span<const std::byte> bytes,
                   std::shared_ptr<const void> owner) {
  using namespace depthd::wire;
  require(bytes.size() >= kHeaderBytes && bytes.size() <= maximum_wire_bytes,
          "frame size outside bounds");
  const auto h = decode_header(bytes.first(kHeaderBytes));
  require(h.kind == MessageKind::kFrameSet &&
              bytes.size() == kHeaderBytes + h.metadata_bytes + h.payload_bytes,
          "invalid complete frame length or kind");
  if (!owner) {
    auto copy = std::make_shared<const std::vector<std::byte>>(bytes.begin(),
                                                               bytes.end());
    bytes = *copy;
    owner = copy;
  }
  Frame result;
  result.owner = std::move(owner);
  result.wire_bytes = bytes;
  result.stream_sequence = h.stream_sequence;
  result.metadata_json.assign(
      reinterpret_cast<const char*>(bytes.data() + kHeaderBytes),
      h.metadata_bytes);
  const auto j = parse(result.metadata_json);
  require(integer(j, "schema_version", 1) == 1, "unsupported metadata schema");
  result.identity.device_uuid = hex(j, "device_uuid", 32);
  result.identity.run_uuid = hex(j, "run_uuid", 32);
  result.identity.capture_epoch = integer(j, "capture_epoch", UINT64_MAX);
  result.identity.capture_sequence = integer(j, "capture_sequence", UINT64_MAX);
  result.identity.configuration_epoch =
      integer(j, "configuration_epoch", UINT64_MAX);
  result.capture_time_ns =
      static_cast<std::int64_t>(integer(j, "capture_time_ns", INT64_MAX));
  result.capture_clock = j.at("capture_clock").get<std::string>();
  require(result.capture_clock == "boottime", "unsupported capture clock");
  result.calibration_sha256 = hex(j, "calibration_sha256", 64);
  result.calibration_authority =
      static_cast<unsigned>(integer(j, "calibration_authority", 2, false));
  const auto payload = bytes.subspan(kHeaderBytes + h.metadata_bytes);
  const auto& descriptors = j.at("products");
  require(descriptors.is_array() && !descriptors.empty() &&
              descriptors.size() <= 14,
          "invalid product count");
  std::vector<std::pair<std::size_t, std::size_t>> ranges;
  ranges.reserve(descriptors.size());
  result.products.reserve(descriptors.size());
  std::uint64_t mask{};
  for (const auto& d : descriptors) {
    Product p;
    p.kind = d.at("kind").get<std::string>();
    p.format = d.at("format").get<std::string>();
    p.grid = grid(d);
    const auto it = std::find(names.begin(), names.end(), p.kind);
    require(it != names.end(), "unknown product");
    const auto index = static_cast<unsigned>(it - names.begin());
    require((mask & (1ULL << index)) == 0, "repeated product");
    mask |= 1ULL << index;
    const bool numeric = index == 2 || index == 3;
    const bool rgb =
        index == 8 || index == 9 || index == 10 || index == 11 || index == 13;
    const bool legacy = index < 2;
    require(
        numeric ? p.format == "float32_le"
        : rgb   ? p.format == "rgb8"
        : legacy
            ? (p.format == "gray8" || p.format == "rgb8" || p.format == "bgr8")
            : (p.format == (index == 4 || index == 5 ? "uint8" : "gray8")),
        "invalid product format");
    const auto channels = numeric ? 4U
                          : (rgb || p.format == "rgb8" || p.format == "bgr8")
                              ? 3U
                              : 1U;
    const auto offset = integer(d, "offset", payload.size(), false),
               length = integer(d, "length", payload.size());
    require(length == static_cast<std::uint64_t>(p.grid.width) * p.grid.height *
                          channels &&
                offset <= payload.size() - length,
            "product exceeds payload or disagrees with grid");
    for (const auto& [start, end] : ranges)
      require(offset >= end || offset + length <= start,
              "overlapping products");
    ranges.emplace_back(offset, offset + length);
    p.bytes = payload.subspan(offset, length);
    result.products.push_back(p);
  }
  std::sort(ranges.begin(), ranges.end());
  std::size_t next{};
  for (const auto& [start, end] : ranges) {
    require(start == next, "payload contains undeclared bytes");
    next = end;
  }
  require(next == payload.size(), "payload contains undeclared bytes");
  if (j.contains("selected_products"))
    require(integer(j, "selected_products", 16383) == mask,
            "selected mask disagrees with descriptors");
  if (j.contains("available_products"))
    require((integer(j, "available_products", 16383) & mask) == mask,
            "available mask disagrees with descriptors");
  const bool measurements = (mask & 60U) != 0;
  if (measurements) {
    require(j.value("depth_unit", std::string{}) == "metres" &&
                j.value("depth_convention", std::string{}) == "optical_axis_z",
            "unsupported depth units or convention");
  }
  if (j.contains("stereo_configuration_error")) {
    const auto& error = j.at("stereo_configuration_error");
    require(error.is_string(), "stereo configuration error must be text");
    if (measurements)
      require(error.get_ref<const std::string&>().empty(), "producer stereo configuration rejected");
  }
  std::optional<GridMapping> image_grid, matching_grid;
  if (j.contains("image_grid")) image_grid = grid_mapping(j.at("image_grid"));
  if (j.contains("matching_grid"))
    matching_grid = grid_mapping(j.at("matching_grid"));
  for (const auto& p : result.products) {
    const bool numeric = p.kind == "depth" || p.kind == "validity" ||
                         p.kind == "disparity" || p.kind == "confidence";
    const bool stereo =
        p.kind == "rectified_stereo" || p.kind == "rectified_stereo_rgb";
    const auto& selected = numeric ? matching_grid : image_grid;
    if (selected)
      require(p.grid.width == selected->dimensions.width * (stereo ? 2U : 1U) &&
                  p.grid.height == selected->dimensions.height,
              "product differs from declared pixel grid");
  }
  if (j.contains("calibration_geometry")) {
    const auto& g = j.at("calibration_geometry");
    require(image_grid && matching_grid,
            "geometry requires both calibrated pixel grids");
    result.image_geometry = geometry(g.at("image"), grid(j.at("image_grid")));
    result.matching_geometry =
        geometry(g.at("matching"), grid(j.at("matching_grid")));
    require(
        image_grid->reference_width == matching_grid->reference_width &&
            image_grid->reference_height == matching_grid->reference_height &&
            near_relative(result.image_geometry->baseline_m,
                          result.matching_geometry->baseline_m),
        "calibrated views disagree on reference grid or baseline");
    for (unsigned side = 0; side < 2; ++side) {
      auto expected = side ? result.image_geometry->right_projection
                           : result.image_geometry->left_projection;
      expected[0] *= image_grid->sx / matching_grid->sx;
      expected[2] =
          (expected[2] * image_grid->sx + image_grid->ox - matching_grid->ox) /
          matching_grid->sx;
      expected[3] *= image_grid->sx / matching_grid->sx;
      expected[5] *= image_grid->sy / matching_grid->sy;
      expected[6] =
          (expected[6] * image_grid->sy + image_grid->oy - matching_grid->oy) /
          matching_grid->sy;
      const auto& actual = side ? result.matching_geometry->right_projection
                                : result.matching_geometry->left_projection;
      for (std::size_t i = 0; i < 12; ++i)
        require(near(expected[i], actual[i]),
                "calibration projections disagree with pixel-grid mapping");
    }
  }
  if (measurements && j.contains("depth_metadata")) {
    const auto& value = j.at("depth_metadata");
    DepthConstraints constraints{number(value, "minimum_m"), number(value, "maximum_m")};
    require(value.at("invalid_is_nan").is_boolean() && value.at("invalid_is_nan") == true &&
                constraints.minimum_m > 0 && constraints.maximum_m > constraints.minimum_m,
            "invalid declared depth constraints");
    result.depth_constraints = constraints;
  }
  if (measurements && j.contains("disparity_metadata")) {
    const auto& value = j.at("disparity_metadata");
    require(value.at("unit") == "matching_grid_pixels" &&
                value.at("invalid_is_nan").is_boolean() && value.at("invalid_is_nan") == true,
            "invalid declared disparity convention");
    DisparityConstraints constraints{number(value, "accepted_min_px"),
                                    number(value, "accepted_max_px"),
                                    number(value, "quantization_px"), {}};
    require(constraints.accepted_max_px > constraints.accepted_min_px && constraints.quantization_px > 0,
            "invalid declared disparity range or quantization");
    const auto* reference = result.find("depth");
    if (!reference) reference = result.find("disparity");
    if (!reference) reference = result.find("validity");
    if (!reference) reference = result.find("confidence");
    const auto& roi = value.at("valid_roi");
    const auto width = reference->grid.width, height = reference->grid.height;
    constraints.valid_roi = {
        static_cast<std::uint32_t>(integer(roi, "x", width, false)),
        static_cast<std::uint32_t>(integer(roi, "y", height, false)),
        static_cast<std::uint32_t>(integer(roi, "width", width)),
        static_cast<std::uint32_t>(integer(roi, "height", height))};
    require(constraints.valid_roi.width <= width - constraints.valid_roi.x &&
                constraints.valid_roi.height <= height - constraints.valid_roi.y,
            "declared valid ROI exceeds matching grid");
    result.disparity_constraints = constraints;
  }
  validate_measurements(result);
  return result;
}
void validate(const Subscription& s) {
  require(
      std::isfinite(s.cadence_hz) && s.cadence_hz >= .001 && s.cadence_hz <= 60,
      "invalid subscription cadence");
  require(s.delivery == "auto" || s.delivery == "exact" ||
              s.delivery == "best_effort",
          "invalid delivery policy");
  require(!s.products.empty() && s.products.size() <= 14,
          "invalid subscription product count");
  std::set<std::string> unique;
  for (const auto& p : s.products)
    require(std::find(names.begin(), names.end(), p) != names.end() &&
                unique.insert(p).second,
            "invalid subscription product");
}
CalibrationOverride load_calibration_override(
    const std::filesystem::path& path) {
  require(std::filesystem::file_size(path) <= 65536,
          "calibration override exceeds bound");
  std::ifstream input(path);
  std::string text((std::istreambuf_iterator<char>(input)), {});
  const auto j = parse(text);
  return {hex(j, "calibration_sha256", 64),
          geometry(j.at("matching"), grid(j.at("matching_grid")))};
}
void apply_calibration_override(Frame& f, const CalibrationOverride& c) {
  require(f.calibration_sha256 == c.sha256,
          "calibration override does not match frame hash");
  const auto* d = f.find("depth");
  require(d && d->grid.width == c.matching.grid.width &&
              d->grid.height == c.matching.grid.height,
          "override grid differs from depth");
  require(!f.matching_geometry, "override must not replace producer geometry");
  f.matching_geometry = c.matching;
  try {
    validate_measurements(f);
  } catch (...) {
    f.matching_geometry.reset();
    throw;
  }
}
}  // namespace nuvo::client
