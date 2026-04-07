#include "Protocol.hpp"
#include "ReceiverEngine.hpp"
#include "SenderEngine.hpp"
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

struct CapturedFragment {
  NetDSP::PacketHeader header{};
  size_t payload_bytes{0};
  size_t payload_offset{0};
  std::vector<std::byte> payload;
};

std::vector<float> build_frame(uint32_t frame_seed) {
  std::vector<float> frame(NetDSP::SHADOW_PIXEL_COUNT, 0.0f);
  auto *bytes = reinterpret_cast<std::byte *>(frame.data());

  for (uint16_t fragment_index = 0;
       fragment_index < NetDSP::SenderEngine::totalFragments();
       ++fragment_index) {
    const size_t offset =
        static_cast<size_t>(fragment_index) * NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES;
    const size_t count =
        NetDSP::SenderEngine::payloadBytesForFragment(fragment_index);
    const std::byte value =
        std::byte{static_cast<uint8_t>((frame_seed + fragment_index) % 251u)};
    std::fill_n(bytes + offset, count, value);
  }

  return frame;
}

std::vector<CapturedFragment> capture_frame(NetDSP::SenderEngine &sender,
                                            const std::vector<float> &frame,
                                            uint16_t frame_id,
                                            uint64_t timestamp_us) {
  std::vector<CapturedFragment> fragments;
  const auto *frame_bytes =
      reinterpret_cast<const std::byte *>(frame.data());

  const bool sent = sender.sendFrame(
      frame.data(), frame_id, timestamp_us,
      [&](const NetDSP::PacketHeader &header, const std::byte *payload,
          size_t payload_bytes) {
        fragments.push_back(CapturedFragment{
            .header = header,
            .payload_bytes = payload_bytes,
            .payload_offset = static_cast<size_t>(payload - frame_bytes),
            .payload = std::vector<std::byte>(payload, payload + payload_bytes),
        });
        return true;
      });

  assert(sent);
  return fragments;
}

std::vector<CapturedFragment>
capture_temporal_frame(NetDSP::SenderEngine &sender,
                       const std::vector<float> &frame, uint16_t frame_id,
                       uint64_t timestamp_us, const NetDSP::RefreshPlan &plan) {
  std::vector<CapturedFragment> fragments;
  const bool sent = sender.sendTemporalRefresh(
      frame.data(), frame_id, timestamp_us, plan,
      [&](const NetDSP::PacketHeader &header, const std::byte *payload,
          size_t payload_bytes) {
        fragments.push_back(CapturedFragment{
            .header = header,
            .payload_bytes = payload_bytes,
            .payload_offset = 0,
            .payload = std::vector<std::byte>(payload, payload + payload_bytes),
        });
        return true;
      });
  assert(sent);
  return fragments;
}

void test_send_frame_fragment_contract() {
  std::print("Test: Sender Fragment Contract... ");

  NetDSP::SenderEngine sender;
  const std::vector<float> frame = build_frame(3);
  const auto fragments = capture_frame(sender, frame, 77, 123456789);

  assert(fragments.size() == NetDSP::SenderEngine::totalFragments());

  const uint32_t expected_sequence = fragments.front().header.sequence;
  size_t covered_bytes = 0;

  for (uint16_t fragment_index = 0; fragment_index < fragments.size();
       ++fragment_index) {
    const auto &fragment = fragments[fragment_index];
    assert(fragment.header.magic == NetDSP::MAGIC_NUMBER);
    assert(fragment.header.sequence == expected_sequence);
    assert(fragment.header.frame_id == 77);
    assert(fragment.header.fragment_index == fragment_index);
    assert(fragment.header.total_fragments == NetDSP::SenderEngine::totalFragments());
    assert(fragment.header.timestamp_us == 123456789);
    assert(fragment.header.quantization == NetDSP::SHADOW_FRAME_QUANTIZATION);
    assert(fragment.payload_bytes ==
           NetDSP::SenderEngine::payloadBytesForFragment(fragment_index));
    assert(fragment.payload_offset ==
           static_cast<size_t>(fragment_index) * NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES);
    covered_bytes += fragment.payload_bytes;
  }

  assert(covered_bytes == NetDSP::SHADOW_BUFFER_BYTES);
  std::println("PASSED");
}

void test_send_frame_stops_on_sink_failure() {
  std::print("Test: Sender Stops On Sink Failure... ");

  NetDSP::SenderEngine sender;
  const std::vector<float> frame = build_frame(11);
  size_t callback_count = 0;

  const bool sent = sender.sendFrame(
      frame.data(), 91, 4444,
      [&](const NetDSP::PacketHeader &, const std::byte *, size_t) {
        ++callback_count;
        return callback_count < 3;
      });

  assert(!sent);
  assert(callback_count == 3);
  std::println("PASSED");
}

void test_sender_receiver_round_trip_multiple_frames() {
  std::print("Test: Sender/Receiver Multi-Frame Round Trip... ");

  NetDSP::SenderEngine sender;
  NetDSP::ReceiverEngine<4, 8> receiver;
  std::vector<std::vector<float>> frames;
  std::vector<uint32_t> sequences;
  std::vector<uint16_t> frame_ids;

  for (uint16_t frame_number = 0; frame_number < 3; ++frame_number) {
    frames.push_back(build_frame(20u + frame_number));
    const uint16_t frame_id = static_cast<uint16_t>(100 + frame_number);
    const uint64_t timestamp_us = 1000000ull + frame_number * 1000ull;
    auto fragments = capture_frame(sender, frames.back(), frame_id, timestamp_us);

    sequences.push_back(fragments.front().header.sequence);
    frame_ids.push_back(frame_id);

    for (auto it = fragments.rbegin(); it != fragments.rend(); ++it) {
      const auto result = receiver.onPacket(it->header, it->payload.data(),
                                            it->payload_bytes, timestamp_us);
      if (it + 1 == fragments.rend()) {
        assert(result.status == NetDSP::PacketStatus::FrameCompleted);
      } else {
        assert(result.status == NetDSP::PacketStatus::AcceptedFragment);
      }
    }
  }

  assert(receiver.queuedFrameCount() == frames.size());

  for (size_t index = 0; index < frames.size(); ++index) {
    const auto ready = receiver.tryAcquireReadyFrame();
    assert(ready.has_value());
    assert(ready->descriptor.sequence == sequences[index]);
    assert(ready->descriptor.frame_id == frame_ids[index]);
    assert(ready->descriptor.isComplete());
    assert(!ready->descriptor.isPartial());
    assert(ready->descriptor.bytes_used == NetDSP::SHADOW_BUFFER_BYTES);
    assert(std::memcmp(ready->bytes, frames[index].data(),
                       NetDSP::SHADOW_BUFFER_BYTES) == 0);
    receiver.releaseReadyFrame(ready->slot_index);
  }

  assert(receiver.queuedFrameCount() == 0);
  std::println("PASSED");
}

void test_sender_payload_serialization_round_trip() {
  std::print("Test: Sender Serialization Round Trip... ");

  NetDSP::SenderEngine sender;
  const std::vector<float> frame = build_frame(33);
  const auto fragments = capture_frame(sender, frame, 55, 8888);
  std::array<std::byte, NetDSP::DEFAULT_PACKET_BYTES> packet{};

  for (const auto &fragment : fragments) {
    const size_t packet_bytes = NetDSP::serializeDatagram(
        fragment.header, fragment.payload.data(), fragment.payload_bytes,
        packet.data(), packet.size());
    assert(packet_bytes == NetDSP::HEADER_SIZE + fragment.payload_bytes);

    NetDSP::PacketHeader parsed_header{};
    const std::byte *parsed_payload = nullptr;
    size_t parsed_payload_bytes = 0;
    const bool parsed = NetDSP::parseDatagram(packet.data(), packet_bytes,
                                              parsed_header, parsed_payload,
                                              parsed_payload_bytes);
    assert(parsed);
    assert(parsed_header.sequence == fragment.header.sequence);
    assert(parsed_header.frame_id == fragment.header.frame_id);
    assert(parsed_header.fragment_index == fragment.header.fragment_index);
    assert(parsed_payload_bytes == fragment.payload_bytes);
    assert(std::memcmp(parsed_payload, fragment.payload.data(),
                       fragment.payload_bytes) == 0);
  }

  std::println("PASSED");
}

void test_temporal_refresh_fragment_contract() {
  std::print("Test: Temporal Refresh Fragment Contract... ");

  NetDSP::SenderEngine sender;
  const std::vector<float> frame = build_frame(99);
  const NetDSP::DistributedIntraRefreshScheduler scheduler(
      NetDSP::SHADOW_FRAME_HEIGHT, 32);
  const NetDSP::RefreshPlan plan = scheduler.planForFrame(7);
  const auto fragments =
      capture_temporal_frame(sender, frame, 321, 777000, plan);

  const std::vector<float> expected_payload = NetDSP::extractRefreshPayload(
      plan, frame.data(), NetDSP::SHADOW_FRAME_WIDTH);
  const auto *expected_bytes =
      reinterpret_cast<const std::byte *>(expected_payload.data());
  const size_t expected_payload_bytes = expected_payload.size() * sizeof(float);
  const uint16_t expected_total_fragments =
      NetDSP::fragmentsForPayloadBytes(expected_payload_bytes);

  assert(!fragments.empty());
  assert(fragments.size() == expected_total_fragments);

  std::println("\n  debug: temporal seq={} frame_id={} start_row={} rows={} "
               "payload_bytes={} fragments={}",
               fragments.front().header.sequence, fragments.front().header.frame_id,
               fragments.front().header.refresh_start_row,
               fragments.front().header.refresh_row_count, expected_payload_bytes,
               expected_total_fragments);

  std::vector<std::byte> reconstructed;
  reconstructed.reserve(expected_payload_bytes);

  for (uint16_t index = 0; index < fragments.size(); ++index) {
    const auto &fragment = fragments[index];
    assert(NetDSP::hasFlag(fragment.header.type_flags,
                           NetDSP::FLAG_TEMPORAL_REFRESH));
    assert(fragment.header.frame_id == 321);
    assert(fragment.header.timestamp_us == 777000);
    assert(fragment.header.fragment_index == index);
    assert(fragment.header.total_fragments == expected_total_fragments);
    assert(fragment.header.refresh_start_row == plan.spans[0].start_row);
    assert(fragment.header.refresh_row_count == plan.payloadRowCount());

    reconstructed.insert(reconstructed.end(), fragment.payload.begin(),
                         fragment.payload.end());
  }

  assert(reconstructed.size() == expected_payload_bytes);
  assert(std::memcmp(reconstructed.data(), expected_bytes, expected_payload_bytes) ==
         0);

  std::println("PASSED");
}

void test_temporal_refresh_stops_on_sink_failure() {
  std::print("Test: Temporal Refresh Stops On Sink Failure... ");

  NetDSP::SenderEngine sender;
  const std::vector<float> frame = build_frame(7);
  const NetDSP::DistributedIntraRefreshScheduler scheduler(
      NetDSP::SHADOW_FRAME_HEIGHT, 24);
  const NetDSP::RefreshPlan plan = scheduler.planForFrame(3);

  size_t callback_count = 0;
  const bool sent = sender.sendTemporalRefresh(
      frame.data(), 77, 9000, plan,
      [&](const NetDSP::PacketHeader &, const std::byte *, size_t) {
        ++callback_count;
        return callback_count < 2;
      });

  assert(!sent);
  assert(callback_count == 2);
  std::println("PASSED");
}

void test_phase1_sender_api_selects_full_or_temporal_path() {
  std::print("Test: Phase 1 Sender API Selects Full Or Temporal Path... ");

  NetDSP::SenderEngine sender;
  const std::vector<float> frame = build_frame(17);
  const NetDSP::DistributedIntraRefreshScheduler scheduler(
      NetDSP::SHADOW_FRAME_HEIGHT, 32);

  size_t full_packets = 0;
  const bool sent_full = sender.sendDistributedIntraRefreshFrame(
      frame.data(), 150, 1234, 0, scheduler,
      [&](const NetDSP::PacketHeader &header, const std::byte *, size_t) {
        ++full_packets;
        assert(!NetDSP::hasFlag(header.type_flags, NetDSP::FLAG_TEMPORAL_REFRESH));
        assert(header.refresh_start_row == 0);
        assert(header.refresh_row_count == 0);
        return true;
      },
      true);
  assert(sent_full);
  assert(full_packets == NetDSP::SenderEngine::totalFragments());

  const NetDSP::RefreshPlan plan = scheduler.planForFrame(2);
  size_t temporal_packets = 0;
  const bool sent_temporal = sender.sendDistributedIntraRefreshFrame(
      frame.data(), 151, 5678, 2, scheduler,
      [&](const NetDSP::PacketHeader &header, const std::byte *, size_t) {
        ++temporal_packets;
        assert(NetDSP::hasFlag(header.type_flags, NetDSP::FLAG_TEMPORAL_REFRESH));
        assert(header.refresh_start_row == plan.spans[0].start_row);
        assert(header.refresh_row_count == plan.payloadRowCount());
        return true;
      },
      false);
  assert(sent_temporal);
  assert(temporal_packets ==
         NetDSP::fragmentsForPayloadBytes(
             static_cast<size_t>(plan.payloadRowCount()) *
             NetDSP::SHADOW_FRAME_WIDTH * sizeof(float)));

  std::println("PASSED");
}

} // namespace

int main() {
  std::println("==========================================================");
  std::println("Sender Engine Tests");
  std::println("==========================================================\n");

  test_send_frame_fragment_contract();
  test_send_frame_stops_on_sink_failure();
  test_sender_receiver_round_trip_multiple_frames();
  test_sender_payload_serialization_round_trip();
  test_temporal_refresh_fragment_contract();
  test_temporal_refresh_stops_on_sink_failure();
  test_phase1_sender_api_selects_full_or_temporal_path();

  std::println("\n==========================================================");
  std::println("All sender tests PASSED!");
  std::println("==========================================================");
  return 0;
}
