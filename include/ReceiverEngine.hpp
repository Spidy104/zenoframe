#pragma once

#include "FrameBufferPool.hpp"
#include "PhaseConcealment.hpp"
#include "SPSCQueue.hpp"
#include "TemporalRefresh.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

namespace NetDSP {

enum class TimeoutPolicy : uint8_t {
  ForceCommitPartial = 0,
  DropPartial = 1,
};

enum class PacketStatus : uint8_t {
  InvalidPacket = 0,
  NoFreeSlot = 1,
  AcceptedFragment = 2,
  DuplicateFragment = 3,
  FrameCompleted = 4,
  ReadyQueueFull = 5,
  LateFragment = 6,
};

struct PacketResult {
  PacketStatus status{PacketStatus::InvalidPacket};
  uint16_t slot_index{0};

  [[nodiscard]] bool accepted() const {
    return status == PacketStatus::AcceptedFragment ||
           status == PacketStatus::FrameCompleted;
  }
};

struct ReadyFrame {
  uint16_t slot_index{0};
  FrameDescriptor descriptor{};
  float *pixels{nullptr};
  std::byte *bytes{nullptr};
};

struct SweepResult {
  size_t committed{0};
  size_t dropped{0};
  size_t queue_full{0};
};

template <size_t PoolSize = DEFAULT_SHADOW_BUFFER_COUNT,
          size_t ReadyQueueCapacity = 32>
class ReceiverEngine {
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

  FrameBufferPool<PoolSize> pool_{};
  SPSCQueue<uint16_t, ReadyQueueCapacity> ready_queue_{};
  std::array<bool, PoolSize> inflight_active_{};
  std::array<uint32_t, PoolSize> inflight_sequence_{};
  std::array<uint16_t, PoolSize> inflight_frame_id_{};
  std::unique_ptr<TemporalAssemblySlot[]> temporal_slots_;
  std::array<Tombstone, TombstoneCapacity> tombstones_{};
  TemporalRefreshReconstructor reference_;
  ConcealmentWorkspace concealment_workspace_{
      SHADOW_FRAME_WIDTH, SHADOW_FRAME_HEIGHT};
  uint64_t frame_timeout_us_;
  uint64_t tombstone_retention_us_;
  TimeoutPolicy timeout_policy_;

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
  explicit ReceiverEngine(
      uint64_t frame_timeout_us = 10000,
      TimeoutPolicy timeout_policy = TimeoutPolicy::ForceCommitPartial,
      float initial_reference_value = 0.0f)
      : temporal_slots_(std::make_unique<TemporalAssemblySlot[]>(PoolSize)),
        reference_(SHADOW_FRAME_WIDTH, SHADOW_FRAME_HEIGHT,
                   initial_reference_value),
        frame_timeout_us_(frame_timeout_us),
        tombstone_retention_us_(
            std::max(frame_timeout_us * 4, MinTombstoneRetentionUs)),
        timeout_policy_(timeout_policy) {}

  [[nodiscard]] PacketResult onPacket(const PacketHeader &header,
                                      const void *payload,
                                      size_t payload_bytes) {
    return onPacket(header, payload, payload_bytes, header.timestamp_us);
  }

  [[nodiscard]] PacketResult onPacket(const PacketHeader &header,
                                      const void *payload,
                                      size_t payload_bytes,
                                      uint64_t arrival_time_us) {
    if (hasFlag(header.type_flags, FLAG_TEMPORAL_REFRESH) ||
        usesTemporalRefresh(header)) {
      return onTemporalPacket(header, payload, payload_bytes, arrival_time_us);
    }

    return onFullFramePacket(header, payload, payload_bytes, arrival_time_us);
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
  [[nodiscard]] TimeoutPolicy timeoutPolicy() const { return timeout_policy_; }

  SweepResult pollExpiredFrames(uint64_t now_us) {
    SweepResult result = pollExpiredFullFrames(now_us);

    for (uint16_t index = 0; index < PoolSize; ++index) {
      auto &slot = temporal_slots_[index];
      if (!slot.active || slot.deadline_us == 0 || now_us <= slot.deadline_us) {
        continue;
      }

      rememberFrame(slot.sequence, slot.frame_id, now_us);
      if (slot.fragments_received == 0 ||
          timeout_policy_ == TimeoutPolicy::DropPartial) {
        slot.reset();
        ++result.dropped;
        continue;
      }

      const std::vector<ConcealmentSpan> missing_spans =
          applyAvailableTemporalRows(slot);
      const ConcealmentReport report = concealMissingRowsWithAnalyticContinuation(
          reference_.data(), SHADOW_FRAME_WIDTH, SHADOW_FRAME_HEIGHT,
          missing_spans, concealment_workspace_, false);
      const auto ready_slot =
          enqueueReconstructedReference(slot, true, true, report.healed_rows);
      slot.reset();

      if (!ready_slot.has_value()) {
        ++result.queue_full;
        continue;
      }

      ++result.committed;
    }

    return result;
  }

  [[nodiscard]] const FrameBufferPool<PoolSize> &pool() const { return pool_; }
  [[nodiscard]] FrameBufferPool<PoolSize> &pool() { return pool_; }
  [[nodiscard]] const TemporalRefreshReconstructor &reference() const {
    return reference_;
  }
  [[nodiscard]] TemporalRefreshReconstructor &reference() { return reference_; }

private:
  [[nodiscard]] PacketResult onFullFramePacket(const PacketHeader &header,
                                               const void *payload,
                                               size_t payload_bytes,
                                               uint64_t arrival_time_us) {
    (void)pollExpiredFrames(arrival_time_us);
    purgeExpiredTombstones(arrival_time_us);

    if (!isValidHeader(header) ||
        header.quantization != FrameBufferPool<PoolSize>::quantization() ||
        header.total_fragments != FrameBufferPool<PoolSize>::totalFragments()) {
      return {};
    }

    bool newly_leased = false;
    const auto slot =
        resolveFullFrameSlot(header, arrival_time_us, newly_leased);
    if (!slot.has_value()) {
      if (isTombstoned(header.sequence, header.frame_id, arrival_time_us)) {
        return {.status = PacketStatus::LateFragment, .slot_index = 0};
      }
      return {.status = PacketStatus::NoFreeSlot, .slot_index = 0};
    }
    const uint16_t slot_index = *slot;

    const FragmentWriteResult write =
        pool_.writeFragment(slot_index, header, payload, payload_bytes,
                            arrival_time_us, frame_timeout_us_);

    if (write.duplicate) {
      return {.status = PacketStatus::DuplicateFragment,
              .slot_index = slot_index};
    }

    if (!write.accepted) {
      if (newly_leased) {
        clearInflight(slot_index);
        pool_.releaseProcessedSlot(slot_index);
      }
      return {.status = PacketStatus::InvalidPacket, .slot_index = slot_index};
    }

    if (write.frame_complete) {
      syncReferenceFromPoolSlot(slot_index);
      clearInflight(slot_index);
      rememberFrame(header.sequence, header.frame_id, arrival_time_us);
      if (!ready_queue_.push(slot_index)) {
        if (pool_.tryAcquireReadySlot(slot_index)) {
          pool_.releaseProcessedSlot(slot_index);
        }
        return {.status = PacketStatus::ReadyQueueFull,
                .slot_index = slot_index};
      }
      return {.status = PacketStatus::FrameCompleted,
              .slot_index = slot_index};
    }

    return {.status = PacketStatus::AcceptedFragment, .slot_index = slot_index};
  }

  [[nodiscard]] SweepResult pollExpiredFullFrames(uint64_t now_us) {
    SweepResult result{};
    for (uint16_t index = 0; index < PoolSize; ++index) {
      if (!inflight_active_[index] || pool_.state(index) != SlotState::Filling) {
        continue;
      }

      const FrameAssemblyState assembly = pool_.assemblyState(index);
      if (!assembly.active() || assembly.deadline_us == 0 ||
          now_us <= assembly.deadline_us) {
        continue;
      }

      rememberFrame(assembly.sequence, assembly.frame_id, now_us);
      clearInflight(index);

      if (!pool_.hasAssemblyData(index)) {
        pool_.releaseProcessedSlot(index);
        ++result.dropped;
        continue;
      }

      if (timeout_policy_ == TimeoutPolicy::DropPartial) {
        pool_.releaseProcessedSlot(index);
        ++result.dropped;
        continue;
      }

      if (!pool_.forceCommitPartialSlot(index, true)) {
        pool_.releaseProcessedSlot(index);
        ++result.dropped;
        continue;
      }

      if (!ready_queue_.push(index)) {
        if (pool_.tryAcquireReadySlot(index)) {
          pool_.releaseProcessedSlot(index);
        }
        ++result.queue_full;
        continue;
      }

      ++result.committed;
    }

    return result;
  }

  [[nodiscard]] PacketResult onTemporalPacket(const PacketHeader &header,
                                              const void *payload,
                                              size_t payload_bytes,
                                              uint64_t arrival_time_us) {
    (void)pollExpiredFrames(arrival_time_us);
    purgeExpiredTombstones(arrival_time_us);

    if (!isValidTemporalHeader(header) || payload == nullptr) {
      return {};
    }

    bool newly_leased = false;
    const auto slot_index =
        resolveTemporalSlot(header, arrival_time_us, newly_leased);
    if (!slot_index.has_value()) {
      if (isTombstoned(header.sequence, header.frame_id, arrival_time_us)) {
        return {.status = PacketStatus::LateFragment, .slot_index = 0};
      }
      return {.status = PacketStatus::NoFreeSlot, .slot_index = 0};
    }

    auto &slot = temporal_slots_[*slot_index];
    const size_t total_payload_bytes = expectedTemporalPayloadBytes(header);
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

    rememberFrame(header.sequence, header.frame_id, arrival_time_us);
    applyTemporalRefresh(slot);
    const auto ready_slot = enqueueReconstructedReference(slot, false, false, 0);
    slot.reset();

    if (!ready_slot.has_value()) {
      return {.status = PacketStatus::NoFreeSlot, .slot_index = 0};
    }

    return {.status = PacketStatus::FrameCompleted,
            .slot_index = *ready_slot};
  }

  [[nodiscard]] std::optional<uint16_t> resolveFullFrameSlot(
      const PacketHeader &header, uint64_t now_us, bool &newly_leased) {
    if (const auto existing = findInflightSlot(header.sequence, header.frame_id)) {
      newly_leased = false;
      return existing;
    }

    if (isTombstoned(header.sequence, header.frame_id, now_us)) {
      newly_leased = false;
      return std::nullopt;
    }

    const auto leased = pool_.tryLeaseFreeSlot();
    if (!leased.has_value()) {
      newly_leased = false;
      return std::nullopt;
    }

    registerInflight(*leased, header);
    newly_leased = true;
    return leased;
  }

  [[nodiscard]] std::optional<uint16_t> resolveTemporalSlot(
      const PacketHeader &header, uint64_t arrival_time_us, bool &newly_leased) {
    for (uint16_t index = 0; index < PoolSize; ++index) {
      const auto &slot = temporal_slots_[index];
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
      auto &slot = temporal_slots_[index];
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

  [[nodiscard]] std::optional<uint16_t> findInflightSlot(uint32_t sequence,
                                                         uint16_t frame_id) const {
    for (uint16_t index = 0; index < PoolSize; ++index) {
      if (inflight_active_[index] && inflight_sequence_[index] == sequence &&
          inflight_frame_id_[index] == frame_id) {
        return index;
      }
    }

    return std::nullopt;
  }

  void registerInflight(uint16_t slot_index, const PacketHeader &header) {
    inflight_active_[slot_index] = true;
    inflight_sequence_[slot_index] = header.sequence;
    inflight_frame_id_[slot_index] = header.frame_id;
  }

  void clearInflight(uint16_t slot_index) {
    inflight_active_[slot_index] = false;
    inflight_sequence_[slot_index] = 0;
    inflight_frame_id_[slot_index] = 0;
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

  [[nodiscard]] bool isValidTemporalHeader(const PacketHeader &header) const {
    if (!isValidHeader(header) || !usesTemporalRefresh(header) ||
        !hasFlag(header.type_flags, FLAG_TEMPORAL_REFRESH) ||
        header.quantization != SHADOW_FRAME_QUANTIZATION ||
        header.refresh_start_row >= SHADOW_FRAME_HEIGHT ||
        header.refresh_row_count == 0 ||
        header.refresh_row_count > SHADOW_FRAME_HEIGHT) {
      return false;
    }

    return header.total_fragments ==
           fragmentsForPayloadBytes(expectedTemporalPayloadBytes(header));
  }

  [[nodiscard]] size_t expectedTemporalPayloadBytes(
      const PacketHeader &header) const {
    return static_cast<size_t>(header.refresh_row_count) * SHADOW_FRAME_WIDTH *
           sizeof(float);
  }

  void syncReferenceFromPoolSlot(uint16_t slot_index) {
    std::memcpy(reference_.data(), pool_.slot(slot_index).data(),
                SHADOW_BUFFER_BYTES);
  }

  void applyTemporalRefresh(const TemporalAssemblySlot &slot) {
    const RefreshPlan plan = makeRefreshPlanFromWindow(
        SHADOW_FRAME_HEIGHT, slot.refresh_start_row, slot.refresh_row_count);
    const RefreshPayloadLayout layout = makeRefreshPayloadLayout(
        plan, SHADOW_FRAME_WIDTH, SHADOW_FRAME_QUANTIZATION);
    reference_.applyRefreshPayload(
        layout, reinterpret_cast<const float *>(slot.payload.data()));
  }

  [[nodiscard]] bool payloadRowAvailable(const TemporalAssemblySlot &slot,
                                         size_t payload_row_index) const {
    const size_t row_bytes = static_cast<size_t>(SHADOW_FRAME_WIDTH) * sizeof(float);
    const size_t total_payload_bytes = static_cast<size_t>(slot.refresh_row_count) *
                                       static_cast<size_t>(SHADOW_FRAME_WIDTH) *
                                       sizeof(float);
    const size_t row_begin = payload_row_index * row_bytes;
    const size_t row_end = std::min(total_payload_bytes, row_begin + row_bytes);
    if (row_begin >= row_end) {
      return false;
    }

    const uint16_t first_fragment =
        static_cast<uint16_t>(row_begin / MAX_FRAGMENT_PAYLOAD_BYTES);
    const uint16_t last_fragment = static_cast<uint16_t>(
        (row_end - 1) / MAX_FRAGMENT_PAYLOAD_BYTES);
    for (uint16_t fragment = first_fragment; fragment <= last_fragment;
         ++fragment) {
      if (!slot.hasFragment(fragment)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::vector<ConcealmentSpan>
  applyAvailableTemporalRows(const TemporalAssemblySlot &slot) {
    const RefreshPlan plan = makeRefreshPlanFromWindow(
        SHADOW_FRAME_HEIGHT, slot.refresh_start_row, slot.refresh_row_count);
    const size_t row_bytes = static_cast<size_t>(SHADOW_FRAME_WIDTH) * sizeof(float);
    std::vector<ConcealmentSpan> missing_spans;
    size_t payload_row_index = 0;

    for (uint8_t span_index = 0; span_index < plan.span_count; ++span_index) {
      const RefreshSpan span = plan.spans[span_index];
      ConcealmentSpan pending_missing{};
      bool tracking_missing = false;

      for (uint32_t row = 0; row < span.row_count; ++row, ++payload_row_index) {
        const uint32_t frame_row = span.start_row + row;
        const size_t payload_offset = payload_row_index * row_bytes;
        const size_t frame_offset =
            static_cast<size_t>(frame_row) * SHADOW_FRAME_WIDTH;

        if (payloadRowAvailable(slot, payload_row_index)) {
          std::memcpy(reference_.data() + frame_offset,
                      reinterpret_cast<const float *>(slot.payload.data() +
                                                      payload_offset),
                      row_bytes);
          if (tracking_missing) {
            missing_spans.push_back(pending_missing);
            pending_missing = {};
            tracking_missing = false;
          }
          continue;
        }

        if (!tracking_missing) {
          pending_missing = {.start_row = frame_row, .row_count = 0};
          tracking_missing = true;
        }
        ++pending_missing.row_count;
      }

      if (tracking_missing) {
        missing_spans.push_back(pending_missing);
      }
    }

    return missing_spans;
  }

  [[nodiscard]] std::optional<uint16_t>
  enqueueReconstructedReference(const TemporalAssemblySlot &slot, bool partial,
                               bool expired_timeout, uint32_t healed_rows) {
    const auto ready_slot = pool_.tryLeaseFreeSlot();
    if (!ready_slot.has_value()) {
      return std::nullopt;
    }

    FrameDescriptor descriptor{
        .sequence = slot.sequence,
        .bytes_used = slot.bytes_received,
        .timestamp_us = slot.timestamp_us,
        .slot_index = *ready_slot,
        .frame_id = slot.frame_id,
        .refresh_start_row = slot.refresh_start_row,
        .refresh_row_count = slot.refresh_row_count,
        .total_fragments = slot.total_fragments,
        .fragments_received = slot.fragments_received,
        .missing_fragments = static_cast<uint16_t>(slot.total_fragments -
                                                   slot.fragments_received),
        .quantization = slot.quantization,
        .type_flags = slot.type_flags,
        .descriptor_flags = static_cast<uint8_t>((partial ? 0x1u : 0x0u) |
                                                 (expired_timeout ? 0x2u : 0x0u)),
    };
    (void)healed_rows;

    if (!pool_.publishReadyFrame(*ready_slot, descriptor, reference_.data())) {
      pool_.releaseProcessedSlot(*ready_slot);
      return std::nullopt;
    }

    if (!ready_queue_.push(*ready_slot)) {
      pool_.releaseProcessedSlot(*ready_slot);
      return std::nullopt;
    }

    return ready_slot;
  }
};

} // namespace NetDSP
