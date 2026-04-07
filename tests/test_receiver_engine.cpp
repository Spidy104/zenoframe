#include "ReceiverEngine.hpp"
#include "SenderEngine.hpp"
#include "TemporalRefresh.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <print>
#include <vector>

NetDSP::PacketHeader make_receiver_header(uint32_t sequence, uint16_t frame_id,
                                          uint16_t fragment_index) {
  return NetDSP::PacketHeader{
      .magic = NetDSP::MAGIC_NUMBER,
      .sequence = sequence,
      .frame_id = frame_id,
      .fragment_index = fragment_index,
      .total_fragments = NetDSP::SHADOW_TOTAL_FRAGMENTS,
      .timestamp_us = 987654,
      .type_flags = NetDSP::FLAG_I_FRAME | NetDSP::FLAG_CS_ENABLED,
      .quantization = NetDSP::SHADOW_FRAME_QUANTIZATION,
  };
}

void fill_receiver_payload(std::vector<std::byte> &payload, uint8_t value) {
  std::fill(payload.begin(), payload.end(), std::byte{value});
}

struct CapturedTemporalFragment {
  NetDSP::PacketHeader header{};
  std::vector<std::byte> payload;
  size_t payload_bytes{0};
};

std::vector<float> build_temporal_frame(float seed) {
  std::vector<float> frame(NetDSP::SHADOW_PIXEL_COUNT, 0.0f);
  for (uint32_t y = 0; y < NetDSP::SHADOW_FRAME_HEIGHT; ++y) {
    for (uint32_t x = 0; x < NetDSP::SHADOW_FRAME_WIDTH; ++x) {
      frame[static_cast<size_t>(y) * NetDSP::SHADOW_FRAME_WIDTH + x] =
          seed + static_cast<float>(y) * 0.01f + static_cast<float>(x) * 0.001f;
    }
  }
  return frame;
}

std::vector<float> build_phase2_frame(uint32_t frame_index) {
  std::vector<float> frame(NetDSP::SHADOW_PIXEL_COUNT, 0.0f);
  const float phase = static_cast<float>(frame_index) * 0.15f;
  for (uint32_t y = 0; y < NetDSP::SHADOW_FRAME_HEIGHT; ++y) {
    for (uint32_t x = 0; x < NetDSP::SHADOW_FRAME_WIDTH; ++x) {
      const float fx =
          static_cast<float>(x) / static_cast<float>(NetDSP::SHADOW_FRAME_WIDTH);
      const float fy = static_cast<float>(y) /
                       static_cast<float>(NetDSP::SHADOW_FRAME_HEIGHT);
      frame[static_cast<size_t>(y) * NetDSP::SHADOW_FRAME_WIDTH + x] =
          std::clamp(0.45f + 0.2f * std::sin(10.0f * fx + phase) +
                         0.2f * std::cos(6.0f * fy + 0.5f * phase),
                     0.0f, 1.0f);
    }
  }
  return frame;
}

double mean_abs_error_rows(const float *lhs, const std::vector<float> &rhs,
                           uint32_t start_row, uint32_t row_count) {
  double error = 0.0;
  size_t samples = 0;
  for (uint32_t row = start_row; row < start_row + row_count; ++row) {
    const size_t offset = static_cast<size_t>(row) * NetDSP::SHADOW_FRAME_WIDTH;
    for (uint32_t x = 0; x < NetDSP::SHADOW_FRAME_WIDTH; ++x) {
      error += std::fabs(lhs[offset + x] - rhs[offset + x]);
      ++samples;
    }
  }
  return error / static_cast<double>(samples);
}

std::vector<CapturedTemporalFragment>
capture_phase1_frame(NetDSP::SenderEngine &sender, const std::vector<float> &frame,
                     uint16_t frame_id, uint64_t timestamp_us,
                     uint64_t refresh_frame_index,
                     const NetDSP::DistributedIntraRefreshScheduler &scheduler,
                     bool seed_with_full_frame) {
  std::vector<CapturedTemporalFragment> fragments;
  const bool sent = sender.sendDistributedIntraRefreshFrame(
      frame.data(), frame_id, timestamp_us, refresh_frame_index, scheduler,
      [&](const NetDSP::PacketHeader &header, const std::byte *payload,
          size_t payload_bytes) {
        fragments.push_back(CapturedTemporalFragment{
            .header = header,
            .payload = std::vector<std::byte>(payload, payload + payload_bytes),
            .payload_bytes = payload_bytes,
        });
        return true;
      },
      seed_with_full_frame);
  assert(sent);
  return fragments;
}

void test_receiver_duplicate_and_partial_tracking() {
  std::print("Test: Receiver Duplicate + Partial Assembly... ");

  NetDSP::ReceiverEngine<2, 8> receiver;
  std::vector<std::byte> payload(NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES);

  fill_receiver_payload(payload, 0x34);
  const auto first =
      receiver.onPacket(make_receiver_header(100, 7, 1), payload.data(),
                        payload.size(), 1000);
  assert(first.status == NetDSP::PacketStatus::AcceptedFragment);

  const auto duplicate =
      receiver.onPacket(make_receiver_header(100, 7, 1), payload.data(),
                        payload.size(), 1100);
  assert(duplicate.status == NetDSP::PacketStatus::DuplicateFragment);
  assert(duplicate.slot_index == first.slot_index);

  fill_receiver_payload(payload, 0x12);
  const auto second =
      receiver.onPacket(make_receiver_header(100, 7, 0), payload.data(),
                        payload.size(), 1200);
  assert(second.status == NetDSP::PacketStatus::AcceptedFragment);
  assert(second.slot_index == first.slot_index);

  const auto assembly = receiver.pool().assemblyState(first.slot_index);
  assert(assembly.received_fragments == 2);
  assert(receiver.pool().hasReceivedFragment(first.slot_index, 0));
  assert(receiver.pool().hasReceivedFragment(first.slot_index, 1));
  assert(receiver.queuedFrameCount() == 0);

  std::println("PASSED");
}

void test_receiver_end_to_end_commit() {
  std::print("Test: Receiver End-to-End Frame Assembly... ");

  NetDSP::ReceiverEngine<2, 8> receiver;
  std::vector<std::byte> payload(NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES);
  std::vector<std::byte> last_payload(
      NetDSP::FrameBufferPool<2>::expectedFragmentPayloadBytes(
          NetDSP::SHADOW_TOTAL_FRAGMENTS - 1));

  for (uint16_t fragment_index = NetDSP::SHADOW_TOTAL_FRAGMENTS;
       fragment_index-- > 0;) {
    const auto header = make_receiver_header(200, 11, fragment_index);

    if (fragment_index == NetDSP::SHADOW_TOTAL_FRAGMENTS - 1) {
      fill_receiver_payload(last_payload, 0xEF);
      const auto result =
          receiver.onPacket(header, last_payload.data(), last_payload.size(),
                            2000 + fragment_index);
      assert(result.accepted());
      continue;
    }

    fill_receiver_payload(payload, static_cast<uint8_t>(fragment_index % 251));
    const auto result =
        receiver.onPacket(header, payload.data(), payload.size(),
                          2000 + fragment_index);
    if (fragment_index == 0) {
      assert(result.status == NetDSP::PacketStatus::FrameCompleted);
    } else {
      assert(result.status == NetDSP::PacketStatus::AcceptedFragment);
    }
  }

  assert(receiver.queuedFrameCount() == 1);

  const auto ready = receiver.tryAcquireReadyFrame();
  assert(ready.has_value());
  assert(ready->descriptor.sequence == 200);
  assert(ready->descriptor.frame_id == 11);
  assert(ready->descriptor.bytes_used == NetDSP::SHADOW_BUFFER_BYTES);
  assert(ready->descriptor.isComplete());
  assert(!ready->descriptor.isPartial());
  assert(ready->descriptor.missing_fragments == 0);
  assert(reinterpret_cast<uintptr_t>(ready->pixels) %
             NetDSP::SHADOW_BUFFER_ALIGNMENT ==
         0);

  assert(ready->bytes[0] == std::byte{0x00});
  assert(ready->bytes[NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES] == std::byte{0x01});
  assert(ready->bytes[NetDSP::SHADOW_BUFFER_BYTES - 1] == std::byte{0xEF});

  receiver.releaseReadyFrame(ready->slot_index);
  assert(receiver.pool().state(ready->slot_index) == NetDSP::SlotState::Free);
  assert(receiver.queuedFrameCount() == 0);

  std::println("PASSED");
}

void test_receiver_timeout_force_commit_partial() {
  std::print("Test: Receiver Timeout Force-Commit... ");

  NetDSP::ReceiverEngine<2, 8> receiver(
      10000, NetDSP::TimeoutPolicy::ForceCommitPartial);
  std::vector<std::byte> payload(NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES);
  fill_receiver_payload(payload, 0x7A);

  const auto first =
      receiver.onPacket(make_receiver_header(300, 21, 5), payload.data(),
                        payload.size(), 1000);
  assert(first.status == NetDSP::PacketStatus::AcceptedFragment);
  assert(receiver.queuedFrameCount() == 0);

  const NetDSP::SweepResult sweep = receiver.pollExpiredFrames(11001);
  assert(sweep.committed == 1);
  assert(sweep.dropped == 0);
  assert(sweep.queue_full == 0);
  assert(receiver.queuedFrameCount() == 1);

  const auto ready = receiver.tryAcquireReadyFrame();
  assert(ready.has_value());
  assert(ready->descriptor.isPartial());
  assert(!ready->descriptor.isComplete());
  assert(ready->descriptor.fragments_received == 1);
  assert(ready->descriptor.missing_fragments ==
         NetDSP::SHADOW_TOTAL_FRAGMENTS - 1);
  assert(ready->bytes[static_cast<size_t>(5) * NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES] ==
         std::byte{0x7A});

  receiver.releaseReadyFrame(ready->slot_index);
  assert(receiver.pool().state(ready->slot_index) == NetDSP::SlotState::Free);

  std::println("PASSED");
}

void test_receiver_timeout_drop_partial() {
  std::print("Test: Receiver Timeout Drop Partial... ");

  NetDSP::ReceiverEngine<2, 8> receiver(10000,
                                        NetDSP::TimeoutPolicy::DropPartial);
  std::vector<std::byte> payload(NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES);
  fill_receiver_payload(payload, 0x55);

  const auto first =
      receiver.onPacket(make_receiver_header(400, 33, 2), payload.data(),
                        payload.size(), 2000);
  assert(first.status == NetDSP::PacketStatus::AcceptedFragment);

  const NetDSP::SweepResult sweep = receiver.pollExpiredFrames(12001);
  assert(sweep.committed == 0);
  assert(sweep.dropped == 1);
  assert(sweep.queue_full == 0);
  assert(receiver.queuedFrameCount() == 0);
  assert(receiver.pool().state(first.slot_index) == NetDSP::SlotState::Free);

  std::println("PASSED");
}

void test_receiver_late_fragments_ignored_after_force_commit() {
  std::print("Test: Receiver Ignores Late Fragments After Force-Commit... ");

  NetDSP::ReceiverEngine<2, 8> receiver(
      10000, NetDSP::TimeoutPolicy::ForceCommitPartial);
  std::vector<std::byte> payload(NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES);
  fill_receiver_payload(payload, 0x6A);

  const auto first =
      receiver.onPacket(make_receiver_header(500, 41, 5), payload.data(),
                        payload.size(), 1000);
  assert(first.status == NetDSP::PacketStatus::AcceptedFragment);

  const NetDSP::SweepResult sweep = receiver.pollExpiredFrames(12001);
  assert(sweep.committed == 1);

  const auto late =
      receiver.onPacket(make_receiver_header(500, 41, 6), payload.data(),
                        payload.size(), 13000);
  assert(late.status == NetDSP::PacketStatus::LateFragment);
  assert(receiver.queuedFrameCount() == 1);

  const auto ready = receiver.tryAcquireReadyFrame();
  assert(ready.has_value());
  assert(ready->descriptor.sequence == 500);
  assert(ready->descriptor.frame_id == 41);
  assert(ready->descriptor.fragments_received == 1);
  receiver.releaseReadyFrame(ready->slot_index);

  std::println("PASSED");
}

void test_receiver_late_fragments_ignored_after_drop() {
  std::print("Test: Receiver Ignores Late Fragments After Drop... ");

  NetDSP::ReceiverEngine<2, 8> receiver(10000,
                                        NetDSP::TimeoutPolicy::DropPartial);
  std::vector<std::byte> payload(NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES);
  fill_receiver_payload(payload, 0x4C);

  const auto first =
      receiver.onPacket(make_receiver_header(600, 51, 5), payload.data(),
                        payload.size(), 1000);
  assert(first.status == NetDSP::PacketStatus::AcceptedFragment);

  const NetDSP::SweepResult sweep = receiver.pollExpiredFrames(12001);
  assert(sweep.dropped == 1);

  const auto late =
      receiver.onPacket(make_receiver_header(600, 51, 6), payload.data(),
                        payload.size(), 13000);
  assert(late.status == NetDSP::PacketStatus::LateFragment);
  assert(receiver.queuedFrameCount() == 0);

  std::println("PASSED");
}

void test_receiver_late_fragments_ignored_after_full_completion() {
  std::print("Test: Receiver Ignores Late Fragments After Full Completion... ");

  NetDSP::ReceiverEngine<2, 8> receiver;
  std::vector<std::byte> payload(NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES);
  std::vector<std::byte> last_payload(
      NetDSP::FrameBufferPool<2>::expectedFragmentPayloadBytes(
          NetDSP::SHADOW_TOTAL_FRAGMENTS - 1));

  for (uint16_t fragment_index = NetDSP::SHADOW_TOTAL_FRAGMENTS;
       fragment_index-- > 0;) {
    const auto header = make_receiver_header(700, 61, fragment_index);

    if (fragment_index == NetDSP::SHADOW_TOTAL_FRAGMENTS - 1) {
      fill_receiver_payload(last_payload, 0xEF);
      const auto result =
          receiver.onPacket(header, last_payload.data(), last_payload.size(),
                            2000 + fragment_index);
      assert(result.accepted());
      continue;
    }

    fill_receiver_payload(payload, static_cast<uint8_t>(fragment_index % 251));
    const auto result =
        receiver.onPacket(header, payload.data(), payload.size(),
                          2000 + fragment_index);
    assert(result.accepted());
  }

  const auto late =
      receiver.onPacket(make_receiver_header(700, 61, 3), payload.data(),
                        payload.size(), 200000);
  assert(late.status == NetDSP::PacketStatus::LateFragment);
  assert(receiver.queuedFrameCount() == 1);

  const auto ready = receiver.tryAcquireReadyFrame();
  assert(ready.has_value());
  assert(ready->descriptor.isComplete());
  receiver.releaseReadyFrame(ready->slot_index);

  std::println("PASSED");
}

void test_receiver_phase1_temporal_refresh_reconstructs_reference() {
  std::print("Test: Receiver Phase 1 Temporal Refresh Reconstruction... ");

  NetDSP::SenderEngine sender;
  NetDSP::ReceiverEngine<4, 8> receiver(
      20000, NetDSP::TimeoutPolicy::DropPartial, -1.0f);
  const NetDSP::DistributedIntraRefreshScheduler scheduler(
      NetDSP::SHADOW_FRAME_HEIGHT, 32);

  const std::vector<float> seed_frame = build_temporal_frame(10.0f);
  const auto seed_fragments = capture_phase1_frame(
      sender, seed_frame, 801, 100000, 0, scheduler, true);
  for (auto it = seed_fragments.rbegin(); it != seed_fragments.rend(); ++it) {
    assert(receiver.onPacket(it->header, it->payload.data(), it->payload_bytes,
                             200000)
               .accepted());
  }
  const auto seeded = receiver.tryAcquireReadyFrame();
  assert(seeded.has_value());
  assert(std::memcmp(seeded->bytes, seed_frame.data(),
                     NetDSP::SHADOW_BUFFER_BYTES) == 0);
  receiver.releaseReadyFrame(seeded->slot_index);

  const std::vector<float> refreshed_frame = build_temporal_frame(50.0f);
  const NetDSP::RefreshPlan plan = scheduler.planForFrame(3);
  const auto refresh_fragments = capture_phase1_frame(
      sender, refreshed_frame, 802, 101000, 3, scheduler, false);
  for (auto it = refresh_fragments.rbegin(); it != refresh_fragments.rend();
       ++it) {
    assert(receiver.onPacket(it->header, it->payload.data(), it->payload_bytes,
                             201000)
               .accepted());
  }

  const auto ready = receiver.tryAcquireReadyFrame();
  assert(ready.has_value());
  assert(ready->descriptor.usesTemporalRefresh());
  assert(ready->descriptor.refresh_start_row == plan.spans[0].start_row);
  assert(ready->descriptor.refresh_row_count == plan.payloadRowCount());
  assert(ready->descriptor.bytes_used < NetDSP::SHADOW_BUFFER_BYTES);

  std::vector<float> expected = seed_frame;
  for (uint32_t y = 0; y < NetDSP::SHADOW_FRAME_HEIGHT; ++y) {
    if (!plan.coversRow(y)) {
      continue;
    }
    const size_t offset = static_cast<size_t>(y) * NetDSP::SHADOW_FRAME_WIDTH;
    std::copy_n(refreshed_frame.data() + offset, NetDSP::SHADOW_FRAME_WIDTH,
                expected.data() + offset);
  }

  assert(std::memcmp(ready->bytes, expected.data(),
                     NetDSP::SHADOW_BUFFER_BYTES) == 0);
  receiver.releaseReadyFrame(ready->slot_index);

  std::println("PASSED");
}

void test_receiver_phase1_temporal_timeout_drops_partial_refresh() {
  std::print("Test: Receiver Phase 1 Temporal Timeout Drops Partial... ");

  NetDSP::SenderEngine sender;
  NetDSP::ReceiverEngine<4, 8> receiver(
      5000, NetDSP::TimeoutPolicy::DropPartial, -1.0f);
  const NetDSP::DistributedIntraRefreshScheduler scheduler(
      NetDSP::SHADOW_FRAME_HEIGHT, 32);

  const std::vector<float> seed_frame = build_temporal_frame(20.0f);
  const auto seed_fragments = capture_phase1_frame(
      sender, seed_frame, 901, 200000, 0, scheduler, true);
  for (const auto &fragment : seed_fragments) {
    assert(receiver.onPacket(fragment.header, fragment.payload.data(),
                             fragment.payload_bytes, 300000)
               .accepted());
  }
  const auto seeded = receiver.tryAcquireReadyFrame();
  assert(seeded.has_value());
  receiver.releaseReadyFrame(seeded->slot_index);

  const std::vector<float> refreshed_frame = build_temporal_frame(60.0f);
  const auto refresh_fragments = capture_phase1_frame(
      sender, refreshed_frame, 902, 201000, 1, scheduler, false);
  assert(refresh_fragments.size() > 1);

  for (size_t index = 0; index + 1 < refresh_fragments.size(); ++index) {
    assert(receiver
               .onPacket(refresh_fragments[index].header,
                         refresh_fragments[index].payload.data(),
                         refresh_fragments[index].payload_bytes, 301000 + index)
               .status == NetDSP::PacketStatus::AcceptedFragment);
  }

  const auto sweep =
      receiver.pollExpiredFrames(301000 + receiver.frameTimeoutUs() + 10);
  assert(sweep.committed == 0);
  assert(sweep.dropped == 1);
  assert(!receiver.tryAcquireReadyFrame().has_value());

  const auto late = receiver.onPacket(
      refresh_fragments.back().header, refresh_fragments.back().payload.data(),
      refresh_fragments.back().payload_bytes,
      301000 + receiver.frameTimeoutUs() + 20);
  assert(late.status == NetDSP::PacketStatus::LateFragment);
  assert(receiver.reference().at(0, 0) == seed_frame[0]);

  std::println("PASSED");
}

void test_receiver_phase2_temporal_timeout_conceals_partial_refresh() {
  std::print("Test: Receiver Phase 2 Conceals Partial Refresh On Timeout... ");

  NetDSP::SenderEngine sender;
  NetDSP::ReceiverEngine<4, 8> receiver(
      5000, NetDSP::TimeoutPolicy::ForceCommitPartial, 0.0f);
  const NetDSP::DistributedIntraRefreshScheduler scheduler(
      NetDSP::SHADOW_FRAME_HEIGHT, 32);

  const std::vector<float> seed_frame = build_phase2_frame(0);
  const auto seed_fragments = capture_phase1_frame(
      sender, seed_frame, 1001, 400000, 0, scheduler, true);
  for (const auto &fragment : seed_fragments) {
    assert(receiver.onPacket(fragment.header, fragment.payload.data(),
                             fragment.payload_bytes, 500000)
               .accepted());
  }
  const auto seeded = receiver.tryAcquireReadyFrame();
  assert(seeded.has_value());
  receiver.releaseReadyFrame(seeded->slot_index);

  const std::vector<float> refreshed_frame = build_phase2_frame(1);
  const NetDSP::RefreshPlan plan = scheduler.planForFrame(4);
  const auto refresh_fragments = capture_phase1_frame(
      sender, refreshed_frame, 1002, 401000, 4, scheduler, false);

  constexpr uint32_t missing_payload_row_start = 10;
  constexpr uint32_t missing_payload_row_count = 4;
  const size_t row_bytes =
      static_cast<size_t>(NetDSP::SHADOW_FRAME_WIDTH) * sizeof(float);
  const size_t missing_begin = static_cast<size_t>(missing_payload_row_start) * row_bytes;
  const size_t missing_end = static_cast<size_t>(missing_payload_row_start +
                                                 missing_payload_row_count) *
                             row_bytes;

  for (const auto &fragment : refresh_fragments) {
    const size_t fragment_begin =
        static_cast<size_t>(fragment.header.fragment_index) *
        NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES;
    const size_t fragment_end = fragment_begin + fragment.payload_bytes;
    const bool overlaps_missing =
        fragment_begin < missing_end && fragment_end > missing_begin;
    if (overlaps_missing) {
      continue;
    }

    assert(receiver.onPacket(fragment.header, fragment.payload.data(),
                             fragment.payload_bytes, 501000)
               .accepted());
  }

  const auto sweep =
      receiver.pollExpiredFrames(501000 + receiver.frameTimeoutUs() + 10);
  assert(sweep.committed == 1);
  assert(sweep.dropped == 0);

  const auto ready = receiver.tryAcquireReadyFrame();
  assert(ready.has_value());
  assert(ready->descriptor.usesTemporalRefresh());
  assert(ready->descriptor.isPartial());
  assert(ready->descriptor.missing_fragments > 0);

  const uint32_t frame_missing_start =
      plan.spans[0].start_row + missing_payload_row_start;
  const double stale_mae = mean_abs_error_rows(
      seed_frame.data(), refreshed_frame, frame_missing_start,
      missing_payload_row_count);
  const double concealed_mae = mean_abs_error_rows(
      ready->pixels, refreshed_frame, frame_missing_start,
      missing_payload_row_count);

  std::println("\n  debug: stale_mae={:.8f} concealed_mae={:.8f} missing_rows=[{}, {}) "
               "missing_fragments={}",
               stale_mae, concealed_mae, frame_missing_start,
               frame_missing_start + missing_payload_row_count,
               ready->descriptor.missing_fragments);
  assert(concealed_mae < stale_mae);

  receiver.releaseReadyFrame(ready->slot_index);
  std::println("PASSED");
}

int main() {
  std::println("==========================================================");
  std::println("Receiver Engine Tests");
  std::println("==========================================================\n");

  test_receiver_duplicate_and_partial_tracking();
  test_receiver_end_to_end_commit();
  test_receiver_timeout_force_commit_partial();
  test_receiver_timeout_drop_partial();
  test_receiver_late_fragments_ignored_after_force_commit();
  test_receiver_late_fragments_ignored_after_drop();
  test_receiver_late_fragments_ignored_after_full_completion();
  test_receiver_phase1_temporal_refresh_reconstructs_reference();
  test_receiver_phase1_temporal_timeout_drops_partial_refresh();
  test_receiver_phase2_temporal_timeout_conceals_partial_refresh();

  std::println("\n==========================================================");
  std::println("All receiver tests PASSED!");
  std::println("==========================================================");

  return 0;
}
