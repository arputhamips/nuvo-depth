#include "depthd/buffer.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>

namespace {

int fail(const int code, const std::string_view message) {
  std::cerr << "capture_lease_test: " << message << '\n';
  return code;
}

template <typename Function>
bool throws(Function&& function) {
  try {
    function();
  } catch (const std::exception&) {
    return true;
  }
  return false;
}

}  // namespace

int main() {
  int releases = 0;
  {
    depthd::CaptureBufferLease first(7U, 11U, [&releases] { ++releases; });
    if (!first.active() || first.capture_epoch() != 7U || first.token() != 11U) {
      return fail(1, "new lease does not expose its identity");
    }

    depthd::CaptureBufferLease second(std::move(first));
    if (first.active() || !second.active() || releases != 0) {
      return fail(2, "move construction changed release ownership");
    }

    depthd::CaptureBufferLease third(7U, 12U, [&releases] { ++releases; });
    third = std::move(second);
    if (releases != 1 || second.active() || third.token() != 11U) {
      return fail(3, "move assignment did not release the replaced lease once");
    }
    third.reset();
    third.reset();
    if (releases != 2) {
      return fail(4, "reset was not exactly once");
    }
  }
  if (releases != 2) {
    return fail(5, "destruction released an inactive lease");
  }

  {
    int shared_releases{};
    depthd::CaptureBufferLease first(9U, 42U, [&] { ++shared_releases; });
    auto second = first.share();
    auto third = second.share();
    first.reset();
    second.reset();
    if (shared_releases != 0 || third.token() != 42U || third.capture_epoch() != 9U)
      return fail(9, "shared camera request was released before its last reader");
    third.reset();
    if (shared_releases != 1 || !throws([&] { (void)third.share(); }))
      return fail(10, "shared camera request release was not exactly once");
  }

  {
    depthd::CaptureBufferLease throwing(1U, 1U, [] {
      throw std::runtime_error("backend release failure");
    });
  }

  if (!throws([] {
        depthd::CaptureBufferLease invalid(0U, 1U, [] {});
      }) ||
      !throws([] {
        depthd::CaptureBufferLease invalid(1U, 0U, [] {});
      }) ||
      !throws([] {
        depthd::CaptureBufferLease invalid(1U, 1U, {});
      })) {
    return fail(6, "invalid lease construction was accepted");
  }

  depthd::CaptureBufferPlane plane{3, 128U, 4096U, 728U};
  try {
    depthd::validate(plane);
  } catch (const std::exception&) {
    return fail(7, "valid plane descriptor was rejected");
  }
  if (!throws([] { depthd::validate(depthd::CaptureBufferPlane{}); }) ||
      !throws([] {
        depthd::validate(depthd::CaptureBufferPlane{3, 0U, 0U, 0U});
      }) ||
      !throws([] {
        depthd::validate(depthd::CaptureBufferPlane{
            3, std::numeric_limits<std::uint64_t>::max(), 2U, 0U});
      })) {
    return fail(8, "invalid plane descriptor was accepted");
  }

  return 0;
}
