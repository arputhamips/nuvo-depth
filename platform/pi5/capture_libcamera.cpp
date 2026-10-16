#include "depthd/capture.hpp"
#include "depthd/pio_pwm_trigger.hpp"

#if defined(DEPTHD_WITH_PI5_CAPTURE)

#include <linux/gpio.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <time.h>

#include <libcamera/control_ids.h>
#include <libcamera/controls.h>
#include <libcamera/formats.h>
#include <libcamera/framebuffer_allocator.h>
#include <libcamera/libcamera.h>
#include <libcamera/orientation.h>
#include <libcamera/property_ids.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace depthd {
namespace {

using namespace std::chrono_literals;

class GpioOutputLine final {
 public:
  GpioOutputLine(const std::string& chip_path, const std::uint32_t offset,
                 const bool initial_value) {
    const int chip_fd = ::open(chip_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (chip_fd < 0) {
      throw std::runtime_error("failed to open trigger gpiochip " + chip_path);
    }

    gpiohandle_request request{};
    request.lineoffsets[0] = offset;
    request.flags = GPIOHANDLE_REQUEST_OUTPUT;
    request.default_values[0] = initial_value ? 1U : 0U;
    request.lines = 1U;
    std::strncpy(request.consumer_label, "nuvo-depthd-capture-check",
                 sizeof(request.consumer_label) - 1U);
    if (::ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &request) < 0) {
      ::close(chip_fd);
      throw std::runtime_error("failed to request the trigger GPIO line");
    }
    ::close(chip_fd);
    line_fd_ = request.fd;
  }

  ~GpioOutputLine() {
    if (line_fd_ >= 0) {
      ::close(line_fd_);
    }
  }

  GpioOutputLine(const GpioOutputLine&) = delete;
  GpioOutputLine& operator=(const GpioOutputLine&) = delete;

  void set(const bool value) {
    gpiohandle_data data{};
    data.values[0] = value ? 1U : 0U;
    if (::ioctl(line_fd_, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) < 0) {
      throw std::runtime_error("failed to drive the trigger GPIO line");
    }
  }

 private:
  int line_fd_{-1};
};

struct RequestGate {
  std::mutex mutex;
  std::condition_variable ready;
  libcamera::Camera* camera{};
  bool running{};
  bool stop_requested{};
  std::deque<libcamera::Request*> pending;
  std::thread worker;
  std::atomic<std::size_t> outstanding{};
  std::atomic<std::uint64_t> requeue_failures{};
  struct SharedControls* controls{};
};

struct SharedControls {
  std::mutex mutex;
  CaptureControls value;
};

void apply_request_controls(libcamera::Request& request,
                            SharedControls& shared) {
  CaptureControls controls;
  {
    std::lock_guard lock(shared.mutex);
    controls = shared.value;
  }
  request.controls().set(libcamera::controls::AeEnable, false);
  request.controls().set(libcamera::controls::AwbEnable, false);
  request.controls().set(libcamera::controls::ExposureTime,
                         static_cast<std::int32_t>(controls.exposure_time_us));
  request.controls().set(libcamera::controls::AnalogueGain,
                         static_cast<float>(controls.analogue_gain));
  const std::array<float, 2> colour_gains{
      static_cast<float>(controls.colour_gains[0]),
      static_cast<float>(controls.colour_gains[1])};
  request.controls().set(libcamera::controls::ColourGains,
                         libcamera::Span<const float, 2>(colour_gains));
}

void recycle_request(const std::shared_ptr<RequestGate>& gate,
                     libcamera::Request* request) noexcept {
  if (!gate || request == nullptr) {
    return;
  }
  {
    std::lock_guard lock(gate->mutex);
    if (!gate->running || gate->stop_requested) {
      return;
    }
    gate->pending.push_back(request);
  }
  gate->ready.notify_one();
}

void start_request_recycler(const std::shared_ptr<RequestGate>& gate) {
  if (!gate || gate->camera == nullptr) {
    throw std::invalid_argument("request recycler requires a camera");
  }
  {
    std::lock_guard lock(gate->mutex);
    gate->running = true;
    gate->stop_requested = false;
  }
  gate->worker = std::thread([gate] {
    while (true) {
      libcamera::Request* request{};
      {
        std::unique_lock lock(gate->mutex);
        gate->ready.wait(lock, [&] {
          return gate->stop_requested || !gate->pending.empty();
        });
        if (gate->stop_requested) {
          gate->pending.clear();
          return;
        }
        request = gate->pending.front();
        gate->pending.pop_front();
      }
      request->reuse(libcamera::Request::ReuseBuffers);
      if (gate->controls != nullptr) {
        apply_request_controls(*request, *gate->controls);
      }
      if (gate->camera->queueRequest(request) != 0) {
        ++gate->requeue_failures;
      }
    }
  });
}

class CompletedRequestRecycleGuard final {
 public:
  CompletedRequestRecycleGuard(std::shared_ptr<RequestGate> gate,
                               libcamera::Request* request)
      : gate_(std::move(gate)), request_(request) {}
  ~CompletedRequestRecycleGuard() noexcept {
    if (armed_) {
      recycle_request(gate_, request_);
    }
  }
  void dismiss() noexcept { armed_ = false; }

 private:
  std::shared_ptr<RequestGate> gate_;
  libcamera::Request* request_{};
  bool armed_{true};
};

struct CameraState {
  CameraRole role{CameraRole::kLogicalLeft};
  CameraIdentity identity;
  std::shared_ptr<libcamera::Camera> camera;
  std::unique_ptr<libcamera::CameraConfiguration> configuration;
  libcamera::Stream* stream{};
  std::unique_ptr<libcamera::FrameBufferAllocator> allocator;
  std::vector<std::unique_ptr<libcamera::Request>> requests;
  std::unordered_map<const libcamera::FrameBuffer*, std::uint64_t> buffer_ids;
  std::shared_ptr<RequestGate> gate{std::make_shared<RequestGate>()};
  NegotiatedCameraStream negotiated;
  bool acquired{};
  bool started{};
};

CameraIdentity identity_for(const std::shared_ptr<libcamera::Camera>& camera,
                            const std::uint32_t index) {
  CameraIdentity identity;
  identity.stable_id = camera->id();
  identity.model = camera->properties()
                       .get(libcamera::properties::Model)
                       .value_or("libcamera-camera");
  const auto location = camera->properties()
                            .get(libcamera::properties::Location)
                            .value_or(-1);
  switch (location) {
    case libcamera::properties::CameraLocationFront:
      identity.location = "front";
      break;
    case libcamera::properties::CameraLocationBack:
      identity.location = "back";
      break;
    case libcamera::properties::CameraLocationExternal:
      identity.location = "external";
      break;
    default:
      identity.location = "unknown";
      break;
  }
  identity.discovery_index = index;
  return identity;
}

libcamera::PixelFormat requested_pixel_format(const std::string& name) {
  if (name == "YUV420") {
    return libcamera::formats::YUV420;
  }
  if (name == "RGB888") {
    return libcamera::formats::RGB888;
  }
  if (name == "BGR888") {
    return libcamera::formats::BGR888;
  }
  throw std::invalid_argument("unsupported Pi capture pixel format: " + name);
}

void require_external_trigger_mode() {
  constexpr const char* path = "/sys/module/imx296/parameters/trigger_mode";
  std::ifstream input(path);
  std::string value;
  if (!input || !(input >> value)) {
    throw std::runtime_error(
        "IMX296 trigger_mode is unavailable; check the sensor driver and "
        "always-on overlays");
  }
  if (value != "1") {
    throw std::runtime_error(
        "IMX296 external trigger mode is not enabled (trigger_mode != 1)");
  }
}

class Pi5LibcameraCapture final : public CaptureSource {
 public:
  Pi5LibcameraCapture() : manager_(std::make_unique<libcamera::CameraManager>()) {
    const int result = manager_->start();
    if (result != 0) {
      throw std::runtime_error("failed to start libcamera CameraManager");
    }
    manager_started_ = true;
  }

  ~Pi5LibcameraCapture() override {
    stop();
    if (manager_started_) {
      manager_->stop();
    }
  }

  std::vector<CameraIdentity> enumerate() override {
    std::vector<CameraIdentity> result;
    const auto cameras = manager_->cameras();
    result.reserve(cameras.size());
    for (std::size_t index = 0; index < cameras.size(); ++index) {
      result.push_back(identity_for(cameras[index],
                                    static_cast<std::uint32_t>(index)));
    }
    canonicalize_camera_order(result);
    return result;
  }

  CaptureSessionInfo start(const CaptureOpenOptions& options) override {
    validate(options);
    {
      std::lock_guard lock(state_mutex_);
      if (running_) {
        throw std::logic_error("Pi capture source is already running");
      }
    }

    const auto bindings = bind_pi5_fixed_csi_ports(enumerate());
    const auto& left_binding = bindings[0];
    const auto& right_binding = bindings[1];

    auto left = std::make_unique<CameraState>();
    auto right = std::make_unique<CameraState>();
    left->role = CameraRole::kLogicalLeft;
    right->role = CameraRole::kLogicalRight;
    left->camera = manager_->get(left_binding.camera.stable_id);
    right->camera = manager_->get(right_binding.camera.stable_id);
    if (!left->camera || !right->camera) {
      throw std::runtime_error("libcamera camera disappeared during selection");
    }
    left->identity = left_binding.camera;
    right->identity = right_binding.camera;

    if (options.hardware_trigger_enabled) {
      require_external_trigger_mode();
    }

    // Retain one unmatched frame per side, leaving the rest of each camera's
    // negotiated request pool available for forward progress.
    pairer_.configure({options.pair_tolerance_ns, 1U});
    std::uint64_t epoch = pairer_.begin_epoch(left_binding, right_binding);
    current_epoch_.store(epoch);
    delivery_start_boottime_ns_.store(0);
    stop_requested_.store(false);
    callback_errors_.store(0U);
    frame_completion_errors_.store(0U);
    trigger_errors_.store(0U);
    trigger_attempts_.store(0U);
    trigger_deadline_overruns_.store(0U);
    trigger_last_pulse_us_.store(0U);
    trigger_min_pulse_us_.store(std::numeric_limits<std::uint64_t>::max());
    trigger_max_pulse_us_.store(0U);
    trigger_pulse_guard_overruns_.store(0U);
    trigger_max_pulse_overshoot_us_.store(0U);
    ready_pair_replacements_.store(0U);
    last_requeue_failures_.store(0U);
    last_outstanding_after_stop_.store(0U);

    left_ = std::move(left);
    right_ = std::move(right);
    try {
      {
        std::lock_guard lock(controls_.mutex);
        controls_.value = {options.exposure_time_us, options.analogue_gain,
                           options.colour_gains};
      }
      configure_camera(*left_, options, left_binding);
      configure_camera(*right_, options, right_binding);

      if (options.hardware_trigger_enabled) {
        if (options.gpiochip == "/dev/gpiochip0") {
          trigger_pwm_ = PioPwmTrigger::discover(options.gpio_line);
        }
        if (!trigger_pwm_) {
          trigger_line_ = std::make_unique<GpioOutputLine>(
              options.gpiochip, options.gpio_line, options.trigger_idle_high);
        }
      }

      left_->camera->requestCompleted.connect(
          this, &Pi5LibcameraCapture::request_complete);
      right_->camera->requestCompleted.connect(
          this, &Pi5LibcameraCapture::request_complete);

      start_camera(*left_, options);
      start_camera(*right_, options);
      queue_requests(*left_);
      queue_requests(*right_);
      {
        std::lock_guard lock(state_mutex_);
        running_ = true;
      }

      trigger_options_ = options;
      trigger_hardware_timed_.store(static_cast<bool>(trigger_pwm_));
      trigger_enabled_.store(static_cast<bool>(trigger_pwm_) || static_cast<bool>(trigger_line_));
      trigger_commanded_pulse_us_.store(options.exposure_time_us);
      trigger_period_ns_.store(static_cast<std::uint64_t>(trigger_period(options).count()) * 1000ULL);
      if (trigger_pwm_ || trigger_line_) {
        if (trigger_pwm_) {
          trigger_pwm_->start(trigger_period_ns_.load(), options.exposure_time_us,
                              options.trigger_idle_high);
          std::this_thread::sleep_for(trigger_period(options) * options.startup_priming_pulses);
        } else {
          for (std::uint32_t pulse = 0;
               pulse < options.startup_priming_pulses; ++pulse) {
            pulse_trigger(options.exposure_time_us);
            std::this_thread::sleep_for(trigger_period(options));
          }
        }
        // Camera/IPA startup latency can make the first priming event visible
        // to only one sensor. Priming is deliberately outside the delivered
        // capture epoch: release its retained latest pair, discard unmatched
        // frames, reset statistics, and then assign all subsequent callbacks
        // to a fresh epoch before continuous triggering begins.
        {
          std::lock_guard lock(ready_mutex_);
          timespec cutoff{};
          if (::clock_gettime(CLOCK_BOOTTIME, &cutoff) != 0) {
            throw std::runtime_error("cannot establish capture priming timestamp boundary");
          }
          delivery_start_boottime_ns_.store(
              static_cast<std::int64_t>(cutoff.tv_sec) * 1'000'000'000LL + cutoff.tv_nsec);
          current_epoch_.store(0U);
          ready_pair_.reset();
          pairer_.end_epoch();
          epoch = pairer_.begin_epoch(left_binding, right_binding);
          current_epoch_.store(epoch);
        }
        if (trigger_line_) {
          trigger_thread_ =
              std::thread(&Pi5LibcameraCapture::trigger_loop, this);
        }
      }

      CaptureSessionInfo info;
      info.capture_epoch = epoch;
      info.left = left_->negotiated;
      info.right = right_->negotiated;
      info.hardware_trigger_active = trigger_enabled_.load();
      info.trigger_source = trigger_pwm_ ? "rp1-pio-pwm" :
          (trigger_line_ ? "userspace-gpio-smoke-check" : "free-running");
      return info;
    } catch (...) {
      stop();
      throw;
    }
  }

  std::optional<StereoPair> wait_for_pair(
      const std::chrono::milliseconds timeout) override {
    std::unique_lock lock(ready_mutex_);
    ready_condition_.wait_for(lock, timeout, [this] {
      return ready_pair_.has_value() || stop_requested_.load() ||
             !running_snapshot();
    });
    if (!ready_pair_) {
      return std::nullopt;
    }
    auto pair = std::move(ready_pair_);
    ready_pair_.reset();
    return pair;
  }

  void set_controls(const CaptureControls& controls) override {
    validate(controls);
    if (!running_snapshot()) {
      throw std::logic_error("capture controls require a running camera");
    }
    if (trigger_line_ || trigger_pwm_) {
      const auto period_us = static_cast<std::uint64_t>(
          trigger_period(trigger_options_).count());
      if (static_cast<std::uint64_t>(controls.exposure_time_us) +
              kExternalTriggerGuardUs >
          period_us) {
        throw std::invalid_argument(
            "capture exposure must leave the external-trigger period guard");
      }
    }
    std::lock_guard lock(controls_.mutex);
    if (trigger_pwm_ && controls.exposure_time_us != controls_.value.exposure_time_us) {
      trigger_pwm_->set_pulse(controls.exposure_time_us);
    }
    controls_.value = controls;
    trigger_commanded_pulse_us_.store(controls.exposure_time_us);
  }

  void stop() noexcept override {
    stop_requested_.store(true);
    ready_condition_.notify_all();
    if (trigger_thread_.joinable()) {
      trigger_thread_.join();
    }
    if (trigger_line_) {
      try {
        trigger_line_->set(trigger_options_.trigger_idle_high);
      } catch (...) {
      }
    }

    {
      std::lock_guard lock(state_mutex_);
      running_ = false;
    }
    disable_requeue(left_.get());
    disable_requeue(right_.get());
    stop_camera(right_.get());
    stop_camera(left_.get());
    if (trigger_pwm_) {
      try { trigger_pwm_->stop(); }
      catch (...) { ++trigger_errors_; }
    }

    pairer_.end_epoch();
    {
      std::lock_guard lock(ready_mutex_);
      ready_pair_.reset();
    }
    ready_condition_.notify_all();

    const auto outstanding = outstanding_for(left_.get()) +
                             outstanding_for(right_.get());
    last_outstanding_after_stop_.store(outstanding);
    last_requeue_failures_.store(requeue_failures_for(left_.get()) +
                                 requeue_failures_for(right_.get()));
    release_camera(right_.get());
    release_camera(left_.get());
    trigger_line_.reset();
    trigger_pwm_.reset();
    trigger_enabled_.store(false);
    right_.reset();
    left_.reset();
    current_epoch_.store(0U);
  }

  CaptureRuntimeStats runtime_stats() const noexcept override {
    CaptureRuntimeStats result;
    result.trigger_source = trigger_enabled_.load() ?
        (trigger_hardware_timed_.load() ? "rp1-pio-pwm" : "userspace-gpio-smoke-check") :
        (running_snapshot() ? "free-running" : "stopped");
    result.trigger_hardware_timed = trigger_hardware_timed_.load();
    result.trigger_pulse_timing_measured = trigger_enabled_.load() && !result.trigger_hardware_timed;
    result.trigger_commanded_pulse_us = trigger_commanded_pulse_us_.load();
    result.trigger_period_ns = trigger_period_ns_.load();
    result.pairer = pairer_.stats();
    result.callback_or_trigger_errors = callback_errors_.load();
    result.frame_completion_errors = frame_completion_errors_.load();
    result.trigger_errors = trigger_errors_.load();
    result.trigger_attempts = trigger_attempts_.load();
    result.trigger_deadline_overruns = trigger_deadline_overruns_.load();
    result.trigger_last_pulse_us = trigger_last_pulse_us_.load();
    const auto minimum_pulse = trigger_min_pulse_us_.load();
    result.trigger_min_pulse_us =
        minimum_pulse == std::numeric_limits<std::uint64_t>::max()
            ? 0U
            : minimum_pulse;
    result.trigger_max_pulse_us = trigger_max_pulse_us_.load();
    result.trigger_pulse_guard_overruns =
        trigger_pulse_guard_overruns_.load();
    result.trigger_max_pulse_overshoot_us =
        trigger_max_pulse_overshoot_us_.load();
    result.ready_pair_replacements = ready_pair_replacements_.load();
    result.request_requeue_failures =
        std::max(last_requeue_failures_.load(),
                 requeue_failures_for(left_.get()) +
                     requeue_failures_for(right_.get()));
    return result;
  }

  std::size_t outstanding_leases() const noexcept override {
    const auto active = outstanding_for(left_.get()) + outstanding_for(right_.get());
    return std::max(active, last_outstanding_after_stop_.load());
  }

 private:
  static std::chrono::microseconds trigger_period(
      const CaptureOpenOptions& options) {
    return std::chrono::microseconds(
        static_cast<std::int64_t>(1'000'000.0 / options.target_hz));
  }

  void pulse_trigger(const std::uint32_t exposure_time_us) {
    const bool active = !trigger_options_.trigger_idle_high;
    trigger_line_->set(active);
    const auto started = std::chrono::steady_clock::now();
    try {
      std::this_thread::sleep_for(
          std::chrono::microseconds(exposure_time_us));
      trigger_line_->set(trigger_options_.trigger_idle_high);
      const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();
      const auto pulse_us = static_cast<std::uint64_t>(std::max<std::int64_t>(
          0, elapsed));
      trigger_last_pulse_us_.store(pulse_us);
      auto minimum = trigger_min_pulse_us_.load();
      while (pulse_us < minimum &&
             !trigger_min_pulse_us_.compare_exchange_weak(minimum, pulse_us)) {
      }
      auto maximum = trigger_max_pulse_us_.load();
      while (pulse_us > maximum &&
             !trigger_max_pulse_us_.compare_exchange_weak(maximum, pulse_us)) {
      }
      const auto overshoot_us = pulse_us > exposure_time_us
                                    ? pulse_us - exposure_time_us
                                    : 0U;
      if (overshoot_us > kExternalTriggerGuardUs) {
        ++trigger_pulse_guard_overruns_;
      }
      auto maximum_overshoot = trigger_max_pulse_overshoot_us_.load();
      while (overshoot_us > maximum_overshoot &&
             !trigger_max_pulse_overshoot_us_.compare_exchange_weak(
                 maximum_overshoot, overshoot_us)) {
      }
    } catch (...) {
      try {
        trigger_line_->set(trigger_options_.trigger_idle_high);
      } catch (...) {
      }
      throw;
    }
  }

  void trigger_loop() noexcept {
    const auto period = trigger_period(trigger_options_);
    auto next = std::chrono::steady_clock::now();
    while (!stop_requested_.load()) {
      const auto now = std::chrono::steady_clock::now();
      if (now > next) {
        const auto missed = (now - next) / period;
        if (missed > 0) {
          trigger_deadline_overruns_.fetch_add(
              static_cast<std::uint64_t>(missed));
          next += period * missed;
        }
      }
      if (now < next) {
        std::this_thread::sleep_until(next);
      }
      if (stop_requested_.load()) {
        break;
      }
      try {
        ++trigger_attempts_;
        std::uint32_t exposure_time_us{};
        {
          std::lock_guard lock(controls_.mutex);
          exposure_time_us = controls_.value.exposure_time_us;
        }
        pulse_trigger(exposure_time_us);
      } catch (...) {
        ++trigger_errors_;
        ++callback_errors_;
        stop_requested_.store(true);
        ready_condition_.notify_all();
        break;
      }
      next += period;
    }
  }

  void configure_camera(CameraState& state,
                        const CaptureOpenOptions& options,
                        const CameraBinding& binding) {
    if (state.camera->acquire() != 0) {
      throw std::runtime_error("failed to acquire camera " +
                               state.identity.stable_id);
    }
    state.acquired = true;
    state.configuration = state.camera->generateConfiguration(
        {libcamera::StreamRole::VideoRecording});
    if (!state.configuration || state.configuration->empty()) {
      throw std::runtime_error("camera could not generate a video stream");
    }
    auto& stream_configuration = state.configuration->at(0);
    stream_configuration.size = libcamera::Size(options.width, options.height);
    stream_configuration.pixelFormat =
        requested_pixel_format(options.pixel_format);
    stream_configuration.colorSpace = libcamera::ColorSpace::Sycc;
    stream_configuration.bufferCount = options.buffer_count;
    state.configuration->orientation =
        options.orientation_degrees == 180U
            ? libcamera::Orientation::Rotate180
            : libcamera::Orientation::Rotate0;
    if (state.configuration->validate() ==
        libcamera::CameraConfiguration::Invalid) {
      throw std::runtime_error("libcamera rejected the stream configuration");
    }
    if (stream_configuration.colorSpace != libcamera::ColorSpace::Sycc) {
      throw std::runtime_error("camera must negotiate full-range sYCC for YUV/RGB conversion");
    }
    if (state.camera->configure(state.configuration.get()) != 0) {
      throw std::runtime_error("failed to configure camera " +
                               state.identity.stable_id);
    }
    state.stream = stream_configuration.stream();
    state.allocator =
        std::make_unique<libcamera::FrameBufferAllocator>(state.camera);
    const int allocated = state.allocator->allocate(state.stream);
    if (allocated <= 0) {
      throw std::runtime_error("failed to allocate camera frame buffers");
    }

    const auto& buffers = state.allocator->buffers(state.stream);
    state.requests.reserve(buffers.size());
    for (std::size_t index = 0; index < buffers.size(); ++index) {
      const auto cookie = state.role == CameraRole::kLogicalLeft ? 1U : 2U;
      auto request = state.camera->createRequest(cookie);
      if (!request || request->addBuffer(state.stream, buffers[index].get()) != 0) {
        throw std::runtime_error("failed to create a camera capture request");
      }
      state.buffer_ids.emplace(buffers[index].get(), index + 1U);
      state.requests.push_back(std::move(request));
    }
    state.gate->camera = state.camera.get();
    state.gate->controls = &controls_;
    state.negotiated.binding = binding;
    state.negotiated.grid = make_reference_grid(
        stream_configuration.size.width, stream_configuration.size.height);
    state.negotiated.pixel_format = stream_configuration.pixelFormat.toString();
    state.negotiated.stride_bytes = stream_configuration.stride;
    state.negotiated.buffer_count =
        static_cast<std::uint32_t>(buffers.size());
  }

  void start_camera(CameraState& state, const CaptureOpenOptions& options) {
    libcamera::ControlList controls(libcamera::controls::controls);
    controls.set(libcamera::controls::AeEnable, false);
    controls.set(libcamera::controls::AwbEnable, false);
    controls.set(libcamera::controls::ExposureTime,
                 static_cast<std::int32_t>(options.exposure_time_us));
    controls.set(libcamera::controls::AnalogueGain,
                 static_cast<float>(options.analogue_gain));
    const std::array<float, 2> colour_gains{
        static_cast<float>(options.colour_gains[0]),
        static_cast<float>(options.colour_gains[1])};
    controls.set(libcamera::controls::ColourGains,
                 libcamera::Span<const float, 2>(colour_gains));
    const std::int64_t frame_duration =
        static_cast<std::int64_t>(1'000'000.0 / options.target_hz);
    const std::array<std::int64_t, 2> duration_limits{frame_duration,
                                                     frame_duration};
    controls.set(libcamera::controls::FrameDurationLimits,
                 libcamera::Span<const std::int64_t, 2>(duration_limits));
    if (state.camera->start(&controls) != 0) {
      throw std::runtime_error("failed to start camera " +
                               state.identity.stable_id);
    }
    state.started = true;
    start_request_recycler(state.gate);
  }

  static void queue_requests(CameraState& state) {
    for (const auto& request : state.requests) {
      if (state.gate->controls != nullptr) {
        apply_request_controls(*request, *state.gate->controls);
      }
      if (state.camera->queueRequest(request.get()) != 0) {
        throw std::runtime_error("failed to queue initial camera request");
      }
    }
  }

  void request_complete(libcamera::Request* request) noexcept {
    if (request->status() == libcamera::Request::RequestCancelled) {
      return;
    }
    try {
      CameraState* state = request->cookie() == 1U
                               ? left_.get()
                               : request->cookie() == 2U ? right_.get()
                                                         : nullptr;
      if (!state || request->buffers().empty()) {
        throw std::runtime_error("completed request has no camera or buffer");
      }
      auto gate = state->gate;
      CompletedRequestRecycleGuard recycle_on_error(gate, request);
      auto* buffer = request->buffers().begin()->second;
      const auto buffer_identity = state->buffer_ids.find(buffer);
      if (buffer_identity == state->buffer_ids.end()) {
        throw std::runtime_error("completed frame is outside the capture pool");
      }
      const auto& metadata = buffer->metadata();
      if (metadata.status != libcamera::FrameMetadata::FrameSuccess) {
        // libcamera may surface recoverable per-buffer errors while external
        // triggering is priming. Discard and recycle those buffers; reserve
        // callback_errors_ for failures that make the callback incoherent.
        ++frame_completion_errors_;
        return;
      }
      if (metadata.planes().size() != buffer->planes().size()) {
        throw std::runtime_error(
            "completed frame plane metadata does not match allocation");
      }
      const auto required_luma =
          static_cast<std::uint64_t>(state->negotiated.stride_bytes) *
          state->negotiated.grid.height;
      if (buffer->planes().empty() ||
          metadata.planes().front().bytesused < required_luma ||
          buffer->planes().front().length < required_luma) {
        throw std::runtime_error(
            "completed frame does not contain the negotiated luma plane");
      }
      for (std::size_t index = 0; index < buffer->planes().size(); ++index) {
        if (metadata.planes()[index].bytesused > buffer->planes()[index].length) {
          throw std::runtime_error(
              "completed frame plane exceeds its allocation");
        }
      }
      const auto timestamp = request->metadata()
                                 .get(libcamera::controls::SensorTimestamp)
                                 .value_or(0);

      CapturedFrame frame;
      frame.role = state->role;
      frame.camera_stable_id = state->identity.stable_id;
      frame.capture_epoch = current_epoch_.load();
      frame.stream_sequence =
          static_cast<std::uint64_t>(buffer->metadata().sequence) + 1U;
      frame.sensor_timestamp_ns = timestamp;
      if (frame.capture_epoch == 0U ||
          timestamp <= delivery_start_boottime_ns_.load()) {
        return;  // Startup captures cannot be relabeled into the delivered epoch.
      }
      frame.isp_exposure_time_us = request->metadata()
          .get(libcamera::controls::ExposureTime).value_or(0);
      frame.isp_analogue_gain = request->metadata()
          .get(libcamera::controls::AnalogueGain).value_or(0.0F);
      frame.isp_digital_gain = request->metadata()
          .get(libcamera::controls::DigitalGain).value_or(0.0F);
      frame.sensor_clock = CaptureClock::kBoottime;
      frame.grid = state->negotiated.grid;
      frame.pixel_format = state->negotiated.pixel_format;
      frame.planes.reserve(buffer->planes().size());
      for (std::size_t index = 0; index < buffer->planes().size(); ++index) {
        const auto& plane = buffer->planes()[index];
        const auto plane_height = index == 0U
                                      ? state->negotiated.grid.height
                                      : (state->negotiated.grid.height + 1U) / 2U;
        const auto bytes_used = metadata.planes()[index].bytesused;
        auto stride = index == 0U ? state->negotiated.stride_bytes : 0U;
        if (index != 0U && plane_height != 0U &&
            bytes_used % plane_height == 0U) {
          stride = static_cast<std::uint32_t>(bytes_used / plane_height);
        }
        if (index != 0U && stride == 0U) {
          stride = (state->negotiated.stride_bytes + 1U) / 2U;
        }
        frame.planes.push_back(
            {plane.fd.get(), plane.offset, plane.length, stride,
             buffer_identity->second, bytes_used});
      }

      const auto token = ++lease_token_;
      auto lease = CaptureBufferLease(
          frame.capture_epoch, token, [gate, request] {
            --gate->outstanding;
            recycle_request(gate, request);
          });
      ++gate->outstanding;
      frame.lease = std::move(lease);
      recycle_on_error.dismiss();

      auto pair = pairer_.submit(std::move(frame));
      if (pair) {
        std::lock_guard lock(ready_mutex_);
        if (pair->capture_epoch != current_epoch_.load()) return;
        if (ready_pair_) {
          ++ready_pair_replacements_;
        }
        ready_pair_.emplace(std::move(*pair));
        ready_condition_.notify_one();
      }
    } catch (const std::exception& error) {
      if (frame_completion_errors_.fetch_add(1U) == 0U) {
        std::cerr << "depthd: first frame-completion error: " << error.what()
                  << '\n';
      }
      ++callback_errors_;
    } catch (...) {
      if (frame_completion_errors_.fetch_add(1U) == 0U) {
        std::cerr << "depthd: first frame-completion error: unknown failure\n";
      }
      ++callback_errors_;
    }
  }

  bool running_snapshot() const noexcept {
    std::lock_guard lock(state_mutex_);
    return running_;
  }

  static void disable_requeue(CameraState* state) noexcept {
    if (!state) {
      return;
    }
    {
      std::lock_guard lock(state->gate->mutex);
      state->gate->running = false;
      state->gate->stop_requested = true;
    }
    state->gate->ready.notify_all();
    if (state->gate->worker.joinable()) {
      state->gate->worker.join();
    }
  }

  void stop_camera(CameraState* state) noexcept {
    if (!state || !state->camera) {
      return;
    }
    if (state->started) {
      (void)state->camera->stop();
      state->started = false;
    }
    state->camera->requestCompleted.disconnect(
        this, &Pi5LibcameraCapture::request_complete);
  }

  static void release_camera(CameraState* state) noexcept {
    if (!state || !state->camera) {
      return;
    }
    state->requests.clear();
    state->allocator.reset();
    state->configuration.reset();
    if (state->acquired) {
      (void)state->camera->release();
      state->acquired = false;
    }
  }

  static std::size_t outstanding_for(const CameraState* state) noexcept {
    return state ? state->gate->outstanding.load() : 0U;
  }

  static std::uint64_t requeue_failures_for(
      const CameraState* state) noexcept {
    return state ? state->gate->requeue_failures.load() : 0U;
  }

  std::unique_ptr<libcamera::CameraManager> manager_;
  bool manager_started_{};
  StereoPairer pairer_;
  std::unique_ptr<CameraState> left_;
  std::unique_ptr<CameraState> right_;
  std::unique_ptr<GpioOutputLine> trigger_line_;
  std::unique_ptr<PioPwmTrigger> trigger_pwm_;
  std::atomic<bool> trigger_enabled_{};
  std::atomic<bool> trigger_hardware_timed_{};
  std::atomic<std::uint64_t> trigger_commanded_pulse_us_{};
  std::atomic<std::uint64_t> trigger_period_ns_{};
  CaptureOpenOptions trigger_options_;
  SharedControls controls_;
  std::thread trigger_thread_;
  std::atomic<bool> stop_requested_{true};
  std::atomic<std::uint64_t> current_epoch_{};
  std::atomic<std::int64_t> delivery_start_boottime_ns_{};
  std::atomic<std::uint64_t> lease_token_{};
  std::atomic<std::uint64_t> callback_errors_{};
  std::atomic<std::uint64_t> frame_completion_errors_{};
  std::atomic<std::uint64_t> trigger_errors_{};
  std::atomic<std::uint64_t> trigger_attempts_{};
  std::atomic<std::uint64_t> trigger_deadline_overruns_{};
  std::atomic<std::uint64_t> trigger_last_pulse_us_{};
  std::atomic<std::uint64_t> trigger_min_pulse_us_{
      std::numeric_limits<std::uint64_t>::max()};
  std::atomic<std::uint64_t> trigger_max_pulse_us_{};
  std::atomic<std::uint64_t> trigger_pulse_guard_overruns_{};
  std::atomic<std::uint64_t> trigger_max_pulse_overshoot_us_{};
  std::atomic<std::uint64_t> ready_pair_replacements_{};
  std::atomic<std::uint64_t> last_requeue_failures_{};
  std::atomic<std::size_t> last_outstanding_after_stop_{};
  mutable std::mutex state_mutex_;
  bool running_{};
  std::mutex ready_mutex_;
  std::condition_variable ready_condition_;
  std::optional<StereoPair> ready_pair_;
};

}  // namespace

std::unique_ptr<CaptureSource> make_pi5_libcamera_capture_source() {
  return std::make_unique<Pi5LibcameraCapture>();
}

std::string_view pi5_capture_backend_status() noexcept {
  return "available: native libcamera + Linux GPIO character device";
}

}  // namespace depthd

#else

namespace depthd {

std::unique_ptr<CaptureSource> make_pi5_libcamera_capture_source() {
  return nullptr;
}

std::string_view pi5_capture_backend_status() noexcept {
  return "unavailable: rebuild on Linux with -DDEPTHD_WITH_PI5_CAPTURE=ON";
}

}  // namespace depthd

#endif
