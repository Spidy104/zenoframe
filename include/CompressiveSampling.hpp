#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace NetDSP {

inline constexpr uint16_t CS_TILE_WIDTH = 24;
inline constexpr uint16_t CS_TILE_HEIGHT = 10;

struct CompressiveSamplingConfig {
  float sampling_ratio{0.10f};
  uint8_t min_samples_per_tile{6};
  uint8_t max_omp_atoms{8};
  uint8_t dictionary_atoms{24};
  uint16_t tile_width{CS_TILE_WIDTH};
  uint16_t tile_height{CS_TILE_HEIGHT};
  uint32_t sampling_seed{0xC05E1234u};
};

struct CompressiveFrameStats {
  size_t tile_count{0};
  size_t sample_count{0};
  size_t payload_bytes{0};
  double sample_ratio{0.0};
  double payload_ratio{0.0};
  double mean_abs_error{0.0};
  float max_abs_error{0.0f};
  double elapsed_ms{0.0};
};

struct CompressivePayloadHeader {
  uint16_t tile_width{0};
  uint16_t tile_height{0};
  uint16_t tiles_x{0};
  uint16_t tiles_y{0};
  uint8_t max_omp_atoms{0};
  uint8_t dictionary_atoms{0};
  uint16_t tile_count{0};
  uint16_t reserved{0};
};

struct CompressiveTileHeader {
  uint16_t tile_index{0};
  uint8_t sample_count{0};
  uint8_t reserved{0};
};

struct CompressiveTileSample {
  uint8_t local_index{0};
  uint16_t quantized_value{0};
};

static_assert(sizeof(CompressivePayloadHeader) == 14,
              "CompressivePayloadHeader layout changed");
static_assert(sizeof(CompressiveTileHeader) == 4,
              "CompressiveTileHeader layout changed");

[[nodiscard]] bool isValidCompressiveConfig(const CompressiveSamplingConfig &config,
                                            uint32_t width, uint32_t height);

[[nodiscard]] std::vector<std::byte>
encodeCompressiveFramePayload(const float *frame_pixels, uint32_t width,
                              uint32_t height,
                              const CompressiveSamplingConfig &config,
                              uint32_t frame_seed,
                              CompressiveFrameStats *stats = nullptr);

[[nodiscard]] bool reconstructCompressiveFramePayload(
    const std::byte *payload_bytes, size_t payload_size, uint32_t width,
    uint32_t height, float *out_pixels,
    CompressiveFrameStats *stats = nullptr);

} // namespace NetDSP
