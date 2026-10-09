#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>

namespace depthd {

// A non-owning description of one camera buffer plane. The lease on the
// containing frame, rather than this descriptor, controls the lifetime of the
// underlying request and dma-buf file descriptor.
struct CaptureBufferPlane {
  int fd{-1};
  std::uint64_t offset{};
  std::uint64_t length{};
  std::uint32_t stride_bytes{};
  // Stable only for one capture epoch. Platform adapters assign this from the
  // capture pool allocation, never from the recyclable descriptor number.
  std::uint64_t buffer_id{};
  // Bytes populated for this completed request. This can be smaller than the
  // allocation length and is required when copying optional chroma planes.
  std::uint64_t bytes_used{};
};

inline void validate(const CaptureBufferPlane& plane) {
  if (plane.fd < 0) {
    throw std::invalid_argument("capture buffer plane has an invalid file descriptor");
  }
  if (plane.length == 0U) {
    throw std::invalid_argument("capture buffer plane length must be non-zero");
  }
  if (plane.offset >
      std::numeric_limits<std::uint64_t>::max() - plane.length) {
    throw std::invalid_argument("capture buffer plane byte range overflows");
  }
}

// Move-only ownership of a completed camera request. Releasing the lease
// returns that request to its capture backend exactly once. Destruction is
// noexcept so leases remain safe during error unwinding and shutdown.
class CaptureBufferLease final {
 public:
  using Release = std::function<void()>;

  CaptureBufferLease() = default;

  CaptureBufferLease(const std::uint64_t capture_epoch,
                     const std::uint64_t token, Release release)
      : capture_epoch_(capture_epoch), token_(token), release_(std::move(release)) {
    if (capture_epoch_ == 0U) {
      throw std::invalid_argument("capture buffer lease epoch must be non-zero");
    }
    if (token_ == 0U) {
      throw std::invalid_argument("capture buffer lease token must be non-zero");
    }
    if (!release_) {
      throw std::invalid_argument("capture buffer lease requires a release callback");
    }
  }

  ~CaptureBufferLease() noexcept { reset(); }

  CaptureBufferLease(const CaptureBufferLease&) = delete;
  CaptureBufferLease& operator=(const CaptureBufferLease&) = delete;

  CaptureBufferLease(CaptureBufferLease&& other) noexcept
      : capture_epoch_(std::exchange(other.capture_epoch_, 0U)),
        token_(std::exchange(other.token_, 0U)),
        release_(std::move(other.release_)) {}

  CaptureBufferLease& operator=(CaptureBufferLease&& other) noexcept {
    if (this != &other) {
      reset();
      capture_epoch_ = std::exchange(other.capture_epoch_, 0U);
      token_ = std::exchange(other.token_, 0U);
      release_ = std::move(other.release_);
    }
    return *this;
  }

  [[nodiscard]] bool active() const noexcept {
    return static_cast<bool>(release_);
  }
  explicit operator bool() const noexcept { return active(); }

  [[nodiscard]] std::uint64_t capture_epoch() const noexcept {
    return capture_epoch_;
  }
  [[nodiscard]] std::uint64_t token() const noexcept { return token_; }

  void reset() noexcept {
    auto release = std::move(release_);
    capture_epoch_ = 0U;
    token_ = 0U;
    if (release) {
      try {
        release();
      } catch (...) {
        // A camera backend must surface release failures through its own
        // diagnostics. A destructor may never terminate the process.
      }
    }
  }

 private:
  std::uint64_t capture_epoch_{};
  std::uint64_t token_{};
  Release release_{};
};

}  // namespace depthd
