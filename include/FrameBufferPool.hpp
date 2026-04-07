#pragma once

#include "Protocol.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>

namespace NetDSP {

enum class SlotState : uint32_t {
  Free = 0,
  Filling = 1,
  Ready = 2,
  Processing = 3,
};

inline constexpr size_t SHADOW_BUFFER_ALIGNMENT = 64;
inline constexpr uint32_t SHADOW_FRAME_WIDTH = 1920;
inline constexpr uint32_t SHADOW_FRAME_HEIGHT = 1080;
inline constexpr uint8_t SHADOW_FRAME_QUANTIZATION = 32;
inline constexpr size_t SHADOW_PIXEL_COUNT =
    static_cast<size_t>(SHADOW_FRAME_WIDTH) * SHADOW_FRAME_HEIGHT;
inline constexpr size_t SHADOW_BUFFER_BYTES =
    SHADOW_PIXEL_COUNT * sizeof(float);
inline constexpr uint16_t DEFAULT_SHADOW_BUFFER_COUNT = 16;
inline constexpr uint16_t SHADOW_TOTAL_FRAGMENTS =
    fragmentsForPayloadBytes(SHADOW_BUFFER_BYTES);
inline constexpr size_t SHADOW_FRAGMENT_BITMAP_WORDS =
    (SHADOW_TOTAL_FRAGMENTS + 63u) / 64u;

static_assert(SHADOW_TOTAL_FRAGMENTS > 0,
              "Shadow buffer must require at least one fragment");
static_assert(SHADOW_TOTAL_FRAGMENTS <= UINT16_MAX,
              "Shadow buffer fragment count exceeds protocol field size");

/**
 * Descriptor for a leased shadow buffer.
 *
 * This metadata moves through queues while the 1080p payload stays in a
 * preallocated aligned slot.
 */
struct FrameDescriptor {
  uint32_t sequence{0};
  uint32_t bytes_used{0};
  uint64_t timestamp_us{0};
  uint16_t slot_index{0};
  uint16_t frame_id{0};
  uint16_t refresh_start_row{0};
  uint16_t refresh_row_count{0};
  uint16_t total_fragments{0};
  uint16_t fragments_received{0};
  uint16_t missing_fragments{0};
  uint8_t quantization{0};
  uint8_t type_flags{0};
  uint8_t descriptor_flags{0};
  uint8_t reserved0{0};

  [[nodiscard]] constexpr bool isComplete() const {
    return total_fragments > 0 && fragments_received == total_fragments;
  }

  [[nodiscard]] constexpr bool isPartial() const {
    return (descriptor_flags & 0x1u) != 0;
  }

  [[nodiscard]] constexpr bool usesTemporalRefresh() const {
    return hasFlag(type_flags, FLAG_TEMPORAL_REFRESH);
  }
};

static_assert(sizeof(FrameDescriptor) == 40,
              "FrameDescriptor layout changed");

struct FragmentWriteResult {
  bool accepted{false};
  bool duplicate{false};
  bool frame_complete{false};
};

struct FrameAssemblyState {
  uint32_t sequence{0};
  uint64_t timestamp_us{0};
  uint64_t first_fragment_arrival_us{0};
  uint64_t last_fragment_arrival_us{0};
  uint64_t deadline_us{0};
  uint32_t bytes_received{0};
  uint16_t frame_id{0};
  uint16_t total_fragments{0};
  uint16_t received_fragments{0};
  uint8_t quantization{0};
  uint8_t type_flags{0};
  std::array<uint64_t, SHADOW_FRAGMENT_BITMAP_WORDS> fragment_bitmap{};

  [[nodiscard]] bool active() const { return total_fragments != 0; }

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

  void reset() { *this = {}; }
};

struct alignas(SHADOW_BUFFER_ALIGNMENT) ShadowBuffer {
  alignas(SHADOW_BUFFER_ALIGNMENT) float pixels[SHADOW_PIXEL_COUNT]{};
  FrameDescriptor descriptor{};
  FrameAssemblyState assembly{};

  [[nodiscard]] float *data() { return pixels; }
  [[nodiscard]] const float *data() const { return pixels; }

  [[nodiscard]] std::byte *bytes() {
    return reinterpret_cast<std::byte *>(pixels);
  }

  [[nodiscard]] const std::byte *bytes() const {
    return reinterpret_cast<const std::byte *>(pixels);
  }

  [[nodiscard]] static constexpr size_t pixelCount() { return SHADOW_PIXEL_COUNT; }
  [[nodiscard]] static constexpr size_t byteSize() { return SHADOW_BUFFER_BYTES; }

  void reset() {
    descriptor = {};
    assembly.reset();
  }
};

static_assert(alignof(ShadowBuffer) >= SHADOW_BUFFER_ALIGNMENT,
              "ShadowBuffer alignment is too small");

template <size_t SlotCount = DEFAULT_SHADOW_BUFFER_COUNT> class FrameBufferPool {
  static_assert(SlotCount > 0, "FrameBufferPool requires at least one slot");
  static_assert(SlotCount <= UINT16_MAX,
                "FrameBufferPool slot count exceeds descriptor capacity");

  std::unique_ptr<ShadowBuffer[]> slots_;
  std::unique_ptr<std::atomic<uint32_t>[]> states_;

  static constexpr uint32_t toRawState(SlotState state) {
    return static_cast<uint32_t>(state);
  }

public:
  FrameBufferPool()
      : slots_(std::make_unique<ShadowBuffer[]>(SlotCount)),
        states_(std::make_unique<std::atomic<uint32_t>[]>(SlotCount)) {
    for (size_t index = 0; index < SlotCount; ++index) {
      states_[index].store(toRawState(SlotState::Free),
                           std::memory_order_relaxed);
    }
  }

  [[nodiscard]] static constexpr size_t slotCount() { return SlotCount; }
  [[nodiscard]] static constexpr size_t slotBytes() { return SHADOW_BUFFER_BYTES; }
  [[nodiscard]] static constexpr size_t pixelCount() { return SHADOW_PIXEL_COUNT; }
  [[nodiscard]] static constexpr uint32_t frameWidth() { return SHADOW_FRAME_WIDTH; }
  [[nodiscard]] static constexpr uint32_t frameHeight() { return SHADOW_FRAME_HEIGHT; }
  [[nodiscard]] static constexpr uint8_t quantization() {
    return SHADOW_FRAME_QUANTIZATION;
  }
  [[nodiscard]] static constexpr uint16_t totalFragments() {
    return SHADOW_TOTAL_FRAGMENTS;
  }

  [[nodiscard]] ShadowBuffer &slot(size_t index) { return slots_[index]; }
  [[nodiscard]] const ShadowBuffer &slot(size_t index) const {
    return slots_[index];
  }

  [[nodiscard]] SlotState state(size_t index) const {
    return static_cast<SlotState>(
        states_[index].load(std::memory_order_acquire));
  }

  [[nodiscard]] bool tryLeaseFreeSlot(size_t index) {
    if (index >= SlotCount) {
      return false;
    }

    uint32_t expected = toRawState(SlotState::Free);
    return states_[index].compare_exchange_strong(
        expected, toRawState(SlotState::Filling), std::memory_order_acq_rel,
        std::memory_order_relaxed);
  }

  [[nodiscard]] std::optional<uint16_t> tryLeaseFreeSlot() {
    for (uint16_t index = 0; index < SlotCount; ++index) {
      if (tryLeaseFreeSlot(index)) {
        return index;
      }
    }

    return std::nullopt;
  }

  [[nodiscard]] bool beginFrameAssembly(size_t index,
                                        const PacketHeader &header,
                                        uint64_t arrival_time_us,
                                        uint64_t frame_timeout_us) {
    if (index >= SlotCount || !isValidHeader(header) ||
        state(index) != SlotState::Filling ||
        header.quantization != SHADOW_FRAME_QUANTIZATION ||
        header.total_fragments != SHADOW_TOTAL_FRAGMENTS) {
      return false;
    }

    auto &slot = slots_[index];
    slot.reset();

    auto &assembly = slot.assembly;
    assembly.sequence = header.sequence;
    assembly.timestamp_us = header.timestamp_us;
    assembly.first_fragment_arrival_us = arrival_time_us;
    assembly.last_fragment_arrival_us = arrival_time_us;
    assembly.deadline_us = arrival_time_us + frame_timeout_us;
    assembly.frame_id = header.frame_id;
    assembly.total_fragments = header.total_fragments;
    assembly.quantization = header.quantization;
    assembly.type_flags = header.type_flags;

    return true;
  }

  [[nodiscard]] FragmentWriteResult writeFragment(size_t index,
                                                  const PacketHeader &header,
                                                  const void *payload,
                                                  size_t payload_bytes,
                                                  uint64_t arrival_time_us,
                                                  uint64_t frame_timeout_us) {
    FragmentWriteResult result{};

    if (index >= SlotCount || payload == nullptr || !isValidHeader(header) ||
        state(index) != SlotState::Filling ||
        header.quantization != SHADOW_FRAME_QUANTIZATION ||
        header.total_fragments != SHADOW_TOTAL_FRAGMENTS ||
        payload_bytes != expectedFragmentPayloadBytes(header.fragment_index)) {
      return result;
    }

    auto &slot = slots_[index];
    auto &assembly = slot.assembly;

    if (!assembly.active()) {
      if (!beginFrameAssembly(index, header, arrival_time_us,
                              frame_timeout_us)) {
        return result;
      }
    } else if (!matchesAssembly(assembly, header)) {
      return result;
    }

    if (assembly.hasFragment(header.fragment_index)) {
      result.duplicate = true;
      return result;
    }

    const size_t offset =
        static_cast<size_t>(header.fragment_index) * MAX_FRAGMENT_PAYLOAD_BYTES;
    std::memcpy(slot.bytes() + offset, payload, payload_bytes);

    assembly.markFragment(header.fragment_index);
    ++assembly.received_fragments;
    assembly.last_fragment_arrival_us = arrival_time_us;
    assembly.bytes_received += static_cast<uint32_t>(payload_bytes);

    result.accepted = true;

    if (assembly.received_fragments == assembly.total_fragments) {
      commitDescriptor(index, false, false);
      result.frame_complete = true;
    }

    return result;
  }

  [[nodiscard]] bool forceCommitPartialSlot(size_t index,
                                            bool expired_timeout = false) {
    if (index >= SlotCount || state(index) != SlotState::Filling) {
      return false;
    }

    const auto &assembly = slots_[index].assembly;
    if (!assembly.active() || assembly.received_fragments == 0) {
      return false;
    }

    commitDescriptor(index, true, expired_timeout);
    return true;
  }

  [[nodiscard]] bool tryAcquireReadySlot(size_t index) {
    if (index >= SlotCount) {
      return false;
    }

    uint32_t expected = toRawState(SlotState::Ready);
    return states_[index].compare_exchange_strong(
        expected, toRawState(SlotState::Processing), std::memory_order_acq_rel,
        std::memory_order_relaxed);
  }

  [[nodiscard]] std::optional<uint16_t> tryAcquireReadySlot() {
    for (uint16_t index = 0; index < SlotCount; ++index) {
      if (tryAcquireReadySlot(index)) {
        return index;
      }
    }

    return std::nullopt;
  }

  void releaseProcessedSlot(size_t index) {
    if (index >= SlotCount) {
      return;
    }

    slots_[index].reset();
    states_[index].store(toRawState(SlotState::Free),
                         std::memory_order_release);
  }

  [[nodiscard]] FrameDescriptor descriptor(size_t index) const {
    return slots_[index].descriptor;
  }

  [[nodiscard]] bool publishReadyFrame(size_t index,
                                       const FrameDescriptor &descriptor,
                                       const float *pixels) {
    if (index >= SlotCount || pixels == nullptr ||
        state(index) != SlotState::Filling) {
      return false;
    }

    auto &slot = slots_[index];
    const bool already_in_place = pixels == slot.data();
    slot.reset();
    if (!already_in_place) {
      std::memcpy(slot.data(), pixels, SHADOW_BUFFER_BYTES);
    }
    slot.descriptor = descriptor;
    slot.descriptor.slot_index = static_cast<uint16_t>(index);
    states_[index].store(toRawState(SlotState::Ready),
                         std::memory_order_release);
    return true;
  }

  [[nodiscard]] FrameAssemblyState assemblyState(size_t index) const {
    return slots_[index].assembly;
  }

  [[nodiscard]] bool hasAssemblyData(size_t index) const {
    return index < SlotCount && slots_[index].assembly.received_fragments > 0;
  }

  [[nodiscard]] bool hasReceivedFragment(size_t index,
                                         uint16_t fragment_index) const {
    if (index >= SlotCount || fragment_index >= SHADOW_TOTAL_FRAGMENTS) {
      return false;
    }

    return slots_[index].assembly.hasFragment(fragment_index);
  }

  [[nodiscard]] static constexpr size_t expectedFragmentPayloadBytes(
      uint16_t fragment_index) {
    if (fragment_index >= SHADOW_TOTAL_FRAGMENTS) {
      return 0;
    }

    const bool is_last = fragment_index == SHADOW_TOTAL_FRAGMENTS - 1;
    if (!is_last) {
      return MAX_FRAGMENT_PAYLOAD_BYTES;
    }

    const size_t remainder = SHADOW_BUFFER_BYTES % MAX_FRAGMENT_PAYLOAD_BYTES;
    return remainder == 0 ? MAX_FRAGMENT_PAYLOAD_BYTES : remainder;
  }

private:
  void commitDescriptor(size_t index, bool partial, bool expired_timeout) {
    auto &slot = slots_[index];
    auto &assembly = slot.assembly;
    auto &descriptor = slot.descriptor;

    descriptor.sequence = assembly.sequence;
    descriptor.bytes_used = assembly.bytes_received;
    descriptor.timestamp_us = assembly.timestamp_us;
    descriptor.slot_index = static_cast<uint16_t>(index);
    descriptor.frame_id = assembly.frame_id;
    descriptor.refresh_start_row = 0;
    descriptor.refresh_row_count = 0;
    descriptor.total_fragments = assembly.total_fragments;
    descriptor.fragments_received = assembly.received_fragments;
    descriptor.missing_fragments =
        static_cast<uint16_t>(assembly.total_fragments -
                              assembly.received_fragments);
    descriptor.quantization = assembly.quantization;
    descriptor.type_flags = assembly.type_flags;
    descriptor.descriptor_flags =
        static_cast<uint8_t>((partial ? 0x1u : 0x0u) |
                             (expired_timeout ? 0x2u : 0x0u));

    states_[index].store(toRawState(SlotState::Ready),
                         std::memory_order_release);
  }

  [[nodiscard]] static bool matchesAssembly(const FrameAssemblyState &assembly,
                                            const PacketHeader &header) {
    return assembly.sequence == header.sequence &&
           assembly.timestamp_us == header.timestamp_us &&
           assembly.frame_id == header.frame_id &&
           assembly.total_fragments == header.total_fragments &&
           assembly.quantization == header.quantization &&
           assembly.type_flags == header.type_flags;
  }
};

} // namespace NetDSP
