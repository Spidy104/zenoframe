#include "ImageEngine.hpp"
#include <cmath>
#include <immintrin.h>
#include <omp.h>

namespace dsp {

// ============================================================================
// AVX2 OPTIMIZED GAUSSIAN BLUR
// ============================================================================

void gaussianBlurAVX2Into(const Image &input, Image &output) {
  const uint32_t width = input.width();
  const uint32_t height = input.height();
  if (output.width() != width || output.height() != height) {
    output = Image(width, height);
  }

  const float *__restrict__ in = input.data();
  float *__restrict__ out = output.data();

  const __m256 k0 = _mm256_set1_ps(1.0f / 16.0f);
  const __m256 k1 = _mm256_set1_ps(2.0f / 16.0f);
  const __m256 k2 = _mm256_set1_ps(4.0f / 16.0f);
  const __m256 zero = _mm256_setzero_ps();
  const __m256 one = _mm256_set1_ps(1.0f);

#pragma omp parallel for schedule(static) default(none)                        \
    shared(in, out, width, height, k0, k1, k2, zero, one)
  for (uint32_t y = 1; y < height - 1; ++y) {
    const uint32_t row_prev = (y - 1) * width;
    const uint32_t row_curr = y * width;
    const uint32_t row_next = (y + 1) * width;

    uint32_t x = 1;
    for (; x + 7 < width - 1; x += 8) {
      __m256 r0_left = _mm256_loadu_ps(&in[row_prev + x - 1]);
      __m256 r0_center = _mm256_loadu_ps(&in[row_prev + x]);
      __m256 r0_right = _mm256_loadu_ps(&in[row_prev + x + 1]);

      __m256 r1_left = _mm256_loadu_ps(&in[row_curr + x - 1]);
      __m256 r1_center = _mm256_loadu_ps(&in[row_curr + x]);
      __m256 r1_right = _mm256_loadu_ps(&in[row_curr + x + 1]);

      __m256 r2_left = _mm256_loadu_ps(&in[row_next + x - 1]);
      __m256 r2_center = _mm256_loadu_ps(&in[row_next + x]);
      __m256 r2_right = _mm256_loadu_ps(&in[row_next + x + 1]);

      __m256 sum = _mm256_mul_ps(r0_left, k0);
      sum = _mm256_fmadd_ps(r0_center, k1, sum);
      sum = _mm256_fmadd_ps(r0_right, k0, sum);
      sum = _mm256_fmadd_ps(r1_left, k1, sum);
      sum = _mm256_fmadd_ps(r1_center, k2, sum);
      sum = _mm256_fmadd_ps(r1_right, k1, sum);
      sum = _mm256_fmadd_ps(r2_left, k0, sum);
      sum = _mm256_fmadd_ps(r2_center, k1, sum);
      sum = _mm256_fmadd_ps(r2_right, k0, sum);

      sum = _mm256_max_ps(sum, zero);
      sum = _mm256_min_ps(sum, one);
      _mm256_storeu_ps(&out[row_curr + x], sum);
    }

    // Scalar remainder
    for (; x < width - 1; ++x) {
      float sum = in[row_prev + x - 1] * (1.0f / 16.0f);
      sum += in[row_prev + x] * (2.0f / 16.0f);
      sum += in[row_prev + x + 1] * (1.0f / 16.0f);
      sum += in[row_curr + x - 1] * (2.0f / 16.0f);
      sum += in[row_curr + x] * (4.0f / 16.0f);
      sum += in[row_curr + x + 1] * (2.0f / 16.0f);
      sum += in[row_next + x - 1] * (1.0f / 16.0f);
      sum += in[row_next + x] * (2.0f / 16.0f);
      sum += in[row_next + x + 1] * (1.0f / 16.0f);
      sum = (sum < 0.0f) ? 0.0f : sum;
      sum = (sum > 1.0f) ? 1.0f : sum;
      out[row_curr + x] = sum;
    }
  }

  for (uint32_t x = 0; x < width; ++x) {
    output.at(x, 0) = input.at(x, 0);
    output.at(x, height - 1) = input.at(x, height - 1);
  }
  for (uint32_t y = 0; y < height; ++y) {
    output.at(0, y) = input.at(0, y);
    output.at(width - 1, y) = input.at(width - 1, y);
  }
}

Image gaussianBlurAVX2(const Image &input) {
  Image output(input.width(), input.height());
  gaussianBlurAVX2Into(input, output);
  return output;
}

// ============================================================================
// AVX2 OPTIMIZED SOBEL MAGNITUDE
// ============================================================================

void sobelMagnitudeAVX2Into(const Image &input, Image &output) {
  const uint32_t width = input.width();
  const uint32_t height = input.height();
  if (output.width() != width || output.height() != height) {
    output = Image(width, height);
  }

  const float *__restrict__ in = input.data();
  float *__restrict__ out = output.data();

  const __m256 w1 = _mm256_set1_ps(1.0f);
  const __m256 w2 = _mm256_set1_ps(2.0f);
  const __m256 zero = _mm256_setzero_ps();
  const __m256 one = _mm256_set1_ps(1.0f);

#pragma omp parallel for schedule(static) default(none)                        \
    shared(in, out, width, height, w1, w2, zero, one)
  for (uint32_t y = 1; y < height - 1; ++y) {
    const uint32_t row_prev = (y - 1) * width;
    const uint32_t row_curr = y * width;
    const uint32_t row_next = (y + 1) * width;

    uint32_t x = 1;
    for (; x + 7 < width - 1; x += 8) {
      __m256 r0_left = _mm256_loadu_ps(&in[row_prev + x - 1]);
      __m256 r0_center = _mm256_loadu_ps(&in[row_prev + x]);
      __m256 r0_right = _mm256_loadu_ps(&in[row_prev + x + 1]);
      __m256 r1_left = _mm256_loadu_ps(&in[row_curr + x - 1]);
      __m256 r1_right = _mm256_loadu_ps(&in[row_curr + x + 1]);
      __m256 r2_left = _mm256_loadu_ps(&in[row_next + x - 1]);
      __m256 r2_center = _mm256_loadu_ps(&in[row_next + x]);
      __m256 r2_right = _mm256_loadu_ps(&in[row_next + x + 1]);

      __m256 gx = _mm256_sub_ps(r0_right, r0_left);
      gx = _mm256_fmadd_ps(_mm256_sub_ps(r1_right, r1_left), w2, gx);
      gx = _mm256_add_ps(gx, _mm256_sub_ps(r2_right, r2_left));

      __m256 gy = _mm256_sub_ps(r2_left, r0_left);
      gy = _mm256_fmadd_ps(_mm256_sub_ps(r2_center, r0_center), w2, gy);
      gy = _mm256_add_ps(gy, _mm256_sub_ps(r2_right, r0_right));

      __m256 gx2 = _mm256_mul_ps(gx, gx);
      __m256 gy2 = _mm256_mul_ps(gy, gy);
      __m256 mag = _mm256_sqrt_ps(_mm256_add_ps(gx2, gy2));

      mag = _mm256_max_ps(mag, zero);
      mag = _mm256_min_ps(mag, one);
      _mm256_storeu_ps(&out[row_curr + x], mag);
    }

    // Scalar remainder
    for (; x < width - 1; ++x) {
      float gx = -in[row_prev + x - 1] + in[row_prev + x + 1] -
                 2.0f * in[row_curr + x - 1] + 2.0f * in[row_curr + x + 1] -
                 in[row_next + x - 1] + in[row_next + x + 1];

      float gy = -in[row_prev + x - 1] - 2.0f * in[row_prev + x] -
                 in[row_prev + x + 1] + in[row_next + x - 1] +
                 2.0f * in[row_next + x] + in[row_next + x + 1];

      float mag = std::hypot(gx, gy);
      mag = (mag < 0.0f) ? 0.0f : mag;
      mag = (mag > 1.0f) ? 1.0f : mag;
      out[row_curr + x] = mag;
    }
  }

  for (uint32_t x = 0; x < width; ++x) {
    output.at(x, 0) = 0.0f;
    output.at(x, height - 1) = 0.0f;
  }
  for (uint32_t y = 0; y < height; ++y) {
    output.at(0, y) = 0.0f;
    output.at(width - 1, y) = 0.0f;
  }
}

Image sobelMagnitudeAVX2(const Image &input) {
  Image output(input.width(), input.height());
  sobelMagnitudeAVX2Into(input, output);
  return output;
}

// ============================================================================
// SEPARABLE 1D CONVOLUTION
// ============================================================================

Image convolve1D_horizontal(const Image &input,
                            const std::array<float, 3> &kernel,
                            float normalization) {
  const uint32_t width = input.width();
  const uint32_t height = input.height();
  Image output(width, height);

  const float k0 = kernel[0], k1 = kernel[1], k2 = kernel[2];
  const float inv_norm = 1.0f / normalization;
  const float *__restrict__ in_data = input.data();
  float *__restrict__ out_data = output.data();

#pragma omp parallel for schedule(static) shared(                              \
        out_data, in_data, width, height, k0, k1, k2, inv_norm, input, output)
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t row = y * width;
    for (uint32_t x = 1; x < width - 1; ++x) {
      float sum = in_data[row + (x - 1)] * k0;
      sum += in_data[row + x] * k1;
      sum += in_data[row + (x + 1)] * k2;
      sum *= inv_norm;
      sum = (sum < 0.0f) ? 0.0f : sum;
      sum = (sum > 1.0f) ? 1.0f : sum;
      out_data[row + x] = sum;
    }
    output.at(0, y) = input.at(0, y);
    output.at(width - 1, y) = input.at(width - 1, y);
  }
  return output;
}

Image convolve1D_vertical(const Image &input,
                          const std::array<float, 3> &kernel,
                          float normalization) {
  const uint32_t width = input.width();
  const uint32_t height = input.height();
  Image output(width, height);

  const float k0 = kernel[0], k1 = kernel[1], k2 = kernel[2];
  const float inv_norm = 1.0f / normalization;
  const float *__restrict__ in_data = input.data();
  float *__restrict__ out_data = output.data();

#pragma omp parallel for schedule(static) shared(                              \
        out_data, in_data, width, height, k0, k1, k2, inv_norm, input, output)
  for (uint32_t y = 1; y < height - 1; ++y) {
    const uint32_t row_prev = (y - 1) * width;
    const uint32_t row_curr = y * width;
    const uint32_t row_next = (y + 1) * width;
    for (uint32_t x = 0; x < width; ++x) {
      float sum = in_data[row_prev + x] * k0;
      sum += in_data[row_curr + x] * k1;
      sum += in_data[row_next + x] * k2;
      sum *= inv_norm;
      sum = (sum < 0.0f) ? 0.0f : sum;
      sum = (sum > 1.0f) ? 1.0f : sum;
      out_data[row_curr + x] = sum;
    }
  }
  for (uint32_t x = 0; x < width; ++x) {
    output.at(x, 0) = input.at(x, 0);
    output.at(x, height - 1) = input.at(x, height - 1);
  }
  return output;
}

Image gaussianBlurSeparable(const Image &input) {
  Image temp = convolve1D_horizontal(input, GAUSSIAN_1D, GAUSSIAN_1D_NORM);
  return convolve1D_vertical(temp, GAUSSIAN_1D, GAUSSIAN_1D_NORM);
}

Image sobelMagnitudeFused(const Image &input) {
  const uint32_t width = input.width();
  const uint32_t height = input.height();
  Image output(width, height);

  const float *__restrict__ in_data = input.data();
  float *__restrict__ out_data = output.data();

#pragma omp parallel for schedule(static)                                      \
    shared(out_data, in_data, width, height)
  for (uint32_t y = 1; y < height - 1; ++y) {
    const uint32_t row_prev = (y - 1) * width;
    const uint32_t row_curr = y * width;
    const uint32_t row_next = (y + 1) * width;

    for (uint32_t x = 1; x < width - 1; ++x) {
      float gx = -in_data[row_prev + (x - 1)] + in_data[row_prev + (x + 1)] -
                 2.0f * in_data[row_curr + (x - 1)] +
                 2.0f * in_data[row_curr + (x + 1)] -
                 in_data[row_next + (x - 1)] + in_data[row_next + (x + 1)];

      float gy = -in_data[row_prev + (x - 1)] - 2.0f * in_data[row_prev + x] -
                 in_data[row_prev + (x + 1)] + in_data[row_next + (x - 1)] +
                 2.0f * in_data[row_next + x] + in_data[row_next + (x + 1)];

      float magnitude = std::hypot(gx, gy);
      magnitude = (magnitude < 0.0f) ? 0.0f : magnitude;
      magnitude = (magnitude > 1.0f) ? 1.0f : magnitude;
      out_data[row_curr + x] = magnitude;
    }
  }

  for (uint32_t x = 0; x < width; ++x) {
    output.at(x, 0) = 0.0f;
    output.at(x, height - 1) = 0.0f;
  }
  for (uint32_t y = 0; y < height; ++y) {
    output.at(0, y) = 0.0f;
    output.at(width - 1, y) = 0.0f;
  }
  return output;
}

// ============================================================================
// STANDARD 3x3 CONVOLUTION
// ============================================================================

Image convolve3x3(const Image &input,
                  const std::array<std::array<float, 3>, 3> &kernel,
                  float normalization) {
  const uint32_t width = input.width();
  const uint32_t height = input.height();
  Image output(width, height);

  const float k00 = kernel[0][0], k01 = kernel[0][1], k02 = kernel[0][2];
  const float k10 = kernel[1][0], k11 = kernel[1][1], k12 = kernel[1][2];
  const float k20 = kernel[2][0], k21 = kernel[2][1], k22 = kernel[2][2];
  const float inv_norm = 1.0f / normalization;

  const float *__restrict__ in_data = input.data();
  float *__restrict__ out_data = output.data();

#pragma omp parallel for schedule(static)                                      \
    shared(out_data, in_data, width, height, k00, k01, k02, k10, k11, k12,     \
               k20, k21, k22, inv_norm)
  for (uint32_t y = 1; y < height - 1; ++y) {
    const uint32_t row_prev = (y - 1) * width;
    const uint32_t row_curr = y * width;
    const uint32_t row_next = (y + 1) * width;

    for (uint32_t x = 1; x < width - 1; ++x) {
      float sum = in_data[row_prev + (x - 1)] * k00;
      sum += in_data[row_prev + x] * k01;
      sum += in_data[row_prev + (x + 1)] * k02;
      sum += in_data[row_curr + (x - 1)] * k10;
      sum += in_data[row_curr + x] * k11;
      sum += in_data[row_curr + (x + 1)] * k12;
      sum += in_data[row_next + (x - 1)] * k20;
      sum += in_data[row_next + x] * k21;
      sum += in_data[row_next + (x + 1)] * k22;
      sum *= inv_norm;
      sum = (sum < 0.0f) ? 0.0f : sum;
      sum = (sum > 1.0f) ? 1.0f : sum;
      out_data[row_curr + x] = sum;
    }
  }

  for (uint32_t x = 0; x < width; ++x) {
    output.at(x, 0) = input.at(x, 0);
    output.at(x, height - 1) = input.at(x, height - 1);
  }
  for (uint32_t y = 0; y < height; ++y) {
    output.at(0, y) = input.at(0, y);
    output.at(width - 1, y) = input.at(width - 1, y);
  }
  return output;
}

Image gaussianBlur(const Image &input) { return gaussianBlurAVX2(input); }

void gaussianBlurInto(const Image &input, Image &output) {
  gaussianBlurAVX2Into(input, output);
}

Image sobelEdges(const Image &input) {
  return convolve3x3(input, SOBEL_X_KERNEL, 1.0f);
}

Image sobelMagnitude(const Image &input) { return sobelMagnitudeAVX2(input); }

void sobelMagnitudeInto(const Image &input, Image &output) {
  sobelMagnitudeAVX2Into(input, output);
}

// ============================================================================
// FUSED BLUR+SOBEL (5x5 Pre-Convolved Kernels)
// ============================================================================

void blurAndSobelFusedInto(const Image &input, Image &output) {
  const uint32_t width = input.width();
  const uint32_t height = input.height();
  if (output.width() != width || output.height() != height) {
    output = Image(width, height);
  }

  const float *__restrict__ in = input.data();
  float *__restrict__ out = output.data();
  const float inv_norm = 1.0f / FUSED_NORM;
  const __m256 zero = _mm256_setzero_ps();
  const __m256 one = _mm256_set1_ps(1.0f);
  const __m256 inv = _mm256_set1_ps(inv_norm);
  const __m256 gx1 = _mm256_set1_ps(1.0f);
  const __m256 gx2 = _mm256_set1_ps(2.0f);
  const __m256 gx4 = _mm256_set1_ps(4.0f);
  const __m256 gx6 = _mm256_set1_ps(6.0f);
  const __m256 gx8 = _mm256_set1_ps(8.0f);
  const __m256 gx12 = _mm256_set1_ps(12.0f);

#pragma omp parallel for schedule(static)                                      \
    shared(in, out, width, height, inv_norm, zero, one, inv, gx1, gx2, gx4,    \
               gx6, gx8, gx12)
  for (uint32_t y = 2; y < height - 2; ++y) {
    const uint32_t r0 = (y - 2) * width;
    const uint32_t r1 = (y - 1) * width;
    const uint32_t r2 = y * width;
    const uint32_t r3 = (y + 1) * width;
    const uint32_t r4 = (y + 2) * width;

    uint32_t x = 2;
    for (; x + 7 < width - 2; x += 8) {
      __m256 gx = zero;
      gx = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r0 + x - 2]), gx1, gx);
      gx = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r0 + x - 1]), gx2, gx);
      gx = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r0 + x + 1]), gx2, gx);
      gx = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r0 + x + 2]), gx1, gx);
      gx = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r1 + x - 2]), gx4, gx);
      gx = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r1 + x - 1]), gx8, gx);
      gx = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r1 + x + 1]), gx8, gx);
      gx = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r1 + x + 2]), gx4, gx);
      gx = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r2 + x - 2]), gx6, gx);
      gx = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r2 + x - 1]), gx12, gx);
      gx = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r2 + x + 1]), gx12, gx);
      gx = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r2 + x + 2]), gx6, gx);
      gx = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r3 + x - 2]), gx4, gx);
      gx = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r3 + x - 1]), gx8, gx);
      gx = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r3 + x + 1]), gx8, gx);
      gx = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r3 + x + 2]), gx4, gx);
      gx = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r4 + x - 2]), gx1, gx);
      gx = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r4 + x - 1]), gx2, gx);
      gx = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r4 + x + 1]), gx2, gx);
      gx = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r4 + x + 2]), gx1, gx);

      __m256 gy = zero;
      gy = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r0 + x - 2]), gx1, gy);
      gy = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r0 + x - 1]), gx4, gy);
      gy = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r0 + x]), gx6, gy);
      gy = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r0 + x + 1]), gx4, gy);
      gy = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r0 + x + 2]), gx1, gy);
      gy = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r1 + x - 2]), gx2, gy);
      gy = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r1 + x - 1]), gx8, gy);
      gy = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r1 + x]), gx12, gy);
      gy = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r1 + x + 1]), gx8, gy);
      gy = _mm256_fnmadd_ps(_mm256_loadu_ps(&in[r1 + x + 2]), gx2, gy);
      gy = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r3 + x - 2]), gx2, gy);
      gy = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r3 + x - 1]), gx8, gy);
      gy = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r3 + x]), gx12, gy);
      gy = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r3 + x + 1]), gx8, gy);
      gy = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r3 + x + 2]), gx2, gy);
      gy = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r4 + x - 2]), gx1, gy);
      gy = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r4 + x - 1]), gx4, gy);
      gy = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r4 + x]), gx6, gy);
      gy = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r4 + x + 1]), gx4, gy);
      gy = _mm256_fmadd_ps(_mm256_loadu_ps(&in[r4 + x + 2]), gx1, gy);

      gx = _mm256_mul_ps(gx, inv);
      gy = _mm256_mul_ps(gy, inv);
      __m256 mag = _mm256_sqrt_ps(
          _mm256_fmadd_ps(gx, gx, _mm256_mul_ps(gy, gy)));
      mag = _mm256_max_ps(mag, zero);
      mag = _mm256_min_ps(mag, one);
      _mm256_storeu_ps(&out[r2 + x], mag);
    }

    for (; x < width - 2; ++x) {
      float gx =
          in[r0 + x - 2] * FUSED_GX[0][0] + in[r0 + x - 1] * FUSED_GX[0][1] +
          in[r0 + x + 1] * FUSED_GX[0][3] + in[r0 + x + 2] * FUSED_GX[0][4] +
          in[r1 + x - 2] * FUSED_GX[1][0] + in[r1 + x - 1] * FUSED_GX[1][1] +
          in[r1 + x + 1] * FUSED_GX[1][3] + in[r1 + x + 2] * FUSED_GX[1][4] +
          in[r2 + x - 2] * FUSED_GX[2][0] + in[r2 + x - 1] * FUSED_GX[2][1] +
          in[r2 + x + 1] * FUSED_GX[2][3] + in[r2 + x + 2] * FUSED_GX[2][4] +
          in[r3 + x - 2] * FUSED_GX[3][0] + in[r3 + x - 1] * FUSED_GX[3][1] +
          in[r3 + x + 1] * FUSED_GX[3][3] + in[r3 + x + 2] * FUSED_GX[3][4] +
          in[r4 + x - 2] * FUSED_GX[4][0] + in[r4 + x - 1] * FUSED_GX[4][1] +
          in[r4 + x + 1] * FUSED_GX[4][3] + in[r4 + x + 2] * FUSED_GX[4][4];

      float gy =
          in[r0 + x - 2] * FUSED_GY[0][0] + in[r0 + x - 1] * FUSED_GY[0][1] +
          in[r0 + x] * FUSED_GY[0][2] + in[r0 + x + 1] * FUSED_GY[0][3] +
          in[r0 + x + 2] * FUSED_GY[0][4] + in[r1 + x - 2] * FUSED_GY[1][0] +
          in[r1 + x - 1] * FUSED_GY[1][1] + in[r1 + x] * FUSED_GY[1][2] +
          in[r1 + x + 1] * FUSED_GY[1][3] + in[r1 + x + 2] * FUSED_GY[1][4] +
          in[r3 + x - 2] * FUSED_GY[3][0] + in[r3 + x - 1] * FUSED_GY[3][1] +
          in[r3 + x] * FUSED_GY[3][2] + in[r3 + x + 1] * FUSED_GY[3][3] +
          in[r3 + x + 2] * FUSED_GY[3][4] + in[r4 + x - 2] * FUSED_GY[4][0] +
          in[r4 + x - 1] * FUSED_GY[4][1] + in[r4 + x] * FUSED_GY[4][2] +
          in[r4 + x + 1] * FUSED_GY[4][3] + in[r4 + x + 2] * FUSED_GY[4][4];

      gx *= inv_norm;
      gy *= inv_norm;
      float mag = std::sqrt(gx * gx + gy * gy);
      mag = (mag > 1.0f) ? 1.0f : mag;
      out[r2 + x] = mag;
    }
  }

  for (uint32_t x = 0; x < width; ++x) {
    out[x] = 0.0f;
    out[width + x] = 0.0f;
    out[(height - 2) * width + x] = 0.0f;
    out[(height - 1) * width + x] = 0.0f;
  }
  for (uint32_t y = 0; y < height; ++y) {
    out[y * width] = 0.0f;
    out[y * width + 1] = 0.0f;
    out[y * width + width - 2] = 0.0f;
    out[y * width + width - 1] = 0.0f;
  }
}

Image blurAndSobelFused(const Image &input) {
  Image output(input.width(), input.height());
  blurAndSobelFusedInto(input, output);
  return output;
}

} // namespace dsp
