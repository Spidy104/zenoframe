#include "ImageEngine.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <print>

namespace {

constexpr float EPSILON = 1e-5f;

bool nearly_equal(float lhs, float rhs, float epsilon = EPSILON) {
  return std::fabs(lhs - rhs) <= epsilon;
}

float max_abs_diff(const dsp::Image &lhs, const dsp::Image &rhs,
                   uint32_t min_x = 0, uint32_t min_y = 0,
                   uint32_t max_x_exclusive = 0,
                   uint32_t max_y_exclusive = 0) {
  if (max_x_exclusive == 0) {
    max_x_exclusive = lhs.width();
  }
  if (max_y_exclusive == 0) {
    max_y_exclusive = lhs.height();
  }

  float diff = 0.0f;
  for (uint32_t y = min_y; y < max_y_exclusive; ++y) {
    for (uint32_t x = min_x; x < max_x_exclusive; ++x) {
      diff = std::max(diff, std::fabs(lhs.at(x, y) - rhs.at(x, y)));
    }
  }
  return diff;
}

dsp::Image make_pattern_image(uint32_t width, uint32_t height) {
  dsp::Image image(width, height);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const float horizontal = static_cast<float>((x * 7u) % 19u) / 19.0f;
      const float vertical = static_cast<float>((y * 11u) % 23u) / 23.0f;
      const float interaction = static_cast<float>((x * y) % 29u) / 58.0f;
      image.at(x, y) = std::clamp(horizontal * 0.45f + vertical * 0.35f +
                                      interaction * 0.20f,
                                  0.0f, 1.0f);
    }
  }
  return image;
}

void assert_image_range(const dsp::Image &image) {
  for (size_t index = 0; index < image.size(); ++index) {
    assert(image.data()[index] >= 0.0f);
    assert(image.data()[index] <= 1.0f);
  }
}

void test_gaussian_separable_impulse_response() {
  std::print("Test: Gaussian Separable Impulse Response... ");

  dsp::Image input(9, 9);
  input.fill(0.0f);
  input.at(4, 4) = 1.0f;

  const dsp::Image separable = dsp::gaussianBlurSeparable(input);

  assert(nearly_equal(separable.at(3, 3), 1.0f / 16.0f));
  assert(nearly_equal(separable.at(4, 3), 2.0f / 16.0f));
  assert(nearly_equal(separable.at(5, 3), 1.0f / 16.0f));
  assert(nearly_equal(separable.at(3, 4), 2.0f / 16.0f));
  assert(nearly_equal(separable.at(4, 4), 4.0f / 16.0f));
  assert(nearly_equal(separable.at(5, 4), 2.0f / 16.0f));
  assert(nearly_equal(separable.at(3, 5), 1.0f / 16.0f));
  assert(nearly_equal(separable.at(4, 5), 2.0f / 16.0f));
  assert(nearly_equal(separable.at(5, 5), 1.0f / 16.0f));

  assert(nearly_equal(separable.at(4, 2), 0.0f));
  assert(nearly_equal(separable.at(2, 4), 0.0f));
  assert(nearly_equal(separable.at(6, 4), 0.0f));
  assert(nearly_equal(separable.at(4, 6), 0.0f));

  assert_image_range(separable);
  std::println("PASSED");
}

void test_sobel_fused_matches_reference() {
  std::print("Test: Fused Sobel Matches AVX2 Reference... ");

  const dsp::Image input = make_pattern_image(40, 28);
  const dsp::Image reference = dsp::sobelMagnitude(input);
  const dsp::Image fused = dsp::sobelMagnitudeFused(input);
  const float diff = max_abs_diff(reference, fused);

  assert(diff <= 2e-5f);
  assert_image_range(reference);
  assert_image_range(fused);
  std::println("PASSED");
}

void test_blur_and_sobel_fused_matches_separate_pipeline() {
  std::print("Test: 5x5 Fused Pipeline Matches Separate Pipeline... ");

  const dsp::Image input = make_pattern_image(32, 24);
  const dsp::Image separate = dsp::sobelMagnitude(dsp::gaussianBlur(input));
  const dsp::Image fused = dsp::blurAndSobelFused(input);
  const float diff =
      max_abs_diff(separate, fused, 3, 3, input.width() - 3, input.height() - 3);

  assert(diff <= 1e-4f);
  assert_image_range(separate);
  assert_image_range(fused);
  std::println("PASSED");
}

void test_pipeline_preserves_expected_boundaries() {
  std::print("Test: Pipeline Boundary Policy Is Stable... ");

  dsp::Image input(12, 12);
  input.fill(0.25f);
  input.at(0, 0) = 0.9f;
  input.at(11, 0) = 0.8f;
  input.at(0, 11) = 0.7f;
  input.at(11, 11) = 0.6f;

  const dsp::Image blurred = dsp::gaussianBlur(input);
  const dsp::Image sobel = dsp::sobelMagnitude(input);
  const dsp::Image fused = dsp::blurAndSobelFused(input);

  assert(nearly_equal(blurred.at(0, 0), input.at(0, 0)));
  assert(nearly_equal(blurred.at(11, 0), input.at(11, 0)));
  assert(nearly_equal(blurred.at(0, 11), input.at(0, 11)));
  assert(nearly_equal(blurred.at(11, 11), input.at(11, 11)));

  for (uint32_t x = 0; x < input.width(); ++x) {
    assert(nearly_equal(sobel.at(x, 0), 0.0f));
    assert(nearly_equal(sobel.at(x, input.height() - 1), 0.0f));
    assert(nearly_equal(fused.at(x, 0), 0.0f));
    assert(nearly_equal(fused.at(x, 1), 0.0f));
    assert(nearly_equal(fused.at(x, input.height() - 2), 0.0f));
    assert(nearly_equal(fused.at(x, input.height() - 1), 0.0f));
  }

  for (uint32_t y = 0; y < input.height(); ++y) {
    assert(nearly_equal(sobel.at(0, y), 0.0f));
    assert(nearly_equal(sobel.at(input.width() - 1, y), 0.0f));
    assert(nearly_equal(fused.at(0, y), 0.0f));
    assert(nearly_equal(fused.at(1, y), 0.0f));
    assert(nearly_equal(fused.at(input.width() - 2, y), 0.0f));
    assert(nearly_equal(fused.at(input.width() - 1, y), 0.0f));
  }

  std::println("PASSED");
}

} // namespace

int main() {
  std::println("==========================================================");
  std::println("DSP Regression Tests");
  std::println("==========================================================\n");

  test_gaussian_separable_impulse_response();
  test_sobel_fused_matches_reference();
  test_blur_and_sobel_fused_matches_separate_pipeline();
  test_pipeline_preserves_expected_boundaries();

  std::println("\n==========================================================");
  std::println("All DSP regression tests PASSED!");
  std::println("==========================================================");
  return 0;
}