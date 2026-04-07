#include "Protocol.hpp"
#include "ReceiverEngine.hpp"
#include "SenderEngine.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <print>
#include <vector>

namespace {

struct CapturedFragment {
  NetDSP::PacketHeader header{};
  std::vector<std::byte> payload;
  size_t payload_bytes{0};
};

struct ExpectedFrame {
  uint32_t seed{0};
  uint32_t sequence{0};
  uint16_t frame_id{0};
  bool partial{false};
  std::vector<uint16_t> dropped_fragments;
};

std::vector<float> build_frame(uint32_t seed) {
  std::vector<float> frame(NetDSP::SHADOW_PIXEL_COUNT, 0.0f);
  auto *frame_bytes = reinterpret_cast<std::byte *>(frame.data());

  for (uint16_t fragment_index = 0;
       fragment_index < NetDSP::SenderEngine::totalFragments();
       ++fragment_index) {
    const size_t offset =
        static_cast<size_t>(fragment_index) * NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES;
    const size_t count =
        NetDSP::SenderEngine::payloadBytesForFragment(fragment_index);
    const std::byte value =
        std::byte{static_cast<uint8_t>((seed * 13u + fragment_index) % 251u)};
    std::fill_n(frame_bytes + offset, count, value);
  }

  return frame;
}

std::vector<CapturedFragment> capture_frame(NetDSP::SenderEngine &sender,
                                            const std::vector<float> &frame,
                                            uint16_t frame_id,
                                            uint64_t timestamp_us) {
  std::vector<CapturedFragment> captured;

  const bool sent = sender.sendFrame(
      frame.data(), frame_id, timestamp_us,
      [&](const NetDSP::PacketHeader &header, const std::byte *payload,
          size_t payload_bytes) {
        captured.push_back(CapturedFragment{
            .header = header,
            .payload = std::vector<std::byte>(payload, payload + payload_bytes),
            .payload_bytes = payload_bytes,
        });
        return true;
      });

  assert(sent);
  return captured;
}

bool contains_fragment(const std::vector<uint16_t> &fragments,
                       uint16_t fragment_index) {
  return std::find(fragments.begin(), fragments.end(), fragment_index) !=
         fragments.end();
}

size_t dropped_bytes(const std::vector<uint16_t> &dropped_fragments) {
  size_t total = 0;
  for (const uint16_t fragment_index : dropped_fragments) {
    total += NetDSP::SenderEngine::payloadBytesForFragment(fragment_index);
  }
  return total;
}

std::vector<uint16_t> make_reverse_order() {
  std::vector<uint16_t> order(NetDSP::SenderEngine::totalFragments());
  for (uint16_t index = 0; index < order.size(); ++index) {
    order[index] = static_cast<uint16_t>(order.size() - 1 - index);
  }
  return order;
}

std::vector<uint16_t> make_even_odd_order() {
  std::vector<uint16_t> order;
  order.reserve(NetDSP::SenderEngine::totalFragments());

  for (uint16_t index = 0; index < NetDSP::SenderEngine::totalFragments(); index += 2) {
    order.push_back(index);
  }
  for (uint16_t index = 1; index < NetDSP::SenderEngine::totalFragments(); index += 2) {
    order.push_back(index);
  }

  return order;
}

void verify_ready_frame(const NetDSP::ReadyFrame &ready,
                        const ExpectedFrame &expected) {
  const std::vector<float> source_frame = build_frame(expected.seed);
  const auto *source_bytes =
      reinterpret_cast<const std::byte *>(source_frame.data());

  assert(ready.descriptor.sequence == expected.sequence);
  assert(ready.descriptor.frame_id == expected.frame_id);

  if (!expected.partial) {
    assert(ready.descriptor.isComplete());
    assert(!ready.descriptor.isPartial());
    assert(ready.descriptor.bytes_used == NetDSP::SHADOW_BUFFER_BYTES);
    assert(std::memcmp(ready.bytes, source_bytes, NetDSP::SHADOW_BUFFER_BYTES) ==
           0);
    return;
  }

  assert(!ready.descriptor.isComplete());
  assert(ready.descriptor.isPartial());
  assert(ready.descriptor.missing_fragments == expected.dropped_fragments.size());
  assert(ready.descriptor.fragments_received +
             ready.descriptor.missing_fragments ==
         NetDSP::SenderEngine::totalFragments());
  assert(ready.descriptor.bytes_used ==
         NetDSP::SHADOW_BUFFER_BYTES - dropped_bytes(expected.dropped_fragments));

  for (uint16_t fragment_index = 0;
       fragment_index < NetDSP::SenderEngine::totalFragments(); ++fragment_index) {
    const size_t offset =
        static_cast<size_t>(fragment_index) * NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES;
    const size_t count =
        NetDSP::SenderEngine::payloadBytesForFragment(fragment_index);
    if (!contains_fragment(expected.dropped_fragments, fragment_index)) {
      assert(std::memcmp(ready.bytes + offset, source_bytes + offset, count) == 0);
    }
  }
}

void test_mixed_multiframe_transport_stress() {
  std::print("Test: Mixed Multi-Frame Transport Stress... ");

  NetDSP::SenderEngine sender;
  NetDSP::ReceiverEngine<16, 32> receiver(
      20000, NetDSP::TimeoutPolicy::ForceCommitPartial);
  std::vector<ExpectedFrame> expected_ready_frames;
  uint64_t now_us = 100000;

  const std::array<std::vector<uint16_t>, 3> partial_drop_sets = {
      std::vector<uint16_t>{1, 31, NetDSP::SenderEngine::totalFragments() - 1},
      std::vector<uint16_t>{2, 256, NetDSP::SenderEngine::totalFragments() / 2},
      std::vector<uint16_t>{3, 511, NetDSP::SenderEngine::totalFragments() - 2}};

  for (uint16_t frame_number = 0; frame_number < 9; ++frame_number) {
    const uint32_t seed = 50u + frame_number;
    const uint16_t frame_id = static_cast<uint16_t>(200 + frame_number);
    const uint64_t timestamp_us = now_us;
    const std::vector<float> frame = build_frame(seed);
    const auto fragments = capture_frame(sender, frame, frame_id, timestamp_us);
    const uint32_t sequence = fragments.front().header.sequence;

    std::vector<uint16_t> order;
    if (frame_number % 3 == 0) {
      order = make_reverse_order();
    } else if (frame_number % 3 == 1) {
      order = make_even_odd_order();
    } else {
      order.resize(NetDSP::SenderEngine::totalFragments());
      std::iota(order.begin(), order.end(), uint16_t{0});
    }

    const bool partial = (frame_number % 2) == 1;
    const std::vector<uint16_t> dropped =
        partial ? partial_drop_sets[frame_number % partial_drop_sets.size()]
                : std::vector<uint16_t>{};

    bool saw_completion = false;
    for (const uint16_t fragment_index : order) {
      if (contains_fragment(dropped, fragment_index)) {
        continue;
      }

      const auto result = receiver.onPacket(
          fragments[fragment_index].header, fragments[fragment_index].payload.data(),
          fragments[fragment_index].payload_bytes, now_us++);
      if (fragment_index == order.front()) {
        const auto duplicate = receiver.onPacket(
            fragments[fragment_index].header,
            fragments[fragment_index].payload.data(),
            fragments[fragment_index].payload_bytes, now_us++);
        assert(duplicate.status == NetDSP::PacketStatus::DuplicateFragment);
      }

      if (!partial && !saw_completion &&
          result.status == NetDSP::PacketStatus::FrameCompleted) {
        saw_completion = true;
      } else if (!partial) {
        assert(result.status == NetDSP::PacketStatus::AcceptedFragment ||
               result.status == NetDSP::PacketStatus::FrameCompleted);
      } else {
        assert(result.status == NetDSP::PacketStatus::AcceptedFragment ||
               result.status == NetDSP::PacketStatus::DuplicateFragment);
      }
    }

    if (partial) {
      const auto sweep = receiver.pollExpiredFrames(now_us + receiver.frameTimeoutUs() + 1);
      assert(sweep.committed == 1);
      assert(sweep.dropped == 0);
      const uint16_t late_fragment = dropped.front();
      const auto late = receiver.onPacket(
          fragments[late_fragment].header, fragments[late_fragment].payload.data(),
          fragments[late_fragment].payload_bytes,
          now_us + receiver.frameTimeoutUs() + 2);
      assert(late.status == NetDSP::PacketStatus::LateFragment);
    } else {
      assert(saw_completion);
      const auto late = receiver.onPacket(
          fragments[order.front()].header, fragments[order.front()].payload.data(),
          fragments[order.front()].payload_bytes, now_us++);
      assert(late.status == NetDSP::PacketStatus::LateFragment);
    }

    expected_ready_frames.push_back(ExpectedFrame{
        .seed = seed,
        .sequence = sequence,
        .frame_id = frame_id,
        .partial = partial,
        .dropped_fragments = dropped,
    });

    now_us += receiver.frameTimeoutUs() + 10;
  }

  assert(receiver.queuedFrameCount() == expected_ready_frames.size());
  for (const auto &expected : expected_ready_frames) {
    const auto ready = receiver.tryAcquireReadyFrame();
    assert(ready.has_value());
    verify_ready_frame(*ready, expected);
    receiver.releaseReadyFrame(ready->slot_index);
  }
  assert(receiver.queuedFrameCount() == 0);

  std::println("PASSED");
}

} // namespace

int main() {
  std::println("==========================================================");
  std::println("Transport Stress Tests");
  std::println("==========================================================\n");

  test_mixed_multiframe_transport_stress();

  std::println("\n==========================================================");
  std::println("All transport stress tests PASSED!");
  std::println("==========================================================");
  return 0;
}