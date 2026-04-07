#include "Protocol.hpp"
#include "ReceiverEngine.hpp"
#include "SenderEngine.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <print>
#include <span>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

class UdpSocket {
  int fd_{-1};

public:
  UdpSocket() = default;
  explicit UdpSocket(int fd) : fd_(fd) {}

  UdpSocket(const UdpSocket &) = delete;
  UdpSocket &operator=(const UdpSocket &) = delete;

  UdpSocket(UdpSocket &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  UdpSocket &operator=(UdpSocket &&other) noexcept {
    if (this != &other) {
      reset();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  ~UdpSocket() { reset(); }

  [[nodiscard]] bool valid() const { return fd_ >= 0; }
  [[nodiscard]] int get() const { return fd_; }

  void reset() {
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }
};

UdpSocket create_udp_socket() {
  return UdpSocket(socket(AF_INET, SOCK_DGRAM, 0));
}

bool set_receive_timeout(int fd, int timeout_ms) {
  timeval timeout{};
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
  return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
         0;
}

bool set_receive_buffer_size(int fd, int bytes) {
  return setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) == 0;
}

sockaddr_in loopback_address(uint16_t port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  return address;
}

uint16_t bind_ephemeral_loopback(int fd) {
  const sockaddr_in address = loopback_address(0);
  if (bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) !=
      0) {
    return 0;
  }

  sockaddr_in bound{};
  socklen_t length = sizeof(bound);
  if (getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &length) != 0) {
    return 0;
  }

  return ntohs(bound.sin_port);
}

uint64_t monotonic_us() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

} // namespace

void test_udp_loopback_end_to_end() {
  std::print("Test: UDP Loopback End-to-End... ");

  UdpSocket receiver_socket = create_udp_socket();
  UdpSocket sender_socket = create_udp_socket();
  assert(receiver_socket.valid());
  assert(sender_socket.valid());

  assert(set_receive_timeout(receiver_socket.get(), 500));
  assert(set_receive_buffer_size(receiver_socket.get(), 1 << 23));

  const uint16_t port = bind_ephemeral_loopback(receiver_socket.get());
  assert(port != 0);
  const sockaddr_in destination = loopback_address(port);

  std::vector<float> frame(NetDSP::SHADOW_PIXEL_COUNT, 0.0f);
  auto *frame_bytes = reinterpret_cast<std::byte *>(frame.data());
  for (uint16_t fragment_index = 0;
       fragment_index < NetDSP::SenderEngine::totalFragments();
       ++fragment_index) {
    const size_t offset =
        static_cast<size_t>(fragment_index) * NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES;
    const size_t count =
        NetDSP::SenderEngine::payloadBytesForFragment(fragment_index);
    std::fill_n(frame_bytes + offset, count,
                std::byte{static_cast<uint8_t>(fragment_index % 251)});
  }

  NetDSP::ReceiverEngine<2, 32> receiver(500000,
                                         NetDSP::TimeoutPolicy::ForceCommitPartial);
  std::atomic<bool> receive_done{false};
  std::atomic<bool> receive_success{false};

  std::thread receiver_thread([&]() {
    std::array<std::byte, NetDSP::DEFAULT_PACKET_BYTES> packet{};

    while (!receive_done.load(std::memory_order_acquire)) {
      sockaddr_in peer{};
      socklen_t peer_len = sizeof(peer);
      const ssize_t bytes_received =
          recvfrom(receiver_socket.get(), packet.data(), packet.size(), 0,
                   reinterpret_cast<sockaddr *>(&peer), &peer_len);
      if (bytes_received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        break;
      }

      NetDSP::PacketHeader header{};
      const std::byte *payload = nullptr;
      size_t payload_bytes = 0;
      const bool parsed = NetDSP::parseDatagram(packet.data(),
                                                static_cast<size_t>(bytes_received),
                                                header, payload, payload_bytes);
      if (!parsed) {
        continue;
      }

      const auto result =
          receiver.onPacket(header, payload, payload_bytes, monotonic_us());
      if (result.status == NetDSP::PacketStatus::FrameCompleted) {
        receive_success.store(true, std::memory_order_release);
        receive_done.store(true, std::memory_order_release);
      }
    }
  });

  NetDSP::SenderEngine sender;
  const uint32_t sequence = sender.reserveSequence();
  std::vector<uint16_t> reverse_order(NetDSP::SenderEngine::totalFragments());
  for (uint16_t index = 0; index < reverse_order.size(); ++index) {
    reverse_order[index] =
        static_cast<uint16_t>(reverse_order.size() - 1 - index);
  }

  std::array<std::byte, NetDSP::DEFAULT_PACKET_BYTES> packet{};
  const uint64_t send_timestamp = monotonic_us();
  for (const uint16_t fragment_index : reverse_order) {
    const NetDSP::PacketHeader header = NetDSP::SenderEngine::makeHeader(
        sequence, 42, fragment_index, send_timestamp,
        NetDSP::FLAG_I_FRAME | NetDSP::FLAG_CS_ENABLED);
    const std::byte *payload =
        NetDSP::SenderEngine::payloadForFragment(frame.data(), fragment_index);
    const size_t payload_bytes =
        NetDSP::SenderEngine::payloadBytesForFragment(fragment_index);
    const size_t packet_bytes = NetDSP::serializeDatagram(
        header, payload, payload_bytes, packet.data(), packet.size());
    assert(packet_bytes == NetDSP::HEADER_SIZE + payload_bytes);

    const ssize_t sent = sendto(sender_socket.get(), packet.data(), packet_bytes,
                                0,
                                reinterpret_cast<const sockaddr *>(&destination),
                                sizeof(destination));
    assert(sent == static_cast<ssize_t>(packet_bytes));
  }

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(2);
  while (!receive_done.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  receive_done.store(true, std::memory_order_release);
  receiver_thread.join();

  assert(receive_success.load(std::memory_order_acquire));
  const auto ready = receiver.tryAcquireReadyFrame();
  assert(ready.has_value());
  assert(ready->descriptor.sequence == sequence);
  assert(ready->descriptor.frame_id == 42);
  assert(ready->descriptor.isComplete());
  assert(!ready->descriptor.isPartial());
  assert(ready->bytes[0] == std::byte{0x00});
  assert(ready->bytes[NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES] == std::byte{0x01});
  assert(ready->bytes[NetDSP::SHADOW_BUFFER_BYTES - 1] ==
         std::byte{static_cast<uint8_t>(
             (NetDSP::SenderEngine::totalFragments() - 1) % 251)});

  receiver.releaseReadyFrame(ready->slot_index);
  assert(receiver.pool().state(ready->slot_index) == NetDSP::SlotState::Free);

  std::println("PASSED");
}

int main() {
  std::println("==========================================================");
  std::println("UDP Loopback Tests");
  std::println("==========================================================\n");

  test_udp_loopback_end_to_end();

  std::println("\n==========================================================");
  std::println("All UDP loopback tests PASSED!");
  std::println("==========================================================");

  return 0;
}
