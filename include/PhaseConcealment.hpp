#pragma once

#include <cstdint>
#include <limits>
#include <vector>

namespace NetDSP {

struct ConcealmentSpan {
  uint32_t start_row{0};
  uint32_t row_count{0};
};

struct ConcealmentReport {
  uint32_t healed_rows{0};
  double elapsed_ms{0.0};
};

struct ConcealmentWorkspace {
  uint32_t width{0};
  uint32_t height{0};
  std::vector<uint8_t> row_missing;
  std::vector<float> prev_real;
  std::vector<float> next_real;
  std::vector<float> prev_imag;
  std::vector<float> next_imag;
  std::vector<float> scratch_imag;
  uint32_t cached_prev_row{std::numeric_limits<uint32_t>::max()};
  uint32_t cached_next_row{std::numeric_limits<uint32_t>::max()};
  bool prev_cache_valid{false};
  bool next_cache_valid{false};

  ConcealmentWorkspace() = default;

  ConcealmentWorkspace(uint32_t frame_width, uint32_t frame_height) {
    ensureCapacity(frame_width, frame_height);
  }

  void ensureCapacity(uint32_t frame_width, uint32_t frame_height) {
    width = frame_width;
    height = frame_height;
    row_missing.resize(frame_height);
    prev_real.resize(frame_width);
    next_real.resize(frame_width);
    prev_imag.resize(frame_width);
    next_imag.resize(frame_width);
    scratch_imag.resize(frame_width);
    resetCache();
  }

  void resetCache() {
    cached_prev_row = std::numeric_limits<uint32_t>::max();
    cached_next_row = std::numeric_limits<uint32_t>::max();
    prev_cache_valid = false;
    next_cache_valid = false;
  }
};

void computeHilbertImag1D(const float *input_row, uint32_t width,
                          float *imag_out);

[[nodiscard]] ConcealmentReport
concealMissingRowsWithAnalyticContinuation(float *pixels, uint32_t width,
                                           uint32_t height,
                                           const std::vector<ConcealmentSpan> &spans,
                                           ConcealmentWorkspace &workspace,
                                           bool measure_timing = true);

[[nodiscard]] ConcealmentReport
concealMissingRowsWithAnalyticContinuation(float *pixels, uint32_t width,
                                           uint32_t height,
                                           const std::vector<ConcealmentSpan> &spans);

} // namespace NetDSP
