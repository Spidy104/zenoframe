#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

/**
 * NDSP (Native Digital Signal Protocol) -
 *
 * Binary contract for the 144 Hz frame stream.
 *
 * Wire rules:
 * - All multi-byte integer fields are serialized as little-endian.
 * - timestamp_us uses a monotonic clock in microseconds.
 * - quantization is restricted to 8, 16, or 32 bits per sample.
 */
namespace NetDSP {

inline constexpr uint32_t PROTOCOL_VERSION = 1;
inline constexpr uint32_t MAGIC_NUMBER = 0x5053444E; // 'NDSP'

enum TypeFlags : uint8_t {
  FLAG_I_FRAME = 1u << 0,
  FLAG_P_FRAME = 1u << 1,
  FLAG_CS_ENABLED = 1u << 2,
  FLAG_TEMPORAL_REFRESH = 1u << 3,
  FLAG_COMPRESSIVE_SAMPLING = 1u << 4,
};

#pragma pack(push, 1)
struct PacketHeader {
  uint32_t magic;           // 'NDSP' (0x5053444E)
  uint32_t sequence;        // Global packet counter
  uint16_t frame_id;        // Shadow buffer index
  uint16_t fragment_index;  // Position within the frame
  uint16_t total_fragments; // Total fragments for this frame
  uint64_t timestamp_us;    // Monotonic microseconds for deadline tracking
  uint8_t type_flags;       // See NetDSP::TypeFlags
  uint8_t quantization;     // Allowed values: 8, 16, 32
  uint16_t refresh_start_row; // 0 for full-frame payloads
  uint16_t refresh_row_count; // 0 for full-frame payloads
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 28, "PacketHeader layout changed");
static_assert(offsetof(PacketHeader, magic) == 0, "magic offset changed");
static_assert(offsetof(PacketHeader, sequence) == 4, "sequence offset changed");
static_assert(offsetof(PacketHeader, frame_id) == 8, "frame_id offset changed");
static_assert(offsetof(PacketHeader, fragment_index) == 10,
              "fragment_index offset changed");
static_assert(offsetof(PacketHeader, total_fragments) == 12,
              "total_fragments offset changed");
static_assert(offsetof(PacketHeader, timestamp_us) == 14,
              "timestamp_us offset changed");
static_assert(offsetof(PacketHeader, type_flags) == 22,
              "type_flags offset changed");
static_assert(offsetof(PacketHeader, quantization) == 23,
              "quantization offset changed");
static_assert(offsetof(PacketHeader, refresh_start_row) == 24,
              "refresh_start_row offset changed");
static_assert(offsetof(PacketHeader, refresh_row_count) == 26,
              "refresh_row_count offset changed");

inline constexpr size_t HEADER_SIZE = sizeof(PacketHeader);
inline constexpr size_t DEFAULT_PACKET_BYTES = 1200;
inline constexpr size_t MAX_FRAGMENT_PAYLOAD_BYTES =
    DEFAULT_PACKET_BYTES - HEADER_SIZE;

[[nodiscard]] constexpr bool isValidMagic(uint32_t magic) {
  return magic == MAGIC_NUMBER;
}

[[nodiscard]] constexpr bool isValidQuantization(uint8_t quantization) {
  return quantization == 8 || quantization == 16 || quantization == 32;
}

[[nodiscard]] constexpr bool hasFlag(uint8_t type_flags, TypeFlags flag) {
  return (type_flags & static_cast<uint8_t>(flag)) != 0;
}

[[nodiscard]] constexpr bool usesTemporalRefresh(const PacketHeader &header) {
  return header.refresh_row_count > 0;
}

[[nodiscard]] constexpr bool usesCompressiveSampling(const PacketHeader &header) {
  return hasFlag(header.type_flags, FLAG_COMPRESSIVE_SAMPLING);
}

[[nodiscard]] constexpr bool hasValidRefreshWindow(const PacketHeader &header) {
  if (header.refresh_row_count == 0) {
    return header.refresh_start_row == 0;
  }
  return true;
}

[[nodiscard]] constexpr bool isValidHeader(const PacketHeader &header) {
  return isValidMagic(header.magic) &&
         header.total_fragments > 0 &&
         header.fragment_index < header.total_fragments &&
         isValidQuantization(header.quantization) &&
         hasValidRefreshWindow(header);
}

[[nodiscard]] constexpr uint16_t fragmentsForPayloadBytes(size_t payload_bytes) {
  if (payload_bytes == 0) {
    return 0;
  }

  const size_t fragments =
      (payload_bytes + MAX_FRAGMENT_PAYLOAD_BYTES - 1) /
      MAX_FRAGMENT_PAYLOAD_BYTES;
  return static_cast<uint16_t>(fragments);
}

[[nodiscard]] constexpr uint16_t fragmentsForFrame(uint32_t width,
                                                   uint32_t height,
                                                   uint8_t quantization) {
  if (width == 0 || height == 0 || !isValidQuantization(quantization)) {
    return 0;
  }

  const size_t bytes_per_sample = quantization / 8;
  const size_t frame_bytes =
      static_cast<size_t>(width) * static_cast<size_t>(height) *
      bytes_per_sample;
  return fragmentsForPayloadBytes(frame_bytes);
}

[[nodiscard]] constexpr uint16_t toWire16(uint16_t value) {
  if constexpr (std::endian::native == std::endian::little) {
    return value;
  } else {
    return std::byteswap(value);
  }
}

[[nodiscard]] constexpr uint32_t toWire32(uint32_t value) {
  if constexpr (std::endian::native == std::endian::little) {
    return value;
  } else {
    return std::byteswap(value);
  }
}

[[nodiscard]] constexpr uint64_t toWire64(uint64_t value) {
  if constexpr (std::endian::native == std::endian::little) {
    return value;
  } else {
    return std::byteswap(value);
  }
}

[[nodiscard]] constexpr uint16_t fromWire16(uint16_t value) {
  return toWire16(value);
}

[[nodiscard]] constexpr uint32_t fromWire32(uint32_t value) {
  return toWire32(value);
}

[[nodiscard]] constexpr uint64_t fromWire64(uint64_t value) {
  return toWire64(value);
}

[[nodiscard]] constexpr PacketHeader hostToWire(PacketHeader header) {
  header.magic = toWire32(header.magic);
  header.sequence = toWire32(header.sequence);
  header.frame_id = toWire16(header.frame_id);
  header.fragment_index = toWire16(header.fragment_index);
  header.total_fragments = toWire16(header.total_fragments);
  header.timestamp_us = toWire64(header.timestamp_us);
  header.refresh_start_row = toWire16(header.refresh_start_row);
  header.refresh_row_count = toWire16(header.refresh_row_count);
  return header;
}

[[nodiscard]] constexpr PacketHeader wireToHost(PacketHeader header) {
  header.magic = fromWire32(header.magic);
  header.sequence = fromWire32(header.sequence);
  header.frame_id = fromWire16(header.frame_id);
  header.fragment_index = fromWire16(header.fragment_index);
  header.total_fragments = fromWire16(header.total_fragments);
  header.timestamp_us = fromWire64(header.timestamp_us);
  header.refresh_start_row = fromWire16(header.refresh_start_row);
  header.refresh_row_count = fromWire16(header.refresh_row_count);
  return header;
}

[[nodiscard]] inline size_t serializeDatagram(const PacketHeader &header,
                                              const void *payload,
                                              size_t payload_bytes,
                                              void *out_buffer,
                                              size_t out_capacity) {
  if (payload == nullptr || out_buffer == nullptr || payload_bytes == 0 ||
      payload_bytes > MAX_FRAGMENT_PAYLOAD_BYTES ||
      out_capacity < HEADER_SIZE + payload_bytes) {
    return 0;
  }

  const PacketHeader wire_header = hostToWire(header);
  auto *out = static_cast<std::byte *>(out_buffer);
  std::memcpy(out, &wire_header, HEADER_SIZE);
  std::memcpy(out + HEADER_SIZE, payload, payload_bytes);
  return HEADER_SIZE + payload_bytes;
}

[[nodiscard]] inline bool parseDatagram(const void *packet_bytes,
                                        size_t packet_size,
                                        PacketHeader &header,
                                        const std::byte *&payload,
                                        size_t &payload_bytes) {
  if (packet_bytes == nullptr || packet_size < HEADER_SIZE ||
      packet_size > DEFAULT_PACKET_BYTES) {
    return false;
  }

  PacketHeader wire_header{};
  std::memcpy(&wire_header, packet_bytes, HEADER_SIZE);
  header = wireToHost(wire_header);
  if (!isValidHeader(header)) {
    return false;
  }

  payload =
      static_cast<const std::byte *>(packet_bytes) + HEADER_SIZE;
  payload_bytes = packet_size - HEADER_SIZE;
  return payload_bytes > 0 && payload_bytes <= MAX_FRAGMENT_PAYLOAD_BYTES;
}

} // namespace NetDSP
