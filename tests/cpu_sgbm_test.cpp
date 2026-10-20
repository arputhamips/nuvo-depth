#include "depthd/stereo_backend.hpp"
#include "depthd/configuration_revision.hpp"
#include "depthd/opencv_execution.hpp"
#include "depthd/opencv_rectification_backend.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <string_view>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

int main() {
  constexpr int width = 728;
  constexpr int height = 544;
  constexpr int expected_disparity = 24;
  cv::Mat left_storage(height, width + 17, CV_8UC1);
  cv::Mat left = left_storage(cv::Rect(0, 0, width, height));
  cv::RNG random(20260712);
  random.fill(left, cv::RNG::UNIFORM, 0, 256);
  cv::Mat right_storage = cv::Mat::zeros(height, width + 23, CV_8UC1);
  cv::Mat right = right_storage(cv::Rect(0, 0, width, height));
  left(cv::Rect(expected_disparity, 0, width - expected_disparity, height))
      .copyTo(right(cv::Rect(0, 0, width - expected_disparity, height)));

  depthd::CpuSgbmConfig config;
  config.working_min_disparity = 8;
  config.accepted_min_disparity_px = 8.0F;
  config.opencv_threads = 1;

  auto native_backend = depthd::make_cpu_sgbm_native_backend(config);
  const auto native_metadata = native_backend->metadata();
  if (native_backend->name() != std::string_view("opencv_sgbm_cpu") ||
      native_metadata.grid.width != 364U ||
      native_metadata.grid.height != 272U ||
      native_metadata.grid.reference_width != 728U ||
      native_metadata.grid.x_scale_to_reference != 2.0 ||
      native_metadata.accepted_min_disparity_px != 8.0F ||
      native_metadata.accepted_max_disparity_px != 64.0F ||
      native_metadata.algorithmic_quantization_px != 1.0F / 16.0F ||
      native_metadata.confidence_model !=
          depthd::ConfidenceModel::kBinarySgbmSupportV1 ||
      native_metadata.derivation !=
          depthd::DisparityDerivation::kNativeMatcherGrid) {
    return 1;
  }

  const auto native_pixels =
      static_cast<std::size_t>(native_metadata.grid.width) *
      native_metadata.grid.height;
  std::vector<float> native_disparity(
      native_pixels, std::numeric_limits<float>::quiet_NaN());
  std::vector<std::uint8_t> native_confidence(native_pixels);
  std::vector<std::uint8_t> native_validity(native_pixels);
  const depthd::ImageView left_view{
      left.ptr<std::uint8_t>(), width, height, left.step,
      depthd::ImageEncoding::kGray8};
  const depthd::ImageView right_view{
      right.ptr<std::uint8_t>(), width, height, right.step,
      depthd::ImageEncoding::kGray8};
  const depthd::MutableDisparityProductsView native_output{
      {native_disparity.data(), native_metadata.grid,
       native_metadata.grid.width},
      {native_confidence.data(), native_metadata.grid,
       native_metadata.grid.width},
      {native_validity.data(), native_metadata.grid,
       native_metadata.grid.width}};
  depthd::StereoBackendStats native_stats;
  native_backend->process_native(left_view, right_view, native_output,
                                 native_stats);

  std::vector<float> native_central;
  for (std::uint32_t y = 40U; y < native_metadata.grid.height - 40U; ++y) {
    for (std::uint32_t x = 110U; x < native_metadata.grid.width - 40U; ++x) {
      const auto index = static_cast<std::size_t>(y) *
                             native_metadata.grid.width +
                         x;
      const auto valid = native_validity[index] ==
                         static_cast<std::uint8_t>(
                             depthd::ValidityCode::kValid);
      if (valid) {
        if (!std::isfinite(native_disparity[index]) ||
            native_confidence[index] != 255U) {
          return 2;
        }
        native_central.push_back(native_disparity[index]);
      } else if (!std::isnan(native_disparity[index]) ||
                 native_confidence[index] != 0U) {
        return 3;
      }
    }
  }
  if (native_central.empty()) {
    return 4;
  }
  const auto native_middle =
      native_central.begin() +
      static_cast<std::ptrdiff_t>(native_central.size() / 2U);
  std::nth_element(native_central.begin(), native_middle,
                   native_central.end());
  if (std::abs(*native_middle - expected_disparity / 2.0F) > 0.75F ||
      native_stats.valid_fraction < 0.45F ||
      native_stats.searchable_valid_fraction < 0.65F) {
    return 5;
  }
  for (std::uint32_t y = 0; y < native_metadata.grid.height; ++y) {
    for (std::uint32_t x = 0; x < native_metadata.valid_roi.x; ++x) {
      const auto index = static_cast<std::size_t>(y) *
                             native_metadata.grid.width +
                         x;
      if (native_validity[index] != static_cast<std::uint8_t>(
                                        depthd::ValidityCode::kOutsideRoi) ||
          !std::isnan(native_disparity[index])) {
        return 6;
      }
    }
  }

  auto backend = depthd::make_cpu_sgbm_backend(config);
  if (backend->name() != std::string_view("opencv_sgbm_cpu")) {
    return 7;
  }

  std::vector<float> disparity(static_cast<std::size_t>(width * height),
                               std::numeric_limits<float>::quiet_NaN());
  const depthd::MutableDisparityView output{disparity.data(), width, height,
                                            width};
  depthd::StereoBackendStats stats;
  backend->process(left_view, right_view, output, stats);

  std::vector<float> central;
  for (int y = 80; y < height - 80; ++y) {
    for (int x = 220; x < width - 80; ++x) {
      const float value = disparity[static_cast<std::size_t>(y * width + x)];
      if (std::isfinite(value)) {
        central.push_back(value);
      }
    }
  }
  if (central.empty()) {
    return 8;
  }
  const auto middle = central.begin() + static_cast<std::ptrdiff_t>(central.size() / 2U);
  std::nth_element(central.begin(), middle, central.end());
  if (std::abs(*middle - expected_disparity) > 1.0F) {
    return 9;
  }
  if (stats.valid_fraction < 0.50F ||
      stats.searchable_valid_fraction < 0.70F || stats.total_ms <= 0.0 ||
      stats.match_ms <= 0.0) {
    return 10;
  }
  // The runtime control file switches algorithms without changing the native
  // geometry or reconstructing the camera/rectification pipeline.
  const auto control = std::filesystem::temp_directory_path() /
                       "nuvo-stereo-control-test.json";
  const auto write_control = [&](const char* algorithm, const int maximum,
                                 const int threads = 1) {
    std::ofstream stream(control, std::ios::trunc);
    stream << "{\"schema_version\":1,\"algorithm\":\"" << algorithm
           << "\",\"sgbm_mode\":\"3way\",\"min_disparity\":0,\"search_range\":128,"
              "\"max_disparity\":" << maximum << ",\"block_size\":9,"
              "\"uniqueness_ratio\":1,\"speckle_window_size\":30,"
              "\"speckle_range\":2,\"disparity_12_max_diff\":1,"
              "\"pre_filter_cap\":31,\"bm_pre_filter_size\":9,"
              "\"bm_texture_threshold\":10,\"specular_threshold\":256,"
              "\"median_filter_size\":0,"
              "\"opencv_threads\":" << threads << '}';
    stream.close();
  };
  write_control("bm", 96);
  depthd::CpuSgbmConfig full;
  full.working_width = width;
  full.working_height = height;
  full.working_disparities = 128;
  full.accepted_max_disparity_px = 128.0F;
  full.control_file = control.string();
  auto selectable = depthd::make_cpu_sgbm_native_backend(full);
  if (selectable->name() != "opencv_bm_cpu" ||
      selectable->metadata().grid.width != width ||
      selectable->metadata().grid.height != height ||
      selectable->metadata().confidence_model !=
          depthd::ConfidenceModel::kBinaryBmSupportV1 ||
      selectable->metadata().accepted_max_disparity_px != 96.0F) return 11;
  const auto initial_state = selectable->configuration_state();
  if (initial_state.generation != 2U || initial_state.applied_revision.size() != 64U ||
      initial_state.requested_revision != initial_state.applied_revision ||
      !initial_state.rejection_error.empty()) return 15;
  const auto full_metadata = selectable->metadata();
  std::vector<float> full_disparity(static_cast<std::size_t>(width * height));
  std::vector<std::uint8_t> full_confidence(full_disparity.size());
  std::vector<std::uint8_t> full_validity(full_disparity.size());
  const depthd::MutableDisparityProductsView full_output{
      {full_disparity.data(), full_metadata.grid, width},
      {full_confidence.data(), full_metadata.grid, width},
      {full_validity.data(), full_metadata.grid, width}};
  depthd::StereoBackendStats bm_stats;
  selectable->process_native(left_view, right_view, full_output, bm_stats);
  if (bm_stats.valid_fraction < 0.20F ||
      !std::isfinite(full_disparity[200U * width + 300U]) ||
      std::abs(full_disparity[200U * width + 300U] -
               expected_disparity) > 1.0F) return 12;
  write_control("sgbm", 128);
  const auto observed_time = std::filesystem::last_write_time(control);
  std::filesystem::last_write_time(control, observed_time +
      std::chrono::seconds(1));
  selectable->refresh_configuration();
  if (selectable->name() != "opencv_sgbm_cpu" ||
      selectable->metadata().accepted_max_disparity_px != 128.0F) return 13;
  const auto changed_state = selectable->configuration_state();
  if (changed_state.generation != initial_state.generation + 1U ||
      changed_state.applied_revision == initial_state.applied_revision ||
      changed_state.applied_revision != changed_state.requested_revision) return 16;
  // Changing serialization alone acknowledges the revision but does not rebuild
  // effective controls or advance the processing configuration epoch.
  { std::ofstream stream(control, std::ios::app); stream << "\n "; }
  std::filesystem::last_write_time(control, observed_time + std::chrono::seconds(2));
  selectable->refresh_configuration();
  const auto equal_state = selectable->configuration_state();
  if (changed_state.effective_revision == initial_state.effective_revision ||
      equal_state.effective_revision != changed_state.effective_revision ||
      equal_state.effective_revision.size() != 64U) return 20;
  if (equal_state.generation != changed_state.generation ||
      equal_state.applied_revision == changed_state.applied_revision ||
      equal_state.applied_revision != equal_state.requested_revision ||
      !equal_state.rejection_error.empty()) return 17;
  write_control("bm", 200);
  std::filesystem::last_write_time(control, observed_time +
      std::chrono::seconds(3));
  selectable->refresh_configuration();
  if (selectable->name() != "opencv_sgbm_cpu" ||
      selectable->metadata().accepted_max_disparity_px != 128.0F) return 14;
  const auto rejected_state = selectable->configuration_state();
  if (rejected_state.generation != equal_state.generation ||
      rejected_state.applied_revision != equal_state.applied_revision ||
      rejected_state.requested_revision == rejected_state.applied_revision ||
      rejected_state.rejection_error.empty()) return 18;
  // A malformed request is also rejected without disturbing a usable matcher.
  { std::ofstream stream(control, std::ios::trunc); stream << "invalid JSON"; }
  std::filesystem::last_write_time(control, observed_time + std::chrono::seconds(4));
  selectable->refresh_configuration();
  if (selectable->configuration_state().generation != equal_state.generation ||
      selectable->configuration_state().applied_revision != equal_state.applied_revision ||
      selectable->configuration_state().rejection_error.empty()) return 19;
  selectable->process_native(left_view,right_view,full_output,bm_stats);
  write_control("sgbm", 128);
  std::filesystem::last_write_time(control, observed_time + std::chrono::seconds(5));
  selectable->refresh_configuration();
  if (selectable->configuration_state().generation != equal_state.generation ||
      !selectable->configuration_state().rejection_error.empty()) return 20;
  // A dormant BM-only control does not change an active SGBM matcher.
  { std::ifstream stream(control); std::string bytes((std::istreambuf_iterator<char>(stream)), {});
    const auto key = bytes.find("\"bm_texture_threshold\":10");
    if (key == std::string::npos) return 21;
    bytes.replace(key, std::string("\"bm_texture_threshold\":10").size(),
                  "\"bm_texture_threshold\":11");
    std::ofstream output(control, std::ios::trunc); output << bytes;
  }
  std::filesystem::last_write_time(control, observed_time + std::chrono::seconds(6));
  selectable->refresh_configuration();
  if (selectable->configuration_state().generation != equal_state.generation ||
      !selectable->configuration_state().rejection_error.empty()) return 22;

  // A private color preview rectifier shares OpenCV's process-global thread
  // count with SGBM. Compute guards permit simultaneous readers; live thread
  // changes must wait for an in-flight operation before changing that count.
  depthd::RectificationMap preview_map;
  preview_map.width = width;
  preview_map.height = height;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      preview_map.source_x.push_back(static_cast<float>(x));
      preview_map.source_y.push_back(static_cast<float>(y));
    }
  }
  depthd::RectificationBackendConfig preview_config;
  preview_config.input_width = preview_config.output_width = width;
  preview_config.input_height = preview_config.output_height = height;
  depthd::RectificationBackendRegistry registry;
  depthd::register_opencv_rectification_backend(registry);
  auto preview = registry.create("cpu_opencv", "", false, preview_config,
                                 {preview_map, preview_map});
  std::vector<std::uint8_t> preview_input(static_cast<std::size_t>(width) * height * 3U, 53U);
  auto preview_output = preview_input;
  std::atomic<std::uint64_t> previews_completed{};
  std::atomic<bool> preview_correct{true};
  std::jthread preview_worker([&](const std::stop_token stop) {
    while (!stop.stop_requested()) {
      depthd::RectificationStats preview_stats;
      preview->rectify(depthd::RectificationCamera::kLeft,
          {preview_input.data(), width, height, width * 3U, depthd::ImageEncoding::kRgb8},
          {preview_output.data(), width, height, width * 3U, depthd::ImageEncoding::kRgb8},
          preview_stats);
      if (preview_output != preview_input) preview_correct = false;
      ++previews_completed;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  auto simultaneous_compute = depthd::opencv_execution::compute_guard();
  const auto preview_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (previews_completed == 0U && std::chrono::steady_clock::now() < preview_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (previews_completed == 0U) return 23;

  write_control("sgbm", 128, 2);
  std::filesystem::last_write_time(control, observed_time + std::chrono::seconds(7));
  std::promise<void> writer_started;
  auto started = writer_started.get_future();
  auto update = std::async(std::launch::async, [&] {
    writer_started.set_value();
    selectable->refresh_configuration();
  });
  started.wait();
  const auto writer_blocked = update.wait_for(std::chrono::milliseconds(30)) ==
                              std::future_status::timeout;
  simultaneous_compute.unlock();
  update.get();
  if (!writer_blocked) return 24;
  for (const int threads : {2, 1, 2}) {
    write_control("sgbm", 128, threads);
    const auto tick = threads == 1 ? 8 : 9;
    std::filesystem::last_write_time(control, observed_time + std::chrono::seconds(tick));
    selectable->refresh_configuration();
    {
      const auto execution = depthd::opencv_execution::compute_guard();
      if (cv::getNumThreads() != threads) return 25;
    }
    const auto before = previews_completed.load();
    selectable->process_native(left_view, right_view, full_output, bm_stats);
    if (previews_completed <= before || !preview_correct ||
        !selectable->configuration_state().rejection_error.empty()) return 26;
  }
  preview_worker.request_stop();
  preview_worker.join();
  // Full-grid gray views alias immutable padded input only for this call.
  // Colour conversion, resize scratch, and lazy compatibility storage must
  // remain independent when one backend alternates these paths.
  cv::Mat color_left, color_right;
  cv::cvtColor(left, color_left, cv::COLOR_GRAY2BGR);
  cv::cvtColor(right, color_right, cv::COLOR_GRAY2BGR);
  const auto saved_left = left.clone(), saved_right = right.clone();
  for (bool half : {false, true}) {
    auto mixed_config = full;
    mixed_config.control_file.clear();
    mixed_config.opencv_threads = 2;
    if (half) { mixed_config.working_width = width / 2; mixed_config.working_height = height / 2; }
    auto mixed = depthd::make_cpu_sgbm_native_backend(mixed_config);
    const auto grid = mixed->metadata().grid;
    std::vector<float> disparity(grid.width * grid.height);
    std::vector<std::uint8_t> confidence(disparity.size()), validity(disparity.size());
    const depthd::MutableDisparityProductsView output{
        {disparity.data(), grid, grid.width}, {confidence.data(), grid, grid.width}, {validity.data(), grid, grid.width}};
    mixed->process_native(left_view,right_view,output,bm_stats);
    const auto baseline = disparity; const auto baseline_validity = validity;
    auto* compatible = dynamic_cast<depthd::StereoBackend*>(mixed.get());
    if (!compatible) return 27;
    const depthd::ImageView color_l{color_left.data,width,height,color_left.step,depthd::ImageEncoding::kBgr8};
    const depthd::ImageView color_r{color_right.data,width,height,color_right.step,depthd::ImageEncoding::kBgr8};
    mixed->process_native(color_l,color_r,output,bm_stats);
    std::vector<float> legacy(width * height);
    compatible->process(color_l,color_r,{legacy.data(),width,height,width},bm_stats);
    mixed->process_native(left_view,right_view,output,bm_stats);
    if (validity != baseline_validity) return 28;
    for (std::size_t index = 0; index < disparity.size(); ++index)
      if (!(disparity[index] == baseline[index] || (std::isnan(disparity[index]) && std::isnan(baseline[index])))) return 29;
    if (cv::norm(left,saved_left,cv::NORM_INF) != 0 || cv::norm(right,saved_right,cv::NORM_INF) != 0) return 30;
  }
  std::filesystem::remove(control);
  return 0;
}
