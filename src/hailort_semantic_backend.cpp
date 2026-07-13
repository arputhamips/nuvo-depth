#include "depthd/hailort_semantic_backend.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include <opencv2/imgproc.hpp>

#include "hailo/hailort.hpp"

namespace depthd {
namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(const Clock::time_point started) noexcept {
  return std::chrono::duration<double, std::milli>(Clock::now() - started)
      .count();
}

void require_status(const hailo_status status, const char* message) {
  if (status != HAILO_SUCCESS) {
    throw hailort::hailort_error(status, message);
  }
}

class PageAlignedBuffer {
 public:
  explicit PageAlignedBuffer(const std::size_t size) : data_size_(size) {
    const auto page_size = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    allocation_size_ = ((size + page_size - 1U) / page_size) * page_size;
    data_ = ::mmap(nullptr, allocation_size_, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data_ == MAP_FAILED) {
      data_ = nullptr;
      throw std::bad_alloc();
    }
  }

  ~PageAlignedBuffer() {
    if (data_ != nullptr) {
      (void)::munmap(data_, allocation_size_);
    }
  }

  PageAlignedBuffer(PageAlignedBuffer&& other) noexcept
      : data_(std::exchange(other.data_, nullptr)),
        data_size_(std::exchange(other.data_size_, 0U)),
        allocation_size_(std::exchange(other.allocation_size_, 0U)) {}
  PageAlignedBuffer& operator=(PageAlignedBuffer&&) = delete;
  PageAlignedBuffer(const PageAlignedBuffer&) = delete;
  PageAlignedBuffer& operator=(const PageAlignedBuffer&) = delete;

  std::uint8_t* data() noexcept {
    return static_cast<std::uint8_t*>(data_);
  }
  const std::uint8_t* data() const noexcept {
    return static_cast<const std::uint8_t*>(data_);
  }
  std::size_t size() const noexcept { return data_size_; }

 private:
  void* data_{};
  std::size_t data_size_{};
  std::size_t allocation_size_{};
};

struct ModelOutputView {
  const std::uint8_t* data{};
  std::size_t bytes{};
  hailo_3d_image_shape_t shape{};
  bool is_nms{};
  hailo_nms_shape_t nms_shape{};
};

struct PersistentModel {
  PersistentModel(std::string model_name,
                  std::shared_ptr<hailort::InferModel> model,
                  hailort::ConfiguredInferModel configured_model,
                  hailort::ConfiguredInferModel::Bindings model_bindings,
                  PageAlignedBuffer model_input,
                  PageAlignedBuffer model_output,
                  const hailo_3d_image_shape_t model_input_shape,
                  const hailo_3d_image_shape_t model_output_shape,
                  const bool output_is_nms,
                  const hailo_nms_shape_t model_nms_shape)
      : name(std::move(model_name)),
        infer_model(std::move(model)),
        input(std::move(model_input)),
        output(std::move(model_output)),
        configured(std::move(configured_model)),
        bindings(std::move(model_bindings)),
        input_shape(model_input_shape),
        output_shape(model_output_shape),
        is_nms(output_is_nms),
        nms_shape(model_nms_shape) {}

  std::string name;
  std::shared_ptr<hailort::InferModel> infer_model;
  PageAlignedBuffer input;
  PageAlignedBuffer output;
  hailort::ConfiguredInferModel configured;
  hailort::ConfiguredInferModel::Bindings bindings;
  hailo_3d_image_shape_t input_shape{};
  hailo_3d_image_shape_t output_shape{};
  bool is_nms{};
  hailo_nms_shape_t nms_shape{};
};

class HailortSemanticOwner {
 public:
  explicit HailortSemanticOwner(const HailortSemanticModelConfig& config) {
    hailo_vdevice_params_t params{};
    require_status(hailo_init_vdevice_params(&params),
                   "failed to initialize Hailo VDevice parameters");
    params.scheduling_algorithm = HAILO_SCHEDULING_ALGORITHM_ROUND_ROBIN;
    vdevice_ = hailort::VDevice::create_shared(params).expect(
        "failed to create semantic Hailo VDevice");
    models_.push_back(create_model("yolov8s", config.yolo_hef_path,
                                   HAILO_FORMAT_TYPE_FLOAT32));
    models_.push_back(create_model("segformer_b0_bn",
                                   config.segformer_hef_path,
                                   HAILO_FORMAT_TYPE_UINT8));
  }

  ModelOutputView infer(const std::string_view name,
                        const SemanticImageView& image) {
    std::lock_guard lock(mutex_);
    auto& model = find_model(name);
    const auto input_height = static_cast<int>(model.input_shape.height);
    const auto input_width = static_cast<int>(model.input_shape.width);
    if (model.input_shape.features != 3U ||
        model.input.size() != static_cast<std::size_t>(input_height) *
                                  static_cast<std::size_t>(input_width) * 3U) {
      throw std::runtime_error(model.name + " input is not packed RGB888");
    }
    const cv::Mat source(static_cast<int>(image.height),
                         static_cast<int>(image.width), CV_8UC3,
                         const_cast<std::uint8_t*>(image.bgr),
                         image.stride_bytes);
    cv::Mat resized_bgr;
    cv::resize(source, resized_bgr, {input_width, input_height}, 0.0, 0.0,
               cv::INTER_LINEAR);
    cv::Mat input_rgb(input_height, input_width, CV_8UC3, model.input.data());
    cv::cvtColor(resized_bgr, input_rgb, cv::COLOR_BGR2RGB);
    require_status(model.configured.wait_for_async_ready(
                       std::chrono::milliseconds(1000), 1U),
                   "semantic Hailo model did not become ready");
    require_status(model.configured.run(model.bindings,
                                        std::chrono::milliseconds(5000)),
                   "semantic Hailo inference failed");
    return {model.output.data(), model.output.size(), model.output_shape,
            model.is_nms, model.nms_shape};
  }

 private:
  std::unique_ptr<PersistentModel> create_model(
      const std::string& name, const std::string& hef_path,
      const hailo_format_type_t output_type) {
    if (hef_path.empty()) {
      throw std::invalid_argument(name + " HEF path is empty");
    }
    auto infer_model = vdevice_->create_infer_model(hef_path).expect(
        "failed to create semantic infer model");
    infer_model->set_batch_size(1U);
    auto input_stream = infer_model->input().expect(
        "semantic model must have exactly one input");
    auto output_stream = infer_model->output().expect(
        "semantic model must have exactly one output");
    input_stream.set_format_type(HAILO_FORMAT_TYPE_UINT8);
    output_stream.set_format_type(output_type);
    if (name == "yolov8s") {
      if (!output_stream.is_nms()) {
        throw std::runtime_error("YOLO HEF output is not Hailo NMS-by-class");
      }
      output_stream.set_nms_score_threshold(0.35F);
    }
    const auto input_shape = input_stream.shape();
    const auto output_shape = output_stream.shape();
    const auto is_nms = output_stream.is_nms();
    auto nms_shape = hailo_nms_shape_t{};
    if (is_nms) {
      nms_shape = output_stream.get_nms_shape().expect(
          "failed to obtain semantic NMS shape");
    }
    PageAlignedBuffer input(input_stream.get_frame_size());
    PageAlignedBuffer output(output_stream.get_frame_size());
    auto configured = infer_model->configure().expect(
        "failed to configure semantic infer model");
    auto bindings = configured.create_bindings().expect(
        "failed to create semantic infer bindings");
    require_status(bindings.input().expect("missing semantic input binding")
                       .set_buffer(hailort::MemoryView(input.data(), input.size())),
                   "failed to bind semantic input buffer");
    require_status(bindings.output().expect("missing semantic output binding")
                       .set_buffer(hailort::MemoryView(output.data(), output.size())),
                   "failed to bind semantic output buffer");
    return std::make_unique<PersistentModel>(
        name, std::move(infer_model), std::move(configured),
        std::move(bindings), std::move(input), std::move(output), input_shape,
        output_shape, is_nms, nms_shape);
  }

  PersistentModel& find_model(const std::string_view name) {
    const auto found = std::find_if(
        models_.begin(), models_.end(), [name](const auto& model) {
          return model->name == name;
        });
    if (found == models_.end()) {
      throw std::invalid_argument("unregistered semantic model");
    }
    return **found;
  }

  std::shared_ptr<hailort::VDevice> vdevice_;
  std::vector<std::unique_ptr<PersistentModel>> models_;
  std::mutex mutex_;
};

struct RelevantCocoClass {
  std::uint32_t id;
  SemanticClass semantic_class;
  const char* label;
};

constexpr std::array<RelevantCocoClass, 4> kRelevantCoco{{
    {0U, SemanticClass::kHuman, "person"},
    {14U, SemanticClass::kAnimal, "bird"},
    {15U, SemanticClass::kAnimal, "cat"},
    {16U, SemanticClass::kAnimal, "dog"},
}};

const RelevantCocoClass* relevant_coco(const std::uint32_t id) noexcept {
  const auto found = std::find_if(kRelevantCoco.begin(), kRelevantCoco.end(),
                                  [id](const auto& value) {
    return value.id == id;
  });
  return found == kRelevantCoco.end() ? nullptr : &*found;
}

class HailoYoloBackend final : public ISemanticBackend {
 public:
  explicit HailoYoloBackend(std::shared_ptr<HailortSemanticOwner> owner)
      : owner_(std::move(owner)) {}

  SemanticBackendInfo info() const override {
    return {"hailort", "yolov8s", "robot-validated-hailo8l",
            "17270a35bcf3e27a8dcd1dfae8d328a31318bb7c5b7c7a725392b83d008642c6",
            SemanticSourceKind::kInstance};
  }

  SemanticBackendOutput infer(const SemanticImageView& image) override {
    const auto inference_started = Clock::now();
    const auto output = owner_->infer("yolov8s", image);
    timing_.inference_latency_ms = elapsed_ms(inference_started);
    const auto postprocess_started = Clock::now();
    if (!output.is_nms) {
      throw std::runtime_error("YOLO output lost NMS metadata");
    }
    const auto* cursor = output.data;
    auto remaining = output.bytes;
    std::vector<SemanticObservation> observations;
    observations.reserve(16U);
    std::uint32_t observation_id = 1U;
    for (std::uint32_t class_id = 0U;
         class_id < output.nms_shape.number_of_classes; ++class_id) {
      if (remaining < sizeof(float)) {
        throw std::runtime_error("truncated Hailo NMS class count");
      }
      float raw_count{};
      std::memcpy(&raw_count, cursor, sizeof(raw_count));
      cursor += sizeof(raw_count);
      remaining -= sizeof(raw_count);
      if (!std::isfinite(raw_count) || raw_count < 0.0F ||
          raw_count > static_cast<float>(
              output.nms_shape.max_bboxes_per_class)) {
        throw std::runtime_error("invalid Hailo NMS class count");
      }
      const auto count = static_cast<std::uint32_t>(raw_count);
      const auto bytes = static_cast<std::size_t>(count) *
                         sizeof(hailo_bbox_float32_t);
      if (remaining < bytes) {
        throw std::runtime_error("truncated Hailo NMS bounding boxes");
      }
      const auto* relevant = relevant_coco(class_id);
      for (std::uint32_t index = 0U; index < count; ++index) {
        hailo_bbox_float32_t box{};
        std::memcpy(&box, cursor + index * sizeof(box), sizeof(box));
        if (relevant == nullptr || box.score < 0.35F) {
          continue;
        }
        const auto normalized = std::max(
            {box.y_min, box.x_min, box.y_max, box.x_max}) <= 1.5F;
        SemanticObservation observation;
        observation.source_kind = SemanticSourceKind::kInstance;
        observation.semantic_class = relevant->semantic_class;
        observation.observation_id = observation_id++;
        observation.model_class_id = class_id;
        observation.model_label = relevant->label;
        observation.model_name = "yolov8s";
        observation.confidence = box.score;
        observation.box = normalized
            ? ImageBoundingBox{box.x_min * static_cast<float>(image.width),
                               box.y_min * static_cast<float>(image.height),
                               box.x_max * static_cast<float>(image.width),
                               box.y_max * static_cast<float>(image.height)}
            : ImageBoundingBox{box.x_min, box.y_min, box.x_max, box.y_max};
        observations.push_back(std::move(observation));
      }
      cursor += bytes;
      remaining -= bytes;
    }
    std::sort(observations.begin(), observations.end(),
              [](const auto& left, const auto& right) {
      return left.confidence > right.confidence;
    });
    if (observations.size() > 16U) {
      observations.resize(16U);
    }
    timing_.postprocess_latency_ms = elapsed_ms(postprocess_started);
    SemanticBackendOutput result;
    result.instances = std::move(observations);
    return result;
  }

  SemanticBackendTiming last_timing() const noexcept override {
    return timing_;
  }

 private:
  std::shared_ptr<HailortSemanticOwner> owner_;
  SemanticBackendTiming timing_{};
};

struct DenseClass {
  std::uint8_t id;
  SemanticClass semantic_class;
  const char* label;
};

constexpr std::array<DenseClass, 4> kDenseClasses{{
    {3U, SemanticClass::kWall, "wall"},
    {4U, SemanticClass::kFence, "fence"},
    {8U, SemanticClass::kVegetation, "vegetation"},
    {9U, SemanticClass::kTerrain, "terrain"},
}};

class HailoSegformerBackend final : public ISemanticBackend {
 public:
  explicit HailoSegformerBackend(std::shared_ptr<HailortSemanticOwner> owner)
      : owner_(std::move(owner)) {}

  SemanticBackendInfo info() const override {
    return {"hailort", "segformer_b0_bn", "hailo-model-zoo-v2.17.0",
            "2093951835bafdb0585f93e3eddfb13fac2aa926c45475ccedabe5b2daa7a674",
            SemanticSourceKind::kDenseRegion};
  }

  SemanticBackendOutput infer(const SemanticImageView& image) override {
    const auto inference_started = Clock::now();
    const auto output = owner_->infer("segformer_b0_bn", image);
    timing_.inference_latency_ms = elapsed_ms(inference_started);
    const auto postprocess_started = Clock::now();
    if (output.is_nms || output.shape.features != 1U ||
        output.bytes != static_cast<std::size_t>(output.shape.width) *
                            output.shape.height) {
      throw std::runtime_error("unexpected SegFormer UINT8 NHW output");
    }
    const cv::Mat mask(static_cast<int>(output.shape.height),
                       static_cast<int>(output.shape.width), CV_8UC1,
                       const_cast<std::uint8_t*>(output.data));
    std::vector<SemanticObservation> observations;
    observations.reserve(48U);
    std::uint32_t observation_id = 10'000U;
    const auto scale_x = static_cast<float>(image.width) /
                         static_cast<float>(output.shape.width);
    const auto scale_y = static_cast<float>(image.height) /
                         static_cast<float>(output.shape.height);
    for (const auto& value : kDenseClasses) {
      cv::Mat binary;
      cv::compare(mask, value.id, binary, cv::CMP_EQ);
      cv::Mat labels;
      cv::Mat stats;
      cv::Mat centroids;
      const auto count = cv::connectedComponentsWithStats(
          binary, labels, stats, centroids, 8, CV_32S);
      for (int component = 1; component < count; ++component) {
        const auto pixels = stats.at<int>(component, cv::CC_STAT_AREA);
        if (pixels < 128) {
          continue;
        }
        const auto x = stats.at<int>(component, cv::CC_STAT_LEFT);
        const auto y = stats.at<int>(component, cv::CC_STAT_TOP);
        const auto width = stats.at<int>(component, cv::CC_STAT_WIDTH);
        const auto height = stats.at<int>(component, cv::CC_STAT_HEIGHT);
        SemanticObservation observation;
        observation.source_kind = SemanticSourceKind::kDenseRegion;
        observation.semantic_class = value.semantic_class;
        observation.observation_id = observation_id++;
        observation.model_class_id = value.id;
        observation.model_label = value.label;
        observation.model_name = "segformer_b0_bn";
        observation.confidence = 1.0F;
        observation.box = {static_cast<float>(x) * scale_x,
                           static_cast<float>(y) * scale_y,
                           static_cast<float>(x + width) * scale_x,
                           static_cast<float>(y + height) * scale_y};
        observation.pixel_count = static_cast<std::uint32_t>(pixels);
        observations.push_back(std::move(observation));
      }
    }
    std::sort(observations.begin(), observations.end(),
              [](const auto& left, const auto& right) {
      return left.pixel_count > right.pixel_count;
    });
    if (observations.size() > 48U) {
      observations.resize(48U);
    }
    timing_.postprocess_latency_ms = elapsed_ms(postprocess_started);
    SemanticBackendOutput result;
    result.dense_regions = std::move(observations);
    return result;
  }

  SemanticBackendTiming last_timing() const noexcept override {
    return timing_;
  }

 private:
  std::shared_ptr<HailortSemanticOwner> owner_;
  SemanticBackendTiming timing_{};
};

}  // namespace

std::vector<SemanticBackendRegistration> make_hailort_semantic_backends(
    const HailortSemanticModelConfig& config) {
  if (!std::isfinite(config.yolo_hz) || config.yolo_hz <= 0.0 ||
      !std::isfinite(config.segformer_hz) || config.segformer_hz <= 0.0) {
    throw std::invalid_argument("Hailo semantic model cadence is invalid");
  }
  auto owner = std::make_shared<HailortSemanticOwner>(config);
  std::vector<SemanticBackendRegistration> registrations;
  registrations.push_back(
      {std::make_unique<HailoYoloBackend>(owner), config.yolo_hz});
  registrations.push_back(
      {std::make_unique<HailoSegformerBackend>(std::move(owner)),
       config.segformer_hz});
  return registrations;
}

}  // namespace depthd
