#pragma once

#include "CompressiveSampling.hpp"
#include "FrameBufferPool.hpp"
#include "TemporalRefresh.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace NetDSP {

class SenderEngine {
  uint32_t next_sequence_{1};

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
  [[nodiscard]] uint32_t reserveSequence() { return next_sequence_++; }

  [[nodiscard]] static constexpr size_t frameBytes() {
    return FrameBufferPool<>::slotBytes();
  }

  [[nodiscard]] static constexpr uint16_t totalFragments() {
    return FrameBufferPool<>::totalFragments();
  }

  [[nodiscard]] static constexpr size_t payloadBytesForFragment(
      uint16_t fragment_index) {
    return FrameBufferPool<>::expectedFragmentPayloadBytes(fragment_index);
  }

  [[nodiscard]] static PacketHeader makeHeader(uint32_t sequence,
                                               uint16_t frame_id,
                                               uint16_t fragment_index,
                                               uint64_t timestamp_us,
                                               uint8_t type_flags) {
    return PacketHeader{
        .magic = MAGIC_NUMBER,
        .sequence = sequence,
        .frame_id = frame_id,
        .fragment_index = fragment_index,
        .total_fragments = totalFragments(),
        .timestamp_us = timestamp_us,
        .type_flags = type_flags,
        .quantization = FrameBufferPool<>::quantization(),
        .refresh_start_row = 0,
        .refresh_row_count = 0,
    };
  }

  [[nodiscard]] static PacketHeader makeTemporalHeader(
      uint32_t sequence, uint16_t frame_id, uint16_t fragment_index,
      uint16_t total_fragments, uint64_t timestamp_us, uint8_t type_flags,
      uint16_t refresh_start_row, uint16_t refresh_row_count) {
    return PacketHeader{
        .magic = MAGIC_NUMBER,
        .sequence = sequence,
        .frame_id = frame_id,
        .fragment_index = fragment_index,
        .total_fragments = total_fragments,
        .timestamp_us = timestamp_us,
        .type_flags = static_cast<uint8_t>(type_flags | FLAG_TEMPORAL_REFRESH),
        .quantization = FrameBufferPool<>::quantization(),
        .refresh_start_row = refresh_start_row,
        .refresh_row_count = refresh_row_count,
    };
  }

  [[nodiscard]] static PacketHeader makeCompressiveHeader(
      uint32_t sequence, uint16_t frame_id, uint16_t fragment_index,
      uint16_t total_fragments, uint64_t timestamp_us, uint8_t type_flags) {
    return PacketHeader{
        .magic = MAGIC_NUMBER,
        .sequence = sequence,
        .frame_id = frame_id,
        .fragment_index = fragment_index,
        .total_fragments = total_fragments,
        .timestamp_us = timestamp_us,
        .type_flags =
            static_cast<uint8_t>(type_flags | FLAG_COMPRESSIVE_SAMPLING),
        .quantization = FrameBufferPool<>::quantization(),
        .refresh_start_row = 0,
        .refresh_row_count = 0,
    };
  }

  [[nodiscard]] static const std::byte *payloadForFragment(
      const float *frame_pixels, uint16_t fragment_index) {
    const auto *frame_bytes =
        reinterpret_cast<const std::byte *>(frame_pixels);
    return frame_bytes +
           static_cast<size_t>(fragment_index) * MAX_FRAGMENT_PAYLOAD_BYTES;
  }

  template <typename Sink>
  bool sendFrame(const float *frame_pixels, uint16_t frame_id,
                 uint64_t timestamp_us, Sink &&sink,
                 uint8_t type_flags = FLAG_I_FRAME | FLAG_CS_ENABLED) {
    if (frame_pixels == nullptr) {
      return false;
    }

    const uint32_t sequence = reserveSequence();
    for (uint16_t fragment_index = 0; fragment_index < totalFragments();
         ++fragment_index) {
      const PacketHeader header =
          makeHeader(sequence, frame_id, fragment_index, timestamp_us,
                     type_flags);
      if (!sink(header, payloadForFragment(frame_pixels, fragment_index),
                payloadBytesForFragment(fragment_index))) {
        return false;
      }
    }

    return true;
  }

  template <typename Sink>
  bool sendTemporalRefresh(const float *frame_pixels, uint16_t frame_id,
                           uint64_t timestamp_us, const RefreshPlan &plan,
                           Sink &&sink,
                           uint8_t type_flags = FLAG_P_FRAME | FLAG_CS_ENABLED |
                                                FLAG_TEMPORAL_REFRESH) {
    if (frame_pixels == nullptr) {
      return false;
    }

    const uint32_t payload_rows = plan.payloadRowCount();
    if (payload_rows == 0) {
      return false;
    }

    const uint16_t refresh_start_row = static_cast<uint16_t>(plan.spans[0].start_row);
    const uint16_t refresh_row_count = static_cast<uint16_t>(payload_rows);
    const std::vector<float> payload_rows_buffer =
        extractRefreshPayload(plan, frame_pixels, FrameBufferPool<>::frameWidth());

    const auto *payload_bytes =
        reinterpret_cast<const std::byte *>(payload_rows_buffer.data());
    const size_t payload_bytes_total =
        payload_rows_buffer.size() * sizeof(float);
    const uint16_t total_fragments =
        fragmentsForPayloadBytes(payload_bytes_total);
    if (total_fragments == 0) {
      return false;
    }

    const uint32_t sequence = reserveSequence();
    for (uint16_t fragment_index = 0; fragment_index < total_fragments;
         ++fragment_index) {
      const size_t offset =
          static_cast<size_t>(fragment_index) * MAX_FRAGMENT_PAYLOAD_BYTES;
      const size_t fragment_payload_bytes =
          payloadBytesForVariableFragment(payload_bytes_total, fragment_index);
      if (fragment_payload_bytes == 0) {
        return false;
      }

      const PacketHeader header = makeTemporalHeader(
          sequence, frame_id, fragment_index, total_fragments, timestamp_us,
          type_flags, refresh_start_row, refresh_row_count);
      if (!sink(header, payload_bytes + offset, fragment_payload_bytes)) {
        return false;
      }
    }

    return true;
  }

  template <typename Sink>
  bool sendDistributedIntraRefreshFrame(
      const float *frame_pixels, uint16_t frame_id, uint64_t timestamp_us,
      uint64_t refresh_frame_index,
      const DistributedIntraRefreshScheduler &scheduler, Sink &&sink,
      bool seed_with_full_frame = false,
      uint8_t full_frame_flags = FLAG_I_FRAME | FLAG_CS_ENABLED,
      uint8_t refresh_flags = FLAG_P_FRAME | FLAG_CS_ENABLED |
                              FLAG_TEMPORAL_REFRESH) {
    if (seed_with_full_frame) {
      return sendFrame(frame_pixels, frame_id, timestamp_us,
                       std::forward<Sink>(sink), full_frame_flags);
    }

    const RefreshPlan plan = scheduler.planForFrame(refresh_frame_index);
    return sendTemporalRefresh(frame_pixels, frame_id, timestamp_us, plan,
                               std::forward<Sink>(sink), refresh_flags);
  }

  template <typename Sink>
  bool sendCompressiveFrame(
      const float *frame_pixels, uint16_t frame_id, uint64_t timestamp_us,
      const CompressiveSamplingConfig &config, Sink &&sink,
      uint8_t type_flags = FLAG_P_FRAME | FLAG_CS_ENABLED |
                           FLAG_COMPRESSIVE_SAMPLING) {
    if (frame_pixels == nullptr) {
      return false;
    }

    CompressiveFrameStats stats{};
    const std::vector<std::byte> payload = encodeCompressiveFramePayload(
        frame_pixels, FrameBufferPool<>::frameWidth(),
        FrameBufferPool<>::frameHeight(), config, frame_id, &stats);
    if (payload.empty()) {
      return false;
    }

    const uint16_t total_fragments =
        fragmentsForPayloadBytes(payload.size());
    if (total_fragments == 0) {
      return false;
    }

    const uint32_t sequence = reserveSequence();
    for (uint16_t fragment_index = 0; fragment_index < total_fragments;
         ++fragment_index) {
      const size_t offset =
          static_cast<size_t>(fragment_index) * MAX_FRAGMENT_PAYLOAD_BYTES;
      const size_t fragment_payload_bytes =
          payloadBytesForVariableFragment(payload.size(), fragment_index);
      if (fragment_payload_bytes == 0) {
        return false;
      }

      const PacketHeader header = makeCompressiveHeader(
          sequence, frame_id, fragment_index, total_fragments, timestamp_us,
          type_flags);
      if (!sink(header, payload.data() + offset, fragment_payload_bytes)) {
        return false;
      }
    }

    return true;
  }
};

} // namespace NetDSP
