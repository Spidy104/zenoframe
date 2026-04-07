#include "Protocol.hpp"
#include <array>
#include <cassert>
#include <cstring>
#include <print>

void test_header_layout() {
  std::print("Test: PacketHeader Binary Layout... ");

  static_assert(sizeof(NetDSP::PacketHeader) == 28);
  static_assert(NetDSP::HEADER_SIZE == 28);

  NetDSP::PacketHeader header{};
  header.magic = NetDSP::MAGIC_NUMBER;
  header.sequence = 42;
  header.frame_id = 7;
  header.fragment_index = 3;
  header.total_fragments = 8;
  header.timestamp_us = 123456789ULL;
  header.type_flags =
      NetDSP::FLAG_I_FRAME | NetDSP::FLAG_CS_ENABLED;
  header.quantization = 16;
  header.refresh_start_row = 120;
  header.refresh_row_count = 24;

  std::array<unsigned char, sizeof(NetDSP::PacketHeader)> bytes{};
  std::memcpy(bytes.data(), &header, sizeof(header));

  assert(bytes[0] == 0x4E);
  assert(bytes[1] == 0x44);
  assert(bytes[2] == 0x53);
  assert(bytes[3] == 0x50);
  assert(bytes[22] ==
         static_cast<unsigned char>(NetDSP::FLAG_I_FRAME |
                                    NetDSP::FLAG_CS_ENABLED));
  assert(bytes[23] == 16);
  assert(bytes[24] == 120);
  assert(bytes[25] == 0);
  assert(bytes[26] == 24);
  assert(bytes[27] == 0);

  std::println("PASSED");
}

void test_header_validation() {
  std::print("Test: Header Validation Rules... ");

  NetDSP::PacketHeader valid{
      .magic = NetDSP::MAGIC_NUMBER,
      .sequence = 1,
      .frame_id = 2,
      .fragment_index = 0,
      .total_fragments = 4,
      .timestamp_us = 1000,
      .type_flags = NetDSP::FLAG_P_FRAME,
      .quantization = 8,
        .refresh_start_row = 0,
        .refresh_row_count = 0,
  };

  assert(NetDSP::isValidHeader(valid));

  valid.magic = 0;
  assert(!NetDSP::isValidHeader(valid));

  valid.magic = NetDSP::MAGIC_NUMBER;
  valid.total_fragments = 0;
  assert(!NetDSP::isValidHeader(valid));

  valid.total_fragments = 2;
  valid.fragment_index = 2;
  assert(!NetDSP::isValidHeader(valid));

  valid.fragment_index = 1;
  valid.quantization = 12;
  assert(!NetDSP::isValidHeader(valid));

  valid.quantization = 8;
  valid.refresh_start_row = 10;
  valid.refresh_row_count = 0;
  assert(!NetDSP::isValidHeader(valid));

  valid.refresh_start_row = 10;
  valid.refresh_row_count = 4;
  assert(NetDSP::isValidHeader(valid));

  std::println("PASSED");
}

void test_flags() {
  std::print("Test: Type Flag Helpers... ");

  const uint8_t flags =
      NetDSP::FLAG_I_FRAME | NetDSP::FLAG_CS_ENABLED |
      NetDSP::FLAG_TEMPORAL_REFRESH |
      NetDSP::FLAG_COMPRESSIVE_SAMPLING;

  assert(NetDSP::hasFlag(flags, NetDSP::FLAG_I_FRAME));
  assert(!NetDSP::hasFlag(flags, NetDSP::FLAG_P_FRAME));
  assert(NetDSP::hasFlag(flags, NetDSP::FLAG_CS_ENABLED));
  assert(NetDSP::hasFlag(flags, NetDSP::FLAG_TEMPORAL_REFRESH));
  assert(NetDSP::hasFlag(flags, NetDSP::FLAG_COMPRESSIVE_SAMPLING));

  std::println("PASSED");
}

void test_fragment_math() {
  std::print("Test: Fragmentation Math... ");

  static_assert(NetDSP::DEFAULT_PACKET_BYTES > NetDSP::HEADER_SIZE);
  assert(NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES ==
         NetDSP::DEFAULT_PACKET_BYTES - NetDSP::HEADER_SIZE);
  assert(NetDSP::fragmentsForPayloadBytes(0) == 0);
  assert(NetDSP::fragmentsForPayloadBytes(NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES) ==
         1);
  assert(NetDSP::fragmentsForPayloadBytes(NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES +
                                          1) == 2);

  const uint16_t fragments_1080p_8 =
      NetDSP::fragmentsForFrame(1920, 1080, 8);
  const uint16_t fragments_1080p_16 =
      NetDSP::fragmentsForFrame(1920, 1080, 16);

  assert(fragments_1080p_8 > 0);
  assert(fragments_1080p_16 > fragments_1080p_8);
  assert(NetDSP::fragmentsForFrame(1920, 1080, 12) == 0);

  std::println("PASSED");
}

void test_wire_conversion_round_trip() {
  std::print("Test: Endian Conversion Round Trip... ");

  const NetDSP::PacketHeader host{
      .magic = NetDSP::MAGIC_NUMBER,
      .sequence = 0x11223344u,
      .frame_id = 0x5566u,
      .fragment_index = 0x7788u,
      .total_fragments = 0x99AAu,
      .timestamp_us = 0x0102030405060708ULL,
      .type_flags = NetDSP::FLAG_I_FRAME | NetDSP::FLAG_P_FRAME,
      .quantization = 32,
        .refresh_start_row = 0x0A0Bu,
        .refresh_row_count = 0x0C0Du,
  };

  const NetDSP::PacketHeader wire = NetDSP::hostToWire(host);
  const NetDSP::PacketHeader round_trip = NetDSP::wireToHost(wire);

  assert(round_trip.magic == host.magic);
  assert(round_trip.sequence == host.sequence);
  assert(round_trip.frame_id == host.frame_id);
  assert(round_trip.fragment_index == host.fragment_index);
  assert(round_trip.total_fragments == host.total_fragments);
  assert(round_trip.timestamp_us == host.timestamp_us);
  assert(round_trip.type_flags == host.type_flags);
  assert(round_trip.quantization == host.quantization);
  assert(round_trip.refresh_start_row == host.refresh_start_row);
  assert(round_trip.refresh_row_count == host.refresh_row_count);

  std::println("PASSED");
}

void test_temporal_refresh_metadata() {
  std::print("Test: Temporal Refresh Metadata + Debug... ");

  const NetDSP::PacketHeader temporal{
      .magic = NetDSP::MAGIC_NUMBER,
      .sequence = 900,
      .frame_id = 15,
      .fragment_index = 0,
      .total_fragments = 3,
      .timestamp_us = 55555,
      .type_flags = NetDSP::FLAG_P_FRAME | NetDSP::FLAG_TEMPORAL_REFRESH,
      .quantization = 32,
      .refresh_start_row = 144,
      .refresh_row_count = 12,
  };

  assert(NetDSP::usesTemporalRefresh(temporal));
  assert(NetDSP::hasFlag(temporal.type_flags, NetDSP::FLAG_TEMPORAL_REFRESH));
  assert(NetDSP::isValidHeader(temporal));

  std::println("  debug: seq={} frame_id={} rows=[{}, {}) flags=0x{:02X}",
               temporal.sequence, temporal.frame_id,
               temporal.refresh_start_row,
               temporal.refresh_start_row + temporal.refresh_row_count,
               temporal.type_flags);

  std::println("PASSED");
}

void test_compressive_sampling_metadata() {
  std::print("Test: Compressive Sampling Metadata + Debug... ");

  const NetDSP::PacketHeader compressive{
      .magic = NetDSP::MAGIC_NUMBER,
      .sequence = 901,
      .frame_id = 19,
      .fragment_index = 0,
      .total_fragments = 5,
      .timestamp_us = 77777,
      .type_flags = NetDSP::FLAG_P_FRAME | NetDSP::FLAG_COMPRESSIVE_SAMPLING,
      .quantization = 32,
      .refresh_start_row = 0,
      .refresh_row_count = 0,
  };

  assert(NetDSP::usesCompressiveSampling(compressive));
  assert(NetDSP::hasFlag(compressive.type_flags,
                         NetDSP::FLAG_COMPRESSIVE_SAMPLING));
  assert(NetDSP::isValidHeader(compressive));

  std::println("  debug: seq={} frame_id={} flags=0x{:02X}",
               compressive.sequence, compressive.frame_id,
               compressive.type_flags);

  std::println("PASSED");
}

int main() {
  std::println("==========================================================");
  std::println("Protocol Contract Tests");
  std::println("==========================================================\n");

  test_header_layout();
  test_header_validation();
  test_flags();
  test_fragment_math();
  test_wire_conversion_round_trip();
  test_temporal_refresh_metadata();
  test_compressive_sampling_metadata();

  std::println("\n==========================================================");
  std::println("All protocol tests PASSED!");
  std::println("==========================================================");

  return 0;
}
