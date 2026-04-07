#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace dsp {

// ============================================================================
// CONVOLUTION KERNELS
// ============================================================================

// Gaussian 1D kernel for separable convolution
constexpr std::array<float, 3> GAUSSIAN_1D = {1.0f, 2.0f, 1.0f};
constexpr float GAUSSIAN_1D_NORM = 4.0f;

// Gaussian 3x3 kernel
constexpr std::array<std::array<float, 3>, 3> GAUSSIAN_KERNEL = {
    {{1.0f, 2.0f, 1.0f}, {2.0f, 4.0f, 2.0f}, {1.0f, 2.0f, 1.0f}}};
constexpr float GAUSSIAN_NORM = 16.0f;

// Sobel X kernel (vertical edge detection)
constexpr std::array<std::array<float, 3>, 3> SOBEL_X_KERNEL = {
    {{-1.0f, 0.0f, 1.0f}, {-2.0f, 0.0f, 2.0f}, {-1.0f, 0.0f, 1.0f}}};

// Sobel Y kernel (horizontal edge detection)
constexpr std::array<std::array<float, 3>, 3> SOBEL_Y_KERNEL = {
    {{-1.0f, -2.0f, -1.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 2.0f, 1.0f}}};

// Pre-convolved 5x5 Gaussian⊗Sobel kernels for fused blur+edge detection
constexpr float FUSED_GX[5][5] = {{-1, -2, 0, 2, 1},
                                  {-4, -8, 0, 8, 4},
                                  {-6, -12, 0, 12, 6},
                                  {-4, -8, 0, 8, 4},
                                  {-1, -2, 0, 2, 1}};

constexpr float FUSED_GY[5][5] = {{-1, -4, -6, -4, -1},
                                  {-2, -8, -12, -8, -2},
                                  {0, 0, 0, 0, 0},
                                  {2, 8, 12, 8, 2},
                                  {1, 4, 6, 4, 1}};
constexpr float FUSED_NORM = 16.0f;

// ============================================================================
// IMAGE CLASS
// ============================================================================

class Image {
private:
  uint32_t width_;
  uint32_t height_;
  alignas(64) std::vector<float> data_; // Cache-aligned storage

public:
  Image(uint32_t width, uint32_t height)
      : width_(width), height_(height), data_(width * height, 0.0f) {}

  Image() : width_(0), height_(0) {}

  // Pixel access (row-major)
  [[nodiscard]] inline float &at(uint32_t x, uint32_t y) {
    return data_[y * width_ + x];
  }

  [[nodiscard]] inline const float &at(uint32_t x, uint32_t y) const {
    return data_[y * width_ + x];
  }

  [[nodiscard]] inline float &at_checked(uint32_t x, uint32_t y) {
    if (x >= width_ || y >= height_) {
      throw std::out_of_range("Pixel coordinates out of bounds");
    }
    return at(x, y);
  }

  // Direct memory access
  [[nodiscard]] inline float *data() { return data_.data(); }
  [[nodiscard]] inline const float *data() const { return data_.data(); }
  [[nodiscard]] inline size_t size() const { return data_.size(); }

  // Dimensions
  [[nodiscard]] inline uint32_t width() const { return width_; }
  [[nodiscard]] inline uint32_t height() const { return height_; }

  // Utilities
  void fill(float value) { std::fill(data_.begin(), data_.end(), value); }

  void clamp() {
    for (auto &pixel : data_) {
      pixel = std::clamp(pixel, 0.0f, 1.0f);
    }
  }
};

// ============================================================================
// CONVOLUTION OPERATIONS
// ============================================================================

// 3x3 convolution
Image convolve3x3(const Image &input,
                  const std::array<std::array<float, 3>, 3> &kernel,
                  float normalization = 1.0f);

// 1D separable convolution
Image convolve1D_horizontal(const Image &input,
                            const std::array<float, 3> &kernel,
                            float normalization);

Image convolve1D_vertical(const Image &input,
                          const std::array<float, 3> &kernel,
                          float normalization);

// Gaussian blur
Image gaussianBlur(const Image &input);
void gaussianBlurInto(const Image &input, Image &output);
Image gaussianBlurSeparable(const Image &input);

// Sobel edge detection
Image sobelEdges(const Image &input);
Image sobelMagnitude(const Image &input);
void sobelMagnitudeInto(const Image &input, Image &output);
Image sobelMagnitudeFused(const Image &input);

// Fused blur+sobel pipeline (single 5x5 pass)
Image blurAndSobelFused(const Image &input);
void blurAndSobelFusedInto(const Image &input, Image &output);

} // namespace dsp
