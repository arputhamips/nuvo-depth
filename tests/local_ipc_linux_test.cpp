#include "depthd/local_ipc.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

void require(const bool condition, const std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

}  // namespace

int main() {
  std::filesystem::path directory;
  try {
    require(depthd::ipc_v2::seqpacket_transport_available(),
            "Linux seqpacket transport reported unavailable");
    auto shared = depthd::ipc_v2::SealedSharedMemory::create("nuvo-test", 64U);
    shared.writable_bytes()[0] = std::byte{'n'};
    require((::fcntl(shared.readonly_handle(), F_GETFL) & O_ACCMODE) == O_RDONLY,
            "exported shared-memory descriptor is not read-only");
    errno = 0;
    void* rejected = ::mmap(nullptr, shared.size(), PROT_READ | PROT_WRITE,
                            MAP_SHARED, shared.readonly_handle(), 0);
    require(rejected == MAP_FAILED,
            "receiver could create a writable shared-memory mapping");
    void* read_mapping = ::mmap(nullptr, shared.size(), PROT_READ, MAP_SHARED,
                                shared.readonly_handle(), 0);
    require(read_mapping != MAP_FAILED &&
                static_cast<const std::byte*>(read_mapping)[0] == std::byte{'n'},
            "read-only shared-memory mapping could not observe producer data");
    (void)::munmap(read_mapping, shared.size());
    directory = std::filesystem::temp_directory_path() /
                ("nuvo-ipc-v2-test-" + std::to_string(::getpid()));
    std::filesystem::create_directory(directory);
    const auto path = directory / "data.sock";
    {
      depthd::ipc_v2::SeqpacketListener listener(path, 0600U, 2);
      std::thread server([&listener] {
        auto connection = listener.accept(std::chrono::seconds(2));
        require(static_cast<bool>(connection), "server accept timed out");
        const auto peer = connection.peer_credentials();
        require(peer.process_id > 0 && peer.user_id == ::geteuid(),
                "SO_PEERCRED did not authenticate the local peer");
        auto packet = connection.receive_packet(32U, 1U);
        require(packet.payload.size() == 4U && packet.handles.size() == 1U,
                "seqpacket payload or SCM_RIGHTS handle was lost");
        require(::fcntl(packet.handles[0].get(), F_GETFD) >= 0,
                "received SCM_RIGHTS descriptor is invalid");
        const std::array<std::byte, 2> reply{std::byte{'o'}, std::byte{'k'}};
        connection.send_packet(reply);
      });

      auto client = depthd::ipc_v2::connect_seqpacket(path);
      const std::array<std::byte, 4> request{
          std::byte{'n'}, std::byte{'u'}, std::byte{'v'}, std::byte{'o'}};
      const std::array<int, 1> handles{shared.readonly_handle()};
      client.send_packet(request, handles);
      const auto response = client.receive_packet(8U, 0U);
      require(response.payload.size() == 2U &&
                  response.payload[0] == std::byte{'o'} &&
                  response.payload[1] == std::byte{'k'},
              "seqpacket response was corrupted");
      server.join();
    }
    require(!std::filesystem::exists(directory / "data.sock"),
            "listener did not safely remove its own socket inode");
    std::filesystem::remove(directory);
    std::cout << "local_ipc_linux_test: pass\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    if (!directory.empty()) {
      std::error_code ignored;
      std::filesystem::remove(directory / "data.sock", ignored);
      std::filesystem::remove(directory, ignored);
    }
    std::cerr << "local_ipc_linux_test: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
