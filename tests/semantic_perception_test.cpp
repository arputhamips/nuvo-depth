#include "depthd/semantic_perception.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct BackendControl {
  std::atomic<std::uint64_t> calls{};
  std::mutex mutex{};
  std::condition_variable condition{};
  bool entered{};
  bool release{true};
};

class FakeBackend final : public depthd::ISemanticBackend {
 public:
  FakeBackend(std::string name, depthd::SemanticSourceKind kind,
              std::shared_ptr<BackendControl> control)
      : name_(std::move(name)), kind_(kind), control_(std::move(control)) {}

  depthd::SemanticBackendInfo info() const override {
    return {"fake", name_, "test", "test", kind_};
  }

  depthd::SemanticBackendOutput infer(
      const depthd::SemanticImageView& image) override {
    if (image.bgr == nullptr || image.width != 16U || image.height != 12U) {
      throw std::runtime_error("bad test image");
    }
    ++control_->calls;
    {
      std::unique_lock lock(control_->mutex);
      control_->entered = true;
      control_->condition.notify_all();
      control_->condition.wait(lock, [&] { return control_->release; });
    }
    depthd::SemanticObservation observation;
    observation.observation_id = static_cast<std::uint32_t>(control_->calls.load());
    observation.model_name = name_;
    observation.model_label = kind_ == depthd::SemanticSourceKind::kInstance
        ? "person" : "vegetation";
    observation.semantic_class = kind_ == depthd::SemanticSourceKind::kInstance
        ? depthd::SemanticClass::kHuman
        : depthd::SemanticClass::kVegetation;
    observation.box = {2.0F, 2.0F, 14.0F, 10.0F};
    observation.confidence = 0.9F;
    depthd::SemanticBackendOutput output;
    if (kind_ == depthd::SemanticSourceKind::kInstance) {
      output.instances.push_back(std::move(observation));
    } else {
      output.dense_regions.push_back(std::move(observation));
    }
    return output;
  }

 private:
  std::string name_;
  depthd::SemanticSourceKind kind_;
  std::shared_ptr<BackendControl> control_;
};

depthd::SemanticFrame frame(const std::uint64_t sequence,
                            const std::int64_t schedule_ns) {
  depthd::SemanticFrame value;
  value.source_sequence = sequence;
  value.capture_realtime_ns = 1'780'000'000'000'000'000LL +
                              static_cast<std::int64_t>(sequence);
  value.submitted_steady_ns = schedule_ns;
  value.width = 16U;
  value.height = 12U;
  value.color_stride_bytes = 48U;
  value.left_rectified_bgr =
      std::make_shared<const std::vector<std::uint8_t>>(16U * 12U * 3U, 80U);
  value.disparity_stride_elements = 16U;
  value.disparity =
      std::make_shared<const std::vector<float>>(16U * 12U, 10.0F);
  value.min_disparity = 0.0F;
  value.max_disparity = 64.0F;
  value.camera.width = 16U;
  value.camera.height = 12U;
  value.camera.fx = 20.0;
  value.camera.fy = 20.0;
  value.camera.cx = 8.0;
  value.camera.cy = 6.0;
  value.camera.baseline_m = 1.0;
  value.optical_to_target.matrix = {
      0.0, 0.0, 1.0, 0.0,
      -1.0, 0.0, 0.0, 0.0,
      0.0, -1.0, 0.0, 0.0,
      0.0, 0.0, 0.0, 1.0,
  };
  return value;
}

}  // namespace

int main() {
  {
    auto fast = std::make_shared<BackendControl>();
    auto slow_cadence = std::make_shared<BackendControl>();
    std::vector<depthd::SemanticBackendRegistration> backends;
    backends.push_back({std::make_unique<FakeBackend>(
                            "instances", depthd::SemanticSourceKind::kInstance,
                            fast), 5.0});
    backends.push_back({std::make_unique<FakeBackend>(
                            "regions", depthd::SemanticSourceKind::kDenseRegion,
                            slow_cadence), 2.0});
    depthd::SemanticFusionConfig fusion;
    fusion.min_valid_samples = 1U;
    depthd::SemanticPerceptionConfig config;
    config.fusion = fusion;
    depthd::SemanticPerceptionModule module(std::move(backends), config);
    expect(module.submit(frame(1U, 1'000'000'000LL)),
           "first semantic frame is accepted");
    expect(module.wait_for_result_after(0U, 1s),
           "first asynchronous result completes");
    auto first = module.latest_result();
    expect(first.has_value() && first->source_sequence == 1U,
           "result preserves the depth source sequence");
    expect(first->capture_realtime_ns ==
               1'780'000'000'000'000'001LL,
           "result preserves the capture timestamp exactly");
    expect(first->models.size() == 2U && first->objects.size() == 2U,
           "instance and dense models share one semantic result");

    expect(module.submit(frame(2U, 1'250'000'000LL)),
           "second frame is accepted");
    expect(module.wait_for_result_after(1U, 1s),
           "five-hertz backend produces the second result");
    auto second = module.latest_result();
    expect(second.has_value() && second->models.size() == 1U &&
               second->objects.size() == 1U,
           "partial update contains only observations from its own frame");

    expect(module.submit(frame(3U, 1'550'000'000LL)),
           "third frame is accepted");
    expect(module.wait_for_result_after(2U, 1s),
           "both cadence clocks advance independently");
    expect(fast->calls.load() == 3U && slow_cadence->calls.load() == 2U,
           "backend invocation counts match 5 Hz and 2 Hz schedules");
  }

  {
    auto blocked = std::make_shared<BackendControl>();
    blocked->release = false;
    std::vector<depthd::SemanticBackendRegistration> backends;
    backends.push_back({std::make_unique<FakeBackend>(
                            "blocked", depthd::SemanticSourceKind::kInstance,
                            blocked), 100.0});
    depthd::SemanticFusionConfig fusion;
    fusion.min_valid_samples = 1U;
    depthd::SemanticPerceptionConfig config;
    config.fusion = fusion;
    depthd::SemanticPerceptionModule module(std::move(backends), config);
    expect(module.submit(frame(10U, 2'000'000'000LL)),
           "blocking test frame is accepted");
    {
      std::unique_lock lock(blocked->mutex);
      blocked->condition.wait_for(lock, 1s, [&] { return blocked->entered; });
    }
    expect(module.submit(frame(11U, 2'020'000'000LL)) &&
               module.submit(frame(12U, 2'040'000'000LL)) &&
               module.submit(frame(13U, 2'060'000'000LL)),
           "depth-side submits remain nonblocking during inference");
    {
      std::lock_guard lock(blocked->mutex);
      blocked->release = true;
    }
    blocked->condition.notify_all();
    expect(module.wait_for_result_after(10U, 2s),
           "latest pending frame completes after the backend releases");
    std::this_thread::sleep_for(20ms);
    const auto latest = module.latest_result();
    expect(latest.has_value() && latest->source_sequence == 13U,
           "capacity-one queue processes the newest frame, not backlog");
    expect(module.stats().replaced_pending_frames >= 2U,
           "replaced frames are counted explicitly");
  }

  return failures == 0 ? 0 : 1;
}
