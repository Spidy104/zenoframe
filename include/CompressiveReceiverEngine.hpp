#pragma once

#include "CompressiveSampling.hpp"
#include "ReceiverEngine.hpp"
#include "SPSCQueue.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>

namespace NetDSP {

template <size_t PoolSize = 4, size_t ReadyQueueCapacity = 32>
class CompressiveReceiverEngine {
  struct Tombstone {
    bool active{false};
    uint32_t sequence{0};
    uint16_t frame_id{0};
    uint64_t expires_at_us{0};
  };

  struct AssemblySlot {
    bool active{false};
    uint32_t sequence{0};
    uint16_t frame_id{0};
    uint16_t total_fragments{0};
    uint16_t fragments_received{0};
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

  FrameBufferPool<PoolSize> pool_{};
  SPSCQueue<uint16_t, ReadyQueueCapacity> ready_queue_{};
  std::unique_ptr<AssemblySlot[]> slots_;
  std::array<Tombstone, TombstoneCapacity> tombstones_{};
  uint64_t frame_timeout_us_;
  uint64_t tombstone_retention_us_;

public:
  explicit CompressiveReceiverEngine(uint64_t frame_timeout_us = 10000)
      : slots_(std::make_unique<AssemblySlot[]>(PoolSize)),
        frame_timeout_us_(frame_timeout_us),
        tombstone_retention_us_(
            std::max(frame_timeout_us * 4, MinTombstoneRetentionUs)) {}

  [[nodiscard]] PacketResult onPacket(const PacketHeader &header,
                                      const void *payload,
                                      size_t payload_bytes) {
    return onPacket(header, payload, payload_bytes, header.timestamp_us);
  }

  [[nodiscard]] PacketResult onPayload(uint32_t sequence, uint16_t frame_id,
                                       uint64_t timestamp_us,
                                       uint8_t type_flags,
                                       const void *payload,
                                       size_t payload_bytes) {
    if (payload == nullptr || payload_bytes == 0 ||
        payload_bytes > SHADOW_BUFFER_BYTES) {
      return {};
    }

    purgeExpiredTombstones(timestamp_us);
    if (isTombstoned(sequence, frame_id, timestamp_us)) {
      return {.status = PacketStatus::LateFragment, .slot_index = 0};
    }

    rememberFrame(sequence, frame_id, timestamp_us);
    const auto ready_slot = reconstructAndQueuePayload(
        static_cast<const std::byte *>(payload), payload_bytes, sequence,
        frame_id, timestamp_us,
        fragmentsForPayloadBytes(payload_bytes),
        static_cast<uint16_t>(fragmentsForPayloadBytes(payload_bytes)),
        static_cast<uint8_t>(type_flags | FLAG_COMPRESSIVE_SAMPLING));
    if (!ready_slot.has_value()) {
      return {.status = PacketStatus::InvalidPacket, .slot_index = 0};
    }

    return {.status = PacketStatus::FrameCompleted, .slot_index = *ready_slot};
  }

  [[nodiscard]] PacketResult onPacket(const PacketHeader &header,
                                      const void *payload,
                                      size_t payload_bytes,
                                      uint64_t arrival_time_us) {
    (void)pollExpiredFrames(arrival_time_us);
    purgeExpiredTombstones(arrival_time_us);

    if (!isValidHeaderForCompressive(header) || payload == nullptr ||
        payload_bytes == 0 || payload_bytes > MAX_FRAGMENT_PAYLOAD_BYTES) {
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
    if (slot.hasFragment(header.fragment_index)) {
      return {.status = PacketStatus::DuplicateFragment,
              .slot_index = *slot_index};
    }

    const size_t payload_offset =
        static_cast<size_t>(header.fragment_index) * MAX_FRAGMENT_PAYLOAD_BYTES;
    if (payload_offset + payload_bytes > slot.payload.size()) {
      if (newly_leased) {
        slot.reset();
      }
      return {.status = PacketStatus::InvalidPacket, .slot_index = *slot_index};
    }

    std::memcpy(slot.payload.data() + payload_offset, payload, payload_bytes);
    slot.markFragment(header.fragment_index);
    ++slot.fragments_received;
    slot.bytes_received += static_cast<uint32_t>(payload_bytes);

    if (slot.fragments_received != slot.total_fragments) {
      return {.status = PacketStatus::AcceptedFragment,
              .slot_index = *slot_index};
    }

    rememberFrame(header.sequence, header.frame_id, arrival_time_us);
    const auto ready_slot =
        reconstructAndQueue(slot, header.sequence, header.frame_id);
    slot.reset();

    if (!ready_slot.has_value()) {
      return {.status = PacketStatus::InvalidPacket, .slot_index = 0};
    }

    return {.status = PacketStatus::FrameCompleted, .slot_index = *ready_slot};
  }

  [[nodiscard]] std::optional<ReadyFrame> tryAcquireReadyFrame() {
    uint16_t slot_index = 0;
    if (!ready_queue_.pop(slot_index)) {
      return std::nullopt;
    }
    if (!pool_.tryAcquireReadySlot(slot_index)) {
      return std::nullopt;
    }
    auto &slot = pool_.slot(slot_index);
    return ReadyFrame{
        .slot_index = slot_index,
        .descriptor = slot.descriptor,
        .pixels = slot.data(),
        .bytes = slot.bytes(),
    };
  }

  void releaseReadyFrame(uint16_t slot_index) {
    pool_.releaseProcessedSlot(slot_index);
  }

  [[nodiscard]] size_t queuedFrameCount() const { return ready_queue_.size(); }
  [[nodiscard]] uint64_t frameTimeoutUs() const { return frame_timeout_us_; }

  SweepResult pollExpiredFrames(uint64_t now_us) {
    SweepResult result{};
    for (uint16_t index = 0; index < PoolSize; ++index) {
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

private:
  [[nodiscard]] bool isValidHeaderForCompressive(
      const PacketHeader &header) const {
    return isValidHeader(header) &&
           hasFlag(header.type_flags, FLAG_COMPRESSIVE_SAMPLING) &&
           !usesTemporalRefresh(header) &&
           header.quantization == SHADOW_FRAME_QUANTIZATION &&
           header.total_fragments <= SHADOW_TOTAL_FRAGMENTS;
  }

  [[nodiscard]] std::optional<uint16_t> resolveSlot(const PacketHeader &header,
                                                    uint64_t arrival_time_us,
                                                    bool &newly_leased) {
    for (uint16_t index = 0; index < PoolSize; ++index) {
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

    for (uint16_t index = 0; index < PoolSize; ++index) {
      auto &slot = slots_[index];
      if (slot.active) {
        continue;
      }

      slot.reset();
      slot.active = true;
      slot.sequence = header.sequence;
      slot.frame_id = header.frame_id;
      slot.total_fragments = header.total_fragments;
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

  [[nodiscard]] std::optional<uint16_t>
  reconstructAndQueue(const AssemblySlot &slot, uint32_t sequence,
                      uint16_t frame_id) {
    return reconstructAndQueuePayload(
        slot.payload.data(), slot.bytes_received, sequence, frame_id,
        slot.timestamp_us, slot.total_fragments, slot.fragments_received,
        slot.type_flags, slot.quantization);
  }

  [[nodiscard]] std::optional<uint16_t> reconstructAndQueuePayload(
      const std::byte *payload, size_t payload_bytes, uint32_t sequence,
      uint16_t frame_id, uint64_t timestamp_us, uint16_t total_fragments,
      uint16_t fragments_received, uint8_t type_flags,
      uint8_t quantization = SHADOW_FRAME_QUANTIZATION) {
    const auto ready_slot = pool_.tryLeaseFreeSlot();
    if (!ready_slot.has_value()) {
      return std::nullopt;
    }

    auto &pool_slot = pool_.slot(*ready_slot);
    if (!reconstructCompressiveFramePayload(payload, payload_bytes,
                                            SHADOW_FRAME_WIDTH,
                                            SHADOW_FRAME_HEIGHT,
                                            pool_slot.data())) {
      pool_.releaseProcessedSlot(*ready_slot);
      return std::nullopt;
    }

    const FrameDescriptor descriptor{
        .sequence = sequence,
        .bytes_used = static_cast<uint32_t>(payload_bytes),
        .timestamp_us = timestamp_us,
        .slot_index = *ready_slot,
        .frame_id = frame_id,
        .refresh_start_row = 0,
        .refresh_row_count = 0,
        .total_fragments = total_fragments,
        .fragments_received = fragments_received,
        .missing_fragments = 0,
        .quantization = quantization,
        .type_flags = type_flags,
        .descriptor_flags = 0,
    };

    if (!pool_.publishReadyFrame(*ready_slot, descriptor, pool_slot.data())) {
      pool_.releaseProcessedSlot(*ready_slot);
      return std::nullopt;
    }
    if (!ready_queue_.push(*ready_slot)) {
      pool_.releaseProcessedSlot(*ready_slot);
      return std::nullopt;
    }
    return ready_slot;
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
