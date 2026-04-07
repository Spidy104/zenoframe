#include "Protocol.hpp"
#include "ReceiverEngine.hpp"
#include "SenderEngine.hpp"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <print>
#include <random>
#include <vector>

namespace {

struct CapturedFragment {
  NetDSP::PacketHeader header{};
  std::vector<std::byte> payload;
  size_t payload_bytes{0};
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
        std::byte{static_cast<uint8_t>((seed * 17u + fragment_index * 3u) % 251u)};
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

void verify_ready_frame(const NetDSP::ReadyFrame &ready,
                        const std::vector<float> &source_frame,
                        const std::vector<uint16_t> &dropped_fragments,
                        bool partial) {
  const auto *source_bytes =
      reinterpret_cast<const std::byte *>(source_frame.data());

  if (!partial) {
    assert(ready.descriptor.isComplete());
    assert(!ready.descriptor.isPartial());
    assert(std::memcmp(ready.bytes, source_bytes, NetDSP::SHADOW_BUFFER_BYTES) ==
           0);
    return;
  }

  assert(!ready.descriptor.isComplete());
  assert(ready.descriptor.isPartial());
  assert(ready.descriptor.missing_fragments == dropped_fragments.size());
  assert(ready.descriptor.bytes_used ==
         NetDSP::SHADOW_BUFFER_BYTES - dropped_bytes(dropped_fragments));

  for (uint16_t fragment_index = 0;
       fragment_index < NetDSP::SenderEngine::totalFragments(); ++fragment_index) {
    const size_t offset =
        static_cast<size_t>(fragment_index) * NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES;
    const size_t count =
        NetDSP::SenderEngine::payloadBytesForFragment(fragment_index);

    if (!contains_fragment(dropped_fragments, fragment_index)) {
      assert(std::memcmp(ready.bytes + offset, source_bytes + offset, count) == 0);
    }
  }
}

void test_seeded_randomized_transport_properties() {
  std::print("Test: Seeded Randomized Transport Properties... ");

  std::mt19937 rng(1337u);
  std::bernoulli_distribution choose_drop(0.5);
  std::bernoulli_distribution choose_duplicate(0.15);
  std::uniform_int_distribution<uint16_t> fragment_pick(
      0, NetDSP::SenderEngine::totalFragments() - 1);

  for (uint32_t trial = 0; trial < 4; ++trial) {
    const NetDSP::TimeoutPolicy policy =
        (trial % 2 == 0) ? NetDSP::TimeoutPolicy::ForceCommitPartial
                         : NetDSP::TimeoutPolicy::DropPartial;
    NetDSP::ReceiverEngine<8, 16> receiver(20000, policy);
    NetDSP::SenderEngine sender;
    uint64_t now_us = 1000 + static_cast<uint64_t>(trial) * 100000;

    for (uint16_t frame_number = 0; frame_number < 4; ++frame_number) {
      const uint32_t seed = 300u + trial * 10u + frame_number;
      const uint16_t frame_id = static_cast<uint16_t>(500 + trial * 10 + frame_number);
      const std::vector<float> source_frame = build_frame(seed);
      auto fragments = capture_frame(sender, source_frame, frame_id, now_us);

      std::vector<uint16_t> order(NetDSP::SenderEngine::totalFragments());
      std::iota(order.begin(), order.end(), uint16_t{0});
      std::shuffle(order.begin(), order.end(), rng);

      std::vector<uint16_t> dropped_fragments;
      if (choose_drop(rng)) {
        const uint16_t first = fragment_pick(rng);
        dropped_fragments.push_back(first);
        const uint16_t second = fragment_pick(rng);
        if (second != first) {
          dropped_fragments.push_back(second);
        }
      }

      bool saw_completion = false;
      size_t duplicate_count = 0;
      for (const uint16_t fragment_index : order) {
        if (contains_fragment(dropped_fragments, fragment_index)) {
          continue;
        }

        const auto result = receiver.onPacket(
            fragments[fragment_index].header, fragments[fragment_index].payload.data(),
            fragments[fragment_index].payload_bytes, now_us++);
        assert(result.status == NetDSP::PacketStatus::AcceptedFragment ||
               result.status == NetDSP::PacketStatus::FrameCompleted);
        saw_completion = saw_completion ||
                        result.status == NetDSP::PacketStatus::FrameCompleted;

        if (choose_duplicate(rng)) {
          const auto duplicate = receiver.onPacket(
              fragments[fragment_index].header,
              fragments[fragment_index].payload.data(),
              fragments[fragment_index].payload_bytes, now_us++);
          assert(duplicate.status == NetDSP::PacketStatus::DuplicateFragment ||
                 duplicate.status == NetDSP::PacketStatus::LateFragment);
          ++duplicate_count;
        }
      }

      assert(duplicate_count <= order.size());

      const bool expected_partial =
          !dropped_fragments.empty() &&
          policy == NetDSP::TimeoutPolicy::ForceCommitPartial;
      const bool expected_drop =
          !dropped_fragments.empty() && policy == NetDSP::TimeoutPolicy::DropPartial;

      if (expected_partial || expected_drop) {
        const auto sweep = receiver.pollExpiredFrames(now_us + receiver.frameTimeoutUs() + 1);
        if (expected_partial) {
          assert(sweep.committed == 1);
          assert(sweep.dropped == 0);
        } else {
          assert(sweep.committed == 0);
          assert(sweep.dropped == 1);
        }
      } else {
        assert(saw_completion);
      }

      const uint16_t late_fragment =
          dropped_fragments.empty() ? order.front() : dropped_fragments.front();
      const auto late = receiver.onPacket(
          fragments[late_fragment].header, fragments[late_fragment].payload.data(),
          fragments[late_fragment].payload_bytes,
          now_us + receiver.frameTimeoutUs() + 2);
      assert(late.status == NetDSP::PacketStatus::LateFragment);

      if (expected_drop) {
        assert(!receiver.tryAcquireReadyFrame().has_value());
      } else {
        const auto ready = receiver.tryAcquireReadyFrame();
        assert(ready.has_value());
        verify_ready_frame(*ready, source_frame, dropped_fragments, expected_partial);
        receiver.releaseReadyFrame(ready->slot_index);
      }

      now_us += receiver.frameTimeoutUs() + 100;
    }
  }

  std::println("PASSED");
}

} // namespace

int main() {
  std::println("==========================================================");
  std::println("Randomized Transport Tests");
  std::println("==========================================================\n");

  test_seeded_randomized_transport_properties();

  std::println("\n==========================================================");
  std::println("All randomized transport tests PASSED!");
  std::println("==========================================================");
  return 0;
}