#include "PhaseConcealment.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <immintrin.h>
#include <numbers>
#include <vector>

namespace NetDSP {

namespace {

constexpr uint32_t BoundarySupportRows = 2;

constexpr std::array<int, 3> HILBERT_OFFSETS = {1, 3, 5};
constexpr std::array<float, 3> HILBERT_COEFFS = {
    2.0f / (std::numbers::pi_v<float> * 1.0f),
    2.0f / (std::numbers::pi_v<float> * 3.0f),
    2.0f / (std::numbers::pi_v<float> * 5.0f),
};

[[nodiscard]] inline float clamp_indexed(const float *row, uint32_t width,
                                         int32_t index) {
  const int32_t clamped =
      std::clamp(index, 0, static_cast<int32_t>(width) - 1);
  return row[static_cast<uint32_t>(clamped)];
}

void computeHilbertImagScalar(const float *input_row, uint32_t width,
                              float *imag_out, uint32_t begin,
                              uint32_t end) {
  for (uint32_t x = begin; x < end; ++x) {
    float imag = 0.0f;
    for (size_t tap = 0; tap < HILBERT_OFFSETS.size(); ++tap) {
      const int32_t offset = HILBERT_OFFSETS[tap];
      const float coeff = HILBERT_COEFFS[tap];
      imag += coeff * (clamp_indexed(input_row, width, static_cast<int32_t>(x) + offset) -
                       clamp_indexed(input_row, width, static_cast<int32_t>(x) - offset));
    }
    imag_out[x] = imag;
  }
}

[[nodiscard]] uint32_t find_previous_valid_row(
    uint32_t start_row, const std::vector<uint8_t> &row_missing) {
  for (int32_t row = static_cast<int32_t>(start_row) - 1; row >= 0; --row) {
    if (!row_missing[static_cast<uint32_t>(row)]) {
      return static_cast<uint32_t>(row);
    }
  }
  return start_row;
}

[[nodiscard]] uint32_t find_next_valid_row(uint32_t end_row,
                                           uint32_t height,
                                           const std::vector<uint8_t> &row_missing) {
  for (uint32_t row = end_row; row < height; ++row) {
    if (!row_missing[row]) {
      return row;
    }
  }
  return end_row == 0 ? 0 : end_row - 1;
}

template <size_t Capacity> struct NeighborRows {
  std::array<uint32_t, Capacity> rows{};
  uint32_t count{0};
};

template <size_t Capacity>
[[nodiscard]] NeighborRows<Capacity>
collect_previous_valid_rows(uint32_t start_row,
                            const std::vector<uint8_t> &row_missing) {
  NeighborRows<Capacity> result{};
  for (int32_t row = static_cast<int32_t>(start_row) - 1;
       row >= 0 && result.count < Capacity; --row) {
    if (!row_missing[static_cast<uint32_t>(row)]) {
      result.rows[result.count++] = static_cast<uint32_t>(row);
    }
  }
  return result;
}

template <size_t Capacity>
[[nodiscard]] NeighborRows<Capacity>
collect_next_valid_rows(uint32_t end_row, uint32_t height,
                        const std::vector<uint8_t> &row_missing) {
  NeighborRows<Capacity> result{};
  for (uint32_t row = end_row; row < height && result.count < Capacity; ++row) {
    if (!row_missing[row]) {
      result.rows[result.count++] = row;
    }
  }
  return result;
}

[[nodiscard]] inline __m256 rsqrt_nr(__m256 value) {
  const __m256 half = _mm256_set1_ps(0.5f);
  const __m256 three_halves = _mm256_set1_ps(1.5f);
  __m256 estimate = _mm256_rsqrt_ps(value);
  estimate = _mm256_mul_ps(
      estimate,
      _mm256_sub_ps(three_halves,
                    _mm256_mul_ps(half,
                                  _mm256_mul_ps(value,
                                                _mm256_mul_ps(estimate,
                                                              estimate)))));
  return estimate;
}

[[nodiscard]] inline float rsqrt_nr_scalar(float value) {
  return 1.0f / std::sqrt(value);
}

void concealRowAnalyticLerpScalar(float *dst, const float *prev_real,
                                  const float *next_real,
                                  const float *prev_imag,
                                  const float *next_imag, uint32_t width,
                                  float alpha) {
  constexpr float Eps = 1e-12f;
  for (uint32_t x = 0; x < width; ++x) {
    const float real0 = prev_real[x];
    const float imag0 = prev_imag[x];
    const float real1 = next_real[x];
    const float imag1 = next_imag[x];

    const float amp0_sq = real0 * real0 + imag0 * imag0 + Eps;
    const float amp1_sq = real1 * real1 + imag1 * imag1 + Eps;
    const float inv_amp0 = rsqrt_nr_scalar(amp0_sq);
    const float inv_amp1 = rsqrt_nr_scalar(amp1_sq);
    const float amp0 = amp0_sq * inv_amp0;
    const float amp1 = amp1_sq * inv_amp1;

    const float unit0_real = real0 * inv_amp0;
    const float unit0_imag = imag0 * inv_amp0;
    const float unit1_real = real1 * inv_amp1;
    const float unit1_imag = imag1 * inv_amp1;

    const float blend_real = unit0_real + alpha * (unit1_real - unit0_real);
    const float blend_imag = unit0_imag + alpha * (unit1_imag - unit0_imag);
    const float blend_norm_sq =
        blend_real * blend_real + blend_imag * blend_imag + Eps;
    const float inv_blend_norm = rsqrt_nr_scalar(blend_norm_sq);

    const float amplitude = amp0 + alpha * (amp1 - amp0);
    dst[x] = std::clamp(amplitude * blend_real * inv_blend_norm, 0.0f, 1.0f);
  }
}

void concealRowAnalyticLerp(float *dst, const float *prev_real,
                            const float *next_real, const float *prev_imag,
                            const float *next_imag, uint32_t width,
                            float alpha) {
  constexpr float Eps = 1e-12f;
  const __m256 alpha_v = _mm256_set1_ps(alpha);
  const __m256 eps_v = _mm256_set1_ps(Eps);
  const __m256 zero = _mm256_setzero_ps();
  const __m256 one = _mm256_set1_ps(1.0f);

  uint32_t x = 0;
  for (; x + 7 < width; x += 8) {
    const __m256 real0 = _mm256_loadu_ps(prev_real + x);
    const __m256 imag0 = _mm256_loadu_ps(prev_imag + x);
    const __m256 real1 = _mm256_loadu_ps(next_real + x);
    const __m256 imag1 = _mm256_loadu_ps(next_imag + x);

    const __m256 amp0_sq =
        _mm256_add_ps(_mm256_fmadd_ps(real0, real0, _mm256_mul_ps(imag0, imag0)),
                      eps_v);
    const __m256 amp1_sq =
        _mm256_add_ps(_mm256_fmadd_ps(real1, real1, _mm256_mul_ps(imag1, imag1)),
                      eps_v);

    const __m256 inv_amp0 = rsqrt_nr(amp0_sq);
    const __m256 inv_amp1 = rsqrt_nr(amp1_sq);
    const __m256 amp0 = _mm256_mul_ps(amp0_sq, inv_amp0);
    const __m256 amp1 = _mm256_mul_ps(amp1_sq, inv_amp1);

    const __m256 unit0_real = _mm256_mul_ps(real0, inv_amp0);
    const __m256 unit0_imag = _mm256_mul_ps(imag0, inv_amp0);
    const __m256 unit1_real = _mm256_mul_ps(real1, inv_amp1);
    const __m256 unit1_imag = _mm256_mul_ps(imag1, inv_amp1);

    const __m256 blend_real = _mm256_fmadd_ps(
        alpha_v, _mm256_sub_ps(unit1_real, unit0_real), unit0_real);
    const __m256 blend_imag = _mm256_fmadd_ps(
        alpha_v, _mm256_sub_ps(unit1_imag, unit0_imag), unit0_imag);
    const __m256 blend_norm_sq = _mm256_add_ps(
        _mm256_fmadd_ps(blend_real, blend_real,
                        _mm256_mul_ps(blend_imag, blend_imag)),
        eps_v);
    const __m256 inv_blend_norm = rsqrt_nr(blend_norm_sq);

    const __m256 amplitude =
        _mm256_fmadd_ps(alpha_v, _mm256_sub_ps(amp1, amp0), amp0);
    __m256 result =
        _mm256_mul_ps(amplitude, _mm256_mul_ps(blend_real, inv_blend_norm));
    result = _mm256_max_ps(result, zero);
    result = _mm256_min_ps(result, one);
    _mm256_storeu_ps(dst + x, result);
  }

  concealRowAnalyticLerpScalar(dst + x, prev_real + x, next_real + x,
                               prev_imag + x, next_imag + x, width - x,
                               alpha);
}

void blendBoundaryProfile(const float *pixels, uint32_t width,
                          uint32_t anchor_row,
                          const NeighborRows<BoundarySupportRows> &neighbors,
                          std::vector<float> &blended_real,
                          std::vector<float> &blended_imag,
                          std::vector<float> &scratch_imag) {
  std::fill(blended_real.begin(), blended_real.end(), 0.0f);
  std::fill(blended_imag.begin(), blended_imag.end(), 0.0f);

  if (neighbors.count == 0) {
    const float *row_ptr = pixels + static_cast<size_t>(anchor_row) * width;
    std::copy_n(row_ptr, width, blended_real.data());
    computeHilbertImag1D(row_ptr, width, blended_imag.data());
    return;
  }

  float total_weight = 0.0f;
  for (uint32_t index = 0; index < neighbors.count; ++index) {
    const uint32_t row = neighbors.rows[index];
    const uint32_t distance =
        anchor_row > row ? (anchor_row - row) : (row - anchor_row);
    const float weight = 1.0f / static_cast<float>(distance + 1u);
    const float *row_ptr = pixels + static_cast<size_t>(row) * width;
    computeHilbertImag1D(row_ptr, width, scratch_imag.data());
    for (uint32_t x = 0; x < width; ++x) {
      blended_real[x] += weight * row_ptr[x];
      blended_imag[x] += weight * scratch_imag[x];
    }
    total_weight += weight;
  }

  const float inv_total_weight = 1.0f / total_weight;
  for (uint32_t x = 0; x < width; ++x) {
    blended_real[x] *= inv_total_weight;
    blended_imag[x] *= inv_total_weight;
  }
}

} // namespace

void computeHilbertImag1D(const float *input_row, uint32_t width,
                          float *imag_out) {
  if (input_row == nullptr || imag_out == nullptr || width == 0) {
    return;
  }

  if (width <= 2 * static_cast<uint32_t>(HILBERT_OFFSETS.back()) + 8) {
    computeHilbertImagScalar(input_row, width, imag_out, 0, width);
    return;
  }

  const uint32_t edge = static_cast<uint32_t>(HILBERT_OFFSETS.back());
  computeHilbertImagScalar(input_row, width, imag_out, 0, edge);

  const __m256 c1 = _mm256_set1_ps(HILBERT_COEFFS[0]);
  const __m256 c3 = _mm256_set1_ps(HILBERT_COEFFS[1]);
  const __m256 c5 = _mm256_set1_ps(HILBERT_COEFFS[2]);

  uint32_t x = edge;
  for (; x + 7 < width - edge; x += 8) {
    const __m256 left1 = _mm256_loadu_ps(input_row + x - 1);
    const __m256 right1 = _mm256_loadu_ps(input_row + x + 1);
    const __m256 left3 = _mm256_loadu_ps(input_row + x - 3);
    const __m256 right3 = _mm256_loadu_ps(input_row + x + 3);
    const __m256 left5 = _mm256_loadu_ps(input_row + x - 5);
    const __m256 right5 = _mm256_loadu_ps(input_row + x + 5);

    __m256 imag = _mm256_mul_ps(_mm256_sub_ps(right1, left1), c1);
    imag = _mm256_fmadd_ps(_mm256_sub_ps(right3, left3), c3, imag);
    imag = _mm256_fmadd_ps(_mm256_sub_ps(right5, left5), c5, imag);
    _mm256_storeu_ps(imag_out + x, imag);
  }

  computeHilbertImagScalar(input_row, width, imag_out, x, width);
}

ConcealmentReport
concealMissingRowsWithAnalyticContinuation(float *pixels, uint32_t width,
                                           uint32_t height,
                                           const std::vector<ConcealmentSpan> &spans,
                                           ConcealmentWorkspace &workspace,
                                           bool measure_timing) {
  ConcealmentReport report{};
  if (pixels == nullptr || width == 0 || height == 0 || spans.empty()) {
    return report;
  }

  workspace.ensureCapacity(width, height);
  std::fill(workspace.row_missing.begin(), workspace.row_missing.end(),
            static_cast<uint8_t>(0));
  workspace.resetCache();

  const auto start = measure_timing ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
  for (const ConcealmentSpan span : spans) {
    const uint32_t end_row = std::min(height, span.start_row + span.row_count);
    for (uint32_t row = span.start_row; row < end_row; ++row) {
      workspace.row_missing[row] = 1;
    }
  }

  for (const ConcealmentSpan span : spans) {
    if (span.row_count == 0 || span.start_row >= height) {
      continue;
    }

    const uint32_t end_row = std::min(height, span.start_row + span.row_count);
    const NeighborRows<BoundarySupportRows> previous_rows =
        collect_previous_valid_rows<BoundarySupportRows>(span.start_row,
                                                         workspace.row_missing);
    const NeighborRows<BoundarySupportRows> next_rows =
        collect_next_valid_rows<BoundarySupportRows>(end_row, height,
                                                     workspace.row_missing);
    const uint32_t fallback_prev_row =
        find_previous_valid_row(span.start_row, workspace.row_missing);
    const uint32_t fallback_next_row =
        find_next_valid_row(end_row, height, workspace.row_missing);

    blendBoundaryProfile(pixels, width, fallback_prev_row, previous_rows,
                         workspace.prev_real, workspace.prev_imag,
                         workspace.scratch_imag);
    blendBoundaryProfile(pixels, width, fallback_next_row, next_rows,
                         workspace.next_real, workspace.next_imag,
                         workspace.scratch_imag);

    for (uint32_t row = span.start_row; row < end_row; ++row) {
      const float alpha = (end_row == span.start_row + 1)
                              ? 0.5f
                              : static_cast<float>(row - span.start_row + 1) /
                                    static_cast<float>(span.row_count + 1);
      float *dst = pixels + static_cast<size_t>(row) * width;
      concealRowAnalyticLerp(dst, workspace.prev_real.data(),
                             workspace.next_real.data(),
                             workspace.prev_imag.data(),
                             workspace.next_imag.data(), width, alpha);
    }

    report.healed_rows += (end_row - span.start_row);
  }

  if (measure_timing) {
    report.elapsed_ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start)
                            .count();
  }
  return report;
}

ConcealmentReport
concealMissingRowsWithAnalyticContinuation(float *pixels, uint32_t width,
                                           uint32_t height,
                                           const std::vector<ConcealmentSpan> &spans) {
  ConcealmentWorkspace workspace(width, height);
  return concealMissingRowsWithAnalyticContinuation(
      pixels, width, height, spans, workspace, true);
}

} // namespace NetDSP
