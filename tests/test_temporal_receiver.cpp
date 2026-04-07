#include "Protocol.hpp"
#include "SenderEngine.hpp"
#include "TemporalReceiverEngine.hpp"
#include "TemporalRefresh.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <print>
#include <vector>

namespace {

std::vector<float> build_frame(float seed) {
  std::vector<float> frame(NetDSP::SHADOW_PIXEL_COUNT, 0.0f);
  for (uint32_t y = 0; y < NetDSP::SHADOW_FRAME_HEIGHT; ++y) {
    for (uint32_t x = 0; x < NetDSP::SHADOW_FRAME_WIDTH; ++x) {
      frame[static_cast<size_t>(y) * NetDSP::SHADOW_FRAME_WIDTH + x] =
          seed + static_cast<float>(y) * 0.01f + static_cast<float>(x) * 0.001f;
    }
  }
  return frame;
}

struct DatagramFragment {
  NetDSP::PacketHeader header{};
  std::vector<std::byte> packet;
};

std::vector<DatagramFragment> build_temporal_datagrams(
    NetDSP::SenderEngine &sender, const std::vector<float> &frame,
    uint16_t frame_id, uint64_t timestamp_us, const NetDSP::RefreshPlan &plan) {
  std::vector<DatagramFragment> datagrams;

  const bool sent = sender.sendTemporalRefresh(
      frame.data(), frame_id, timestamp_us, plan,
      [&](const NetDSP::PacketHeader &header, const std::byte *payload,
          size_t payload_bytes) {
        std::vector<std::byte> packet(NetDSP::HEADER_SIZE + payload_bytes);
        const size_t packet_bytes = NetDSP::serializeDatagram(
            header, payload, payload_bytes, packet.data(), packet.size());
        assert(packet_bytes == packet.size());
        datagrams.push_back({.header = header, .packet = std::move(packet)});
        return true;
      });

  assert(sent);
  return datagrams;
}

void test_temporal_round_trip_updates_reference_buffer() {
  std::print("Test: Temporal Round Trip Updates Reference Buffer... ");

  NetDSP::SenderEngine sender;
  NetDSP::TemporalReceiverEngine<4, 8> receiver(20000, -1.0f);
  const NetDSP::DistributedIntraRefreshScheduler scheduler(
      NetDSP::SHADOW_FRAME_HEIGHT, 32);
  const std::vector<float> frame = build_frame(10.0f);
  const NetDSP::RefreshPlan plan = scheduler.planForFrame(7);
  auto datagrams = build_temporal_datagrams(sender, frame, 500, 123456, plan);

  std::reverse(datagrams.begin(), datagrams.end());
  for (size_t index = 0; index < datagrams.size(); ++index) {
    NetDSP::PacketHeader parsed_header{};
    const std::byte *payload = nullptr;
    size_t payload_bytes = 0;
    const bool parsed = NetDSP::parseDatagram(
        datagrams[index].packet.data(), datagrams[index].packet.size(),
        parsed_header, payload, payload_bytes);
    assert(parsed);

    const auto result = receiver.onPacket(parsed_header, payload, payload_bytes,
                                          500000 + index);
    if (index + 1 == datagrams.size()) {
      assert(result.status == NetDSP::PacketStatus::FrameCompleted);
    } else {
      assert(result.status == NetDSP::PacketStatus::AcceptedFragment);
    }
  }

  const auto ready = receiver.tryAcquireReadyFrame();
  assert(ready.has_value());
  std::println("\n  debug: temporal recv seq={} frame_id={} rows=[{}, {}) "
               "fragments={}/{} bytes={}",
               ready->descriptor.sequence, ready->descriptor.frame_id,
               ready->descriptor.refresh_start_row,
               ready->descriptor.refresh_start_row +
                   ready->descriptor.refresh_row_count,
               ready->descriptor.fragments_received,
               ready->descriptor.total_fragments, ready->descriptor.bytes_used);

  assert(ready->descriptor.frame_id == 500);
  assert(ready->descriptor.refresh_start_row == plan.spans[0].start_row);
  assert(ready->descriptor.refresh_row_count == plan.payloadRowCount());
  assert(ready->reference_pixels != nullptr);

  for (uint32_t y = 0; y < NetDSP::SHADOW_FRAME_HEIGHT; ++y) {
    for (uint32_t x = 0; x < NetDSP::SHADOW_FRAME_WIDTH; ++x) {
      const float pixel =
          ready->reference_pixels[static_cast<size_t>(y) * NetDSP::SHADOW_FRAME_WIDTH +
                                 x];
      if (plan.coversRow(y)) {
        assert(pixel == frame[static_cast<size_t>(y) * NetDSP::SHADOW_FRAME_WIDTH + x]);
      } else {
        assert(pixel == -1.0f);
      }
    }
  }

  std::println("PASSED");
}

void test_temporal_phase_healing_on_persistent_reference() {
  std::print("Test: Temporal Phase Healing On Persistent Reference... ");

  NetDSP::SenderEngine sender;
  NetDSP::TemporalReceiverEngine<4, 8> receiver(20000, -1.0f);
  const NetDSP::DistributedIntraRefreshScheduler scheduler(
      NetDSP::SHADOW_FRAME_HEIGHT, 32);

  const std::vector<float> frame_a = build_frame(100.0f);
  const std::vector<float> frame_b = build_frame(500.0f);
  const std::vector<float> frame_c = build_frame(900.0f);

  const NetDSP::RefreshPlan plan_a = scheduler.planForFrame(0);
  const NetDSP::RefreshPlan plan_b = scheduler.planForFrame(33);
  const NetDSP::RefreshPlan plan_c = scheduler.planForFrame(1);

  for (const auto &datagram :
       build_temporal_datagrams(sender, frame_a, 601, 200000, plan_a)) {
    NetDSP::PacketHeader header{};
    const std::byte *payload = nullptr;
    size_t payload_bytes = 0;
    assert(NetDSP::parseDatagram(datagram.packet.data(), datagram.packet.size(),
                                 header, payload, payload_bytes));
    assert(receiver.onPacket(header, payload, payload_bytes, 600000).accepted());
  }
  assert(receiver.tryAcquireReadyFrame().has_value());

  for (const auto &datagram :
       build_temporal_datagrams(sender, frame_b, 602, 210000, plan_b)) {
    NetDSP::PacketHeader header{};
    const std::byte *payload = nullptr;
    size_t payload_bytes = 0;
    assert(NetDSP::parseDatagram(datagram.packet.data(), datagram.packet.size(),
                                 header, payload, payload_bytes));
    assert(receiver.onPacket(header, payload, payload_bytes, 610000).accepted());
  }
  const auto ready_b = receiver.tryAcquireReadyFrame();
  assert(ready_b.has_value());

  for (uint32_t x = 0; x < NetDSP::SHADOW_FRAME_WIDTH; ++x) {
      assert(ready_b->reference_pixels[x] == frame_b[x]);
    assert(ready_b->reference_pixels[32 * NetDSP::SHADOW_FRAME_WIDTH + x] ==
           -1.0f);
    assert(ready_b->reference_pixels[1056 * NetDSP::SHADOW_FRAME_WIDTH + x] ==
           frame_b[1056 * NetDSP::SHADOW_FRAME_WIDTH + x]);
      assert(ready_b->reference_pixels[16 * NetDSP::SHADOW_FRAME_WIDTH + x] ==
        frame_a[16 * NetDSP::SHADOW_FRAME_WIDTH + x]);
  }

  for (const auto &datagram :
       build_temporal_datagrams(sender, frame_c, 603, 220000, plan_c)) {
    NetDSP::PacketHeader header{};
    const std::byte *payload = nullptr;
    size_t payload_bytes = 0;
    assert(NetDSP::parseDatagram(datagram.packet.data(), datagram.packet.size(),
                                 header, payload, payload_bytes));
    assert(receiver.onPacket(header, payload, payload_bytes, 620000).accepted());
  }
  const auto ready_c = receiver.tryAcquireReadyFrame();
  assert(ready_c.has_value());

  for (uint32_t x = 0; x < NetDSP::SHADOW_FRAME_WIDTH; ++x) {
    assert(ready_c->reference_pixels[32 * NetDSP::SHADOW_FRAME_WIDTH + x] ==
           frame_c[32 * NetDSP::SHADOW_FRAME_WIDTH + x]);
    assert(ready_c->reference_pixels[0 * NetDSP::SHADOW_FRAME_WIDTH + x] ==
        frame_b[0 * NetDSP::SHADOW_FRAME_WIDTH + x]);
    assert(ready_c->reference_pixels[1056 * NetDSP::SHADOW_FRAME_WIDTH + x] ==
           frame_b[1056 * NetDSP::SHADOW_FRAME_WIDTH + x]);
  }

  std::println("PASSED");
}

void test_temporal_timeout_drops_partial_and_keeps_stale_rows() {
  std::print("Test: Temporal Timeout Drops Partial And Keeps Stale Rows... ");

  NetDSP::SenderEngine sender;
  NetDSP::TemporalReceiverEngine<4, 8> receiver(5000, -1.0f);
  const NetDSP::DistributedIntraRefreshScheduler scheduler(
      NetDSP::SHADOW_FRAME_HEIGHT, 32);

  const std::vector<float> baseline = build_frame(200.0f);
  const NetDSP::RefreshPlan baseline_plan = scheduler.planForFrame(0);
  for (const auto &datagram :
       build_temporal_datagrams(sender, baseline, 701, 300000, baseline_plan)) {
    NetDSP::PacketHeader header{};
    const std::byte *payload = nullptr;
    size_t payload_bytes = 0;
    assert(NetDSP::parseDatagram(datagram.packet.data(), datagram.packet.size(),
                                 header, payload, payload_bytes));
    assert(receiver.onPacket(header, payload, payload_bytes, 700000).accepted());
  }
  assert(receiver.tryAcquireReadyFrame().has_value());

  const std::vector<float> lost_frame = build_frame(600.0f);
  const NetDSP::RefreshPlan lost_plan = scheduler.planForFrame(1);
  auto partial_datagrams =
      build_temporal_datagrams(sender, lost_frame, 702, 310000, lost_plan);
  assert(partial_datagrams.size() > 2);

  for (size_t index = 0; index + 1 < partial_datagrams.size(); ++index) {
    NetDSP::PacketHeader header{};
    const std::byte *payload = nullptr;
    size_t payload_bytes = 0;
    assert(NetDSP::parseDatagram(partial_datagrams[index].packet.data(),
                                 partial_datagrams[index].packet.size(), header,
                                 payload, payload_bytes));
    const auto result =
        receiver.onPacket(header, payload, payload_bytes, 710000 + index);
    assert(result.status == NetDSP::PacketStatus::AcceptedFragment);
  }

  const auto sweep = receiver.pollExpiredFrames(710000 + receiver.frameTimeoutUs() + 10);
  std::println("\n  debug: dropped_partial={} rows=[{}, {})", sweep.dropped,
               lost_plan.spans[0].start_row,
               lost_plan.spans[0].start_row + lost_plan.payloadRowCount());
  assert(sweep.dropped == 1);
  assert(!receiver.tryAcquireReadyFrame().has_value());

  const auto &reference = receiver.reference();
  for (uint32_t x = 0; x < NetDSP::SHADOW_FRAME_WIDTH; ++x) {
    assert(reference.at(x, 0) == baseline[x]);
    assert(reference.at(x, 32) == -1.0f);
  }

  NetDSP::PacketHeader late_header{};
  const std::byte *late_payload = nullptr;
  size_t late_payload_bytes = 0;
  const auto &late_datagram = partial_datagrams.back();
  assert(NetDSP::parseDatagram(late_datagram.packet.data(),
                               late_datagram.packet.size(), late_header,
                               late_payload, late_payload_bytes));
  const auto late = receiver.onPacket(
      late_header, late_payload, late_payload_bytes,
      710000 + receiver.frameTimeoutUs() + 20);
  assert(late.status == NetDSP::PacketStatus::LateFragment);

  std::println("PASSED");
}

void test_temporal_phase_healing_after_missed_refresh() {
  std::print("Test: Temporal Phase Healing After Missed Refresh... ");

  NetDSP::SenderEngine sender;
  NetDSP::TemporalReceiverEngine<4, 8> receiver(5000, -1.0f);
  const NetDSP::DistributedIntraRefreshScheduler scheduler(
      NetDSP::SHADOW_FRAME_HEIGHT, 32);

  const std::vector<float> frame_a = build_frame(1000.0f);
  const std::vector<float> frame_c = build_frame(1800.0f);
  const NetDSP::RefreshPlan plan_a = scheduler.planForFrame(0);
    const NetDSP::RefreshPlan plan_heal =
      NetDSP::makeRefreshPlanFromWindow(NetDSP::SHADOW_FRAME_HEIGHT, 32, 32);

  for (const auto &datagram :
       build_temporal_datagrams(sender, frame_a, 801, 400000, plan_a)) {
    NetDSP::PacketHeader header{};
    const std::byte *payload = nullptr;
    size_t payload_bytes = 0;
    assert(NetDSP::parseDatagram(datagram.packet.data(), datagram.packet.size(),
                                 header, payload, payload_bytes));
    assert(receiver.onPacket(header, payload, payload_bytes, 800000).accepted());
  }
  assert(receiver.tryAcquireReadyFrame().has_value());

  const auto &before_heal = receiver.reference();
  for (uint32_t x = 0; x < NetDSP::SHADOW_FRAME_WIDTH; ++x) {
    assert(before_heal.at(x, 0) == frame_a[x]);
    assert(before_heal.at(x, 32) == -1.0f);
  }

  for (const auto &datagram :
       build_temporal_datagrams(sender, frame_c, 802, 410000, plan_heal)) {
    NetDSP::PacketHeader header{};
    const std::byte *payload = nullptr;
    size_t payload_bytes = 0;
    assert(NetDSP::parseDatagram(datagram.packet.data(), datagram.packet.size(),
                                 header, payload, payload_bytes));
    assert(receiver.onPacket(header, payload, payload_bytes, 810000).accepted());
  }
  const auto healed = receiver.tryAcquireReadyFrame();
  assert(healed.has_value());
  std::println("\n  debug: healed rows=[{}, {}) seq={} frame_id={}",
               healed->descriptor.refresh_start_row,
               healed->descriptor.refresh_start_row +
                   healed->descriptor.refresh_row_count,
               healed->descriptor.sequence, healed->descriptor.frame_id);

  for (uint32_t x = 0; x < NetDSP::SHADOW_FRAME_WIDTH; ++x) {
    assert(healed->reference_pixels[0 * NetDSP::SHADOW_FRAME_WIDTH + x] ==
           frame_a[0 * NetDSP::SHADOW_FRAME_WIDTH + x]);
    assert(healed->reference_pixels[32 * NetDSP::SHADOW_FRAME_WIDTH + x] ==
           frame_c[32 * NetDSP::SHADOW_FRAME_WIDTH + x]);
  }

  std::println("PASSED");
}

void test_temporal_rejects_invalid_refresh_metadata() {
  std::print("Test: Temporal Rejects Invalid Refresh Metadata... ");

  NetDSP::TemporalReceiverEngine<4, 8> receiver(20000, -1.0f);

  constexpr uint16_t invalid_start_row = NetDSP::SHADOW_FRAME_HEIGHT;
  constexpr uint16_t refresh_row_count = 1;
  const size_t total_payload_bytes =
      static_cast<size_t>(refresh_row_count) * NetDSP::SHADOW_FRAME_WIDTH *
      sizeof(float);
  const uint16_t total_fragments =
      NetDSP::fragmentsForPayloadBytes(total_payload_bytes);
  std::vector<float> payload_rows(
      static_cast<size_t>(refresh_row_count) * NetDSP::SHADOW_FRAME_WIDTH,
      0.5f);

  const NetDSP::PacketHeader header = NetDSP::SenderEngine::makeTemporalHeader(
      77, 900, 0, total_fragments, 123456,
      NetDSP::FLAG_P_FRAME | NetDSP::FLAG_CS_ENABLED, invalid_start_row,
      refresh_row_count);

  const auto result = receiver.onPacket(
      header, payload_rows.data(),
      std::min(total_payload_bytes, NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES),
      900000);
  assert(result.status == NetDSP::PacketStatus::InvalidPacket);
  assert(!receiver.tryAcquireReadyFrame().has_value());

  std::println("PASSED");
}

} // namespace

int main() {
  std::println("==========================================================");
  std::println("Temporal Receiver Tests");
  std::println("==========================================================\n");

  test_temporal_round_trip_updates_reference_buffer();
  test_temporal_phase_healing_on_persistent_reference();
  test_temporal_timeout_drops_partial_and_keeps_stale_rows();
  test_temporal_phase_healing_after_missed_refresh();
  test_temporal_rejects_invalid_refresh_metadata();

  std::println("\n==========================================================");
  std::println("All temporal receiver tests PASSED!");
  std::println("==========================================================");
  return 0;
}
