#include "ImageEngine.hpp"
#include <cassert>
#include <cmath>
#include <print>

// ============================================================================
// CONVOLUTION ALGORITHM TESTS
// ============================================================================
// Tests Gaussian blur, Sobel edge detection, and convolution correctness

namespace {

constexpr float EPSILON = 1e-5f;

bool floatEqual(float a, float b, float eps = EPSILON) {
  return std::fabs(a - b) < eps;
}

} // anonymous namespace

// Test 1: Gaussian blur on uniform image
void test_gaussian_uniform() {
  std::print("Test: Gaussian Blur on Uniform Image... ");

  dsp::Image img(10, 10);
  img.fill(0.5f);

  dsp::Image blurred = dsp::gaussianBlur(img);

  // Gaussian blur of uniform image should remain uniform (except edges)
  for (uint32_t y = 1; y < img.height() - 1; ++y) {
    for (uint32_t x = 1; x < img.width() - 1; ++x) {
      assert(floatEqual(blurred.at(x, y), 0.5f, 1e-4f));
    }
  }

  std::println("PASSED");
}

// Test 2: Gaussian blur smoothing effect
void test_gaussian_smoothing() {
  std::print("Test: Gaussian Blur Smoothing... ");

  dsp::Image img(10, 10);
  img.fill(0.0f);

  // Create a bright spot in the center
  img.at(5, 5) = 1.0f;

  dsp::Image blurred = dsp::gaussianBlur(img);

  // Center should be less bright (smoothed)
  assert(blurred.at(5, 5) < 1.0f);
  assert(blurred.at(5, 5) > 0.0f);

  // Neighbors should be brighter than original
  assert(blurred.at(4, 5) > 0.0f);
  assert(blurred.at(6, 5) > 0.0f);
  assert(blurred.at(5, 4) > 0.0f);
  assert(blurred.at(5, 6) > 0.0f);

  std::println("PASSED");
}

// Test 3: Sobel on uniform image (no edges)
void test_sobel_uniform() {
  std::print("Test: Sobel on Uniform Image... ");

  dsp::Image img(10, 10);
  img.fill(0.5f);

  dsp::Image edges = dsp::sobelMagnitude(img);

  // No edges in uniform image (except borders)
  for (uint32_t y = 2; y < img.height() - 2; ++y) {
    for (uint32_t x = 2; x < img.width() - 2; ++x) {
      assert(floatEqual(edges.at(x, y), 0.0f, 1e-4f));
    }
  }

  std::println("PASSED");
}

// Test 4: Sobel vertical edge detection
void test_sobel_vertical_edge() {
  std::print("Test: Sobel Vertical Edge Detection... ");

  dsp::Image img(20, 20);
  img.fill(0.0f);

  // Create a vertical edge (left half black, right half white)
  for (uint32_t y = 0; y < img.height(); ++y) {
    for (uint32_t x = 10; x < img.width(); ++x) {
      img.at(x, y) = 1.0f;
    }
  }

  dsp::Image edges = dsp::sobelMagnitude(img);

  // Strong edge should be detected at x=10
  // (checking a few pixels away from borders)
  for (uint32_t y = 5; y < 15; ++y) {
    assert(edges.at(10, y) > 0.5f); // Strong edge response
  }

  std::println("PASSED");
}

// Test 5: Sobel horizontal edge detection
void test_sobel_horizontal_edge() {
  std::print("Test: Sobel Horizontal Edge Detection... ");

  dsp::Image img(20, 20);
  img.fill(0.0f);

  // Create a horizontal edge (top half black, bottom half white)
  for (uint32_t y = 10; y < img.height(); ++y) {
    for (uint32_t x = 0; x < img.width(); ++x) {
      img.at(x, y) = 1.0f;
    }
  }

  dsp::Image edges = dsp::sobelMagnitude(img);

  // Strong edge should be detected at y=10
  for (uint32_t x = 5; x < 15; ++x) {
    assert(edges.at(x, 10) > 0.5f); // Strong edge response
  }

  std::println("PASSED");
}

// Test 6: Custom convolution kernel (identity)
void test_identity_kernel() {
  std::print("Test: Identity Kernel Convolution... ");

  // Identity kernel (output = input)
  std::array<std::array<float, 3>, 3> identity = {
      {{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}};

  dsp::Image img(10, 10);
  for (uint32_t y = 0; y < img.height(); ++y) {
    for (uint32_t x = 0; x < img.width(); ++x) {
      img.at(x, y) = static_cast<float>(x + y) / 20.0f;
    }
  }

  dsp::Image result = dsp::convolve3x3(img, identity, 1.0f);

  // Interior pixels should be unchanged
  for (uint32_t y = 1; y < img.height() - 1; ++y) {
    for (uint32_t x = 1; x < img.width() - 1; ++x) {
      assert(floatEqual(result.at(x, y), img.at(x, y), 1e-4f));
    }
  }

  std::println("PASSED");
}

// Test 7: Edge preservation
void test_edge_preservation() {
  std::print("Test: Edge Pixel Preservation... ");

  dsp::Image img(10, 10);
  img.fill(0.5f);

  // Set distinct edge values
  img.at(0, 0) = 0.1f;
  img.at(9, 0) = 0.2f;
  img.at(0, 9) = 0.3f;
  img.at(9, 9) = 0.4f;

  dsp::Image blurred = dsp::gaussianBlur(img);

  // Edge pixels should be preserved
  assert(floatEqual(blurred.at(0, 0), 0.1f));
  assert(floatEqual(blurred.at(9, 0), 0.2f));
  assert(floatEqual(blurred.at(0, 9), 0.3f));
  assert(floatEqual(blurred.at(9, 9), 0.4f));

  std::println("PASSED");
}

int main() {
  std::println("==========================================================");
  std::println("Convolution Algorithm Tests");
  std::println("==========================================================\n");

  try {
    test_gaussian_uniform();
    test_gaussian_smoothing();
    test_sobel_uniform();
    test_sobel_vertical_edge();
    test_sobel_horizontal_edge();
    test_identity_kernel();
    test_edge_preservation();

    std::println(
        "\n==========================================================");
    std::println("All convolution tests PASSED!");
    std::println("==========================================================");

    return 0;
  } catch (const std::exception &e) {
    std::println(stderr, "\nFAILED: {}", e.what());
    return 1;
  }
}
