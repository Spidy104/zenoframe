#pragma once

#include "FrameBufferPool.hpp"
#include "Protocol.hpp"
#include "ReceiverEngine.hpp"
#include "SPSCQueue.hpp"
#include "TemporalRefresh.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>

namespace NetDSP {

struct TemporalFrameDescriptor {
  uint32_t sequence{0};
  uint32_t bytes_used{0};
  uint64_t timestamp_us{0};
  uint16_t frame_id{0};
  uint16_t refresh_start_row{0};
  uint16_t refresh_row_count{0};
  uint16_t total_fragments{0};
  uint16_t fragments_received{0};
  uint8_t quantization{0};
  uint8_t type_flags{0};
};

static_assert(sizeof(TemporalFrameDescriptor) == 32,
              "TemporalFrameDescriptor layout changed");

struct TemporalReadyFrame {
  TemporalFrameDescriptor descriptor{};
  const float *reference_pixels{nullptr};
};

struct TemporalSweepResult {
  size_t dropped{0};
};

template <size_t SlotCount = 4, size_t ReadyQueueCapacity = 32>
class TemporalReceiverEngine {
  struct Tombstone {
    bool active{false};
    uint32_t sequence{0};
    uint16_t frame_id{0};
    uint64_t expires_at_us{0};
  };

  struct TemporalAssemblySlot {
    bool active{false};
    uint32_t sequence{0};
    uint16_t frame_id{0};
    uint16_t total_fragments{0};
    uint16_t fragments_received{0};
    uint16_t refresh_start_row{0};
    uint16_t refresh_row_count{0};
    uint64_t timestamp_us{0};
    uint64_t deadline_us{0};
    uint32_t bytes_received{0};
    uint8_t quantization{0};
    uint8_t type_flags{0};
    std::array<uint64_t, SHADOW_FRAGMENT_BITMAP_WORDS> fragment_bitmap{};
    std::array<std::byte, SHADOW_BUFFER_BYTES> payload{};

    void reset() { *this = {}; }

    [[nodiscard]] bool hasFragment(uint16_t fragment_index) const {
      const size_t word_index = fragment_index / 64u;
      const uint64_t bit = 1ULL << (fragment_index % 64u);
      return (fragment_bitmap[word_index] & bit) != 0;
    }

    void markFragment(uint16_t fragment_index) {
      const size_t word_index = fragment_index / 64u;
      const uint64_t bit = 1ULL << (fragment_index % 64u);
      fragment_bitmap[word_index] |= bit;
    }
  };

  static constexpr size_t TombstoneCapacity = 256;
  static constexpr uint64_t MinTombstoneRetentionUs = 1000000;

  std::unique_ptr<TemporalAssemblySlot[]> slots_;
  std::array<Tombstone, TombstoneCapacity> tombstones_{};
  SPSCQueue<TemporalFrameDescriptor, ReadyQueueCapacity> ready_queue_{};
  TemporalRefreshReconstructor reference_;
  uint64_t frame_timeout_us_;
  uint64_t tombstone_retention_us_;

  [[nodiscard]] static size_t payloadBytesForVariableFragment(
      size_t total_payload_bytes, uint16_t fragment_index) {
    const size_t offset =
        static_cast<size_t>(fragment_index) * MAX_FRAGMENT_PAYLOAD_BYTES;
    if (offset >= total_payload_bytes) {
      return 0;
    }
    return std::min(MAX_FRAGMENT_PAYLOAD_BYTES, total_payload_bytes - offset);
  }

public:
  explicit TemporalReceiverEngine(uint64_t frame_timeout_us = 10000,
                                  float initial_reference_value = 0.0f)
    : slots_(std::make_unique<TemporalAssemblySlot[]>(SlotCount)),
      reference_(SHADOW_FRAME_WIDTH, SHADOW_FRAME_HEIGHT,
                   initial_reference_value),
        frame_timeout_us_(frame_timeout_us),
        tombstone_retention_us_(std::max(frame_timeout_us * 4,
                                         MinTombstoneRetentionUs)) {}

  [[nodiscard]] PacketResult onPacket(const PacketHeader &header,
                                      const void *payload,
                                      size_t payload_bytes) {
    return onPacket(header, payload, payload_bytes, header.timestamp_us);
  }

  [[nodiscard]] PacketResult onPacket(const PacketHeader &header,
                                      const void *payload,
                                      size_t payload_bytes,
                                      uint64_t arrival_time_us) {
    (void)pollExpiredFrames(arrival_time_us);
    purgeExpiredTombstones(arrival_time_us);

    if (!isValidTemporalHeader(header) || payload == nullptr) {
      return {};
    }

    bool newly_leased = false;
    const auto slot_index = resolveSlot(header, arrival_time_us, newly_leased);
    if (!slot_index.has_value()) {
      if (isTombstoned(header.sequence, header.frame_id, arrival_time_us)) {
        return {.status = PacketStatus::LateFragment, .slot_index = 0};
      }
      return {.status = PacketStatus::NoFreeSlot, .slot_index = 0};
    }

    auto &slot = slots_[*slot_index];
    const size_t total_payload_bytes = expectedPayloadBytes(header);
    const size_t expected_fragment_bytes =
        payloadBytesForVariableFragment(total_payload_bytes,
                                       header.fragment_index);
    if (payload_bytes != expected_fragment_bytes) {
      if (newly_leased) {
        slot.reset();
      }
      return {.status = PacketStatus::InvalidPacket, .slot_index = *slot_index};
    }

    if (slot.hasFragment(header.fragment_index)) {
      return {.status = PacketStatus::DuplicateFragment,
              .slot_index = *slot_index};
    }

    const size_t payload_offset =
        static_cast<size_t>(header.fragment_index) * MAX_FRAGMENT_PAYLOAD_BYTES;
    std::memcpy(slot.payload.data() + payload_offset, payload, payload_bytes);
    slot.markFragment(header.fragment_index);
    ++slot.fragments_received;
    slot.bytes_received += static_cast<uint32_t>(payload_bytes);

    if (slot.fragments_received != slot.total_fragments) {
      return {.status = PacketStatus::AcceptedFragment,
              .slot_index = *slot_index};
    }

    const RefreshPlan plan = makeRefreshPlanFromWindow(
        SHADOW_FRAME_HEIGHT, header.refresh_start_row, header.refresh_row_count);
    const RefreshPayloadLayout layout =
        makeRefreshPayloadLayout(plan, SHADOW_FRAME_WIDTH, header.quantization);
    reference_.applyRefreshPayload(
        layout, reinterpret_cast<const float *>(slot.payload.data()));

    rememberFrame(header.sequence, header.frame_id, arrival_time_us);
    const TemporalFrameDescriptor descriptor{
        .sequence = header.sequence,
        .bytes_used = slot.bytes_received,
        .timestamp_us = header.timestamp_us,
        .frame_id = header.frame_id,
        .refresh_start_row = header.refresh_start_row,
        .refresh_row_count = header.refresh_row_count,
        .total_fragments = header.total_fragments,
        .fragments_received = slot.fragments_received,
        .quantization = header.quantization,
        .type_flags = header.type_flags,
    };

    slot.reset();
    if (!ready_queue_.push(descriptor)) {
      return {.status = PacketStatus::ReadyQueueFull, .slot_index = *slot_index};
    }
    return {.status = PacketStatus::FrameCompleted, .slot_index = *slot_index};
  }

  [[nodiscard]] std::optional<TemporalReadyFrame> tryAcquireReadyFrame() {
    TemporalFrameDescriptor descriptor{};
    if (!ready_queue_.pop(descriptor)) {
      return std::nullopt;
    }
    return TemporalReadyFrame{
        .descriptor = descriptor,
        .reference_pixels = reference_.data(),
    };
  }

  [[nodiscard]] TemporalSweepResult pollExpiredFrames(uint64_t now_us) {
    TemporalSweepResult result{};
    for (size_t index = 0; index < SlotCount; ++index) {
      auto &slot = slots_[index];
      if (!slot.active || slot.deadline_us == 0 || now_us <= slot.deadline_us) {
        continue;
      }
      rememberFrame(slot.sequence, slot.frame_id, now_us);
      slot.reset();
      ++result.dropped;
    }
    return result;
  }

  [[nodiscard]] const TemporalRefreshReconstructor &reference() const {
    return reference_;
  }

  [[nodiscard]] uint64_t frameTimeoutUs() const { return frame_timeout_us_; }

private:
  [[nodiscard]] bool isValidTemporalHeader(const PacketHeader &header) const {
    if (!isValidHeader(header) || !usesTemporalRefresh(header) ||
        !hasFlag(header.type_flags, FLAG_TEMPORAL_REFRESH) ||
        header.quantization != SHADOW_FRAME_QUANTIZATION ||
        header.refresh_start_row >= SHADOW_FRAME_HEIGHT ||
        header.refresh_row_count == 0 ||
        header.refresh_row_count > SHADOW_FRAME_HEIGHT) {
      return false;
    }

    // Temporal refresh windows are allowed to wrap once from the bottom of the
    // frame back to row 0. The range checks above already guarantee the window
    // stays within at most one full frame of rows.

    return header.total_fragments ==
           fragmentsForPayloadBytes(expectedPayloadBytes(header));
  }

  [[nodiscard]] size_t expectedPayloadBytes(const PacketHeader &header) const {
    return static_cast<size_t>(header.refresh_row_count) * SHADOW_FRAME_WIDTH *
           sizeof(float);
  }

  [[nodiscard]] std::optional<uint16_t> resolveSlot(const PacketHeader &header,
                                                    uint64_t arrival_time_us,
                                                    bool &newly_leased) {
    for (uint16_t index = 0; index < SlotCount; ++index) {
      const auto &slot = slots_[index];
      if (slot.active && slot.sequence == header.sequence &&
          slot.frame_id == header.frame_id) {
        newly_leased = false;
        return index;
      }
    }

    if (isTombstoned(header.sequence, header.frame_id, arrival_time_us)) {
      newly_leased = false;
      return std::nullopt;
    }

    for (uint16_t index = 0; index < SlotCount; ++index) {
      auto &slot = slots_[index];
      if (slot.active) {
        continue;
      }
      slot.reset();
      slot.active = true;
      slot.sequence = header.sequence;
      slot.frame_id = header.frame_id;
      slot.total_fragments = header.total_fragments;
      slot.refresh_start_row = header.refresh_start_row;
      slot.refresh_row_count = header.refresh_row_count;
      slot.timestamp_us = header.timestamp_us;
      slot.deadline_us = arrival_time_us + frame_timeout_us_;
      slot.quantization = header.quantization;
      slot.type_flags = header.type_flags;
      newly_leased = true;
      return index;
    }

    newly_leased = false;
    return std::nullopt;
  }

  void purgeExpiredTombstones(uint64_t now_us) {
    for (auto &tombstone : tombstones_) {
      if (tombstone.active && now_us > tombstone.expires_at_us) {
        tombstone = {};
      }
    }
  }

  [[nodiscard]] bool isTombstoned(uint32_t sequence, uint16_t frame_id,
                                  uint64_t now_us) const {
    for (const auto &tombstone : tombstones_) {
      if (!tombstone.active || now_us > tombstone.expires_at_us) {
        continue;
      }
      if (tombstone.sequence == sequence && tombstone.frame_id == frame_id) {
        return true;
      }
    }
    return false;
  }

  void rememberFrame(uint32_t sequence, uint16_t frame_id, uint64_t now_us) {
    Tombstone *candidate = &tombstones_[0];
    for (auto &tombstone : tombstones_) {
      if (!tombstone.active || now_us > tombstone.expires_at_us) {
        tombstone = {
            .active = true,
            .sequence = sequence,
            .frame_id = frame_id,
            .expires_at_us = now_us + tombstone_retention_us_,
        };
        return;
      }
      if (tombstone.sequence == sequence && tombstone.frame_id == frame_id) {
        tombstone.expires_at_us = now_us + tombstone_retention_us_;
        return;
      }
      if (tombstone.expires_at_us < candidate->expires_at_us) {
        candidate = &tombstone;
      }
    }

    *candidate = {
        .active = true,
        .sequence = sequence,
        .frame_id = frame_id,
        .expires_at_us = now_us + tombstone_retention_us_,
    };
  }
};

} // namespace NetDSP
