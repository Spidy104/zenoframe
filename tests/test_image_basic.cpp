#include "ImageEngine.hpp"
#include <cassert>
#include <cmath>
#include <print>

// ============================================================================
// BASIC IMAGE OPERATIONS TESTS
// ============================================================================
// Tests fundamental image creation, access, and manipulation

namespace {

constexpr float EPSILON = 1e-6f;

bool floatEqual(float a, float b) { return std::fabs(a - b) < EPSILON; }

} // anonymous namespace

// Test 1: Image construction and dimensions
void test_image_construction() {
  std::print("Test: Image Construction... ");

  dsp::Image img(100, 50);
  assert(img.width() == 100);
  assert(img.height() == 50);
  assert(img.size() == 5000);

  std::println("PASSED");
}

// Test 2: Pixel access and modification
void test_pixel_access() {
  std::print("Test: Pixel Access... ");

  dsp::Image img(10, 10);

  // Set and get pixel values
  img.at(5, 5) = 0.75f;
  assert(floatEqual(img.at(5, 5), 0.75f));

  img.at(0, 0) = 0.0f;
  assert(floatEqual(img.at(0, 0), 0.0f));

  img.at(9, 9) = 1.0f;
  assert(floatEqual(img.at(9, 9), 1.0f));

  std::println("PASSED");
}

// Test 3: Fill operation
void test_fill() {
  std::print("Test: Fill Operation... ");

  dsp::Image img(20, 20);
  img.fill(0.5f);

  // Check all pixels are set to 0.5
  for (uint32_t y = 0; y < img.height(); ++y) {
    for (uint32_t x = 0; x < img.width(); ++x) {
      assert(floatEqual(img.at(x, y), 0.5f));
    }
  }

  std::println("PASSED");
}

// Test 4: Clamp operation
void test_clamp() {
  std::print("Test: Clamp Operation... ");

  dsp::Image img(10, 10);

  // Set values outside [0, 1] range
  img.at(0, 0) = -0.5f;
  img.at(1, 1) = 1.5f;
  img.at(2, 2) = 0.5f;

  img.clamp();

  assert(floatEqual(img.at(0, 0), 0.0f)); // Clamped to 0
  assert(floatEqual(img.at(1, 1), 1.0f)); // Clamped to 1
  assert(floatEqual(img.at(2, 2), 0.5f)); // Unchanged

  std::println("PASSED");
}

// Test 5: Row-major memory layout
void test_memory_layout() {
  std::print("Test: Row-Major Memory Layout... ");

  dsp::Image img(4, 3);

  // Fill with sequential values
  for (size_t i = 0; i < img.size(); ++i) {
    img.data()[i] = static_cast<float>(i);
  }

  // Verify row-major ordering: Index = y * width + x
  assert(floatEqual(img.at(0, 0), 0.0f)); // Index 0
  assert(floatEqual(img.at(1, 0), 1.0f)); // Index 1
  assert(floatEqual(img.at(2, 0), 2.0f)); // Index 2
  assert(floatEqual(img.at(3, 0), 3.0f)); // Index 3
  assert(floatEqual(img.at(0, 1), 4.0f)); // Index 4
  assert(floatEqual(img.at(1, 1), 5.0f)); // Index 5

  std::println("PASSED");
}

// Test 6: Direct data access
void test_data_access() {
  std::print("Test: Direct Data Access... ");

  dsp::Image img(5, 5);

  // Access via data pointer
  float *data = img.data();
  data[0] = 0.1f;
  data[12] = 0.9f;

  // Verify through at() accessor
  assert(floatEqual(img.at(0, 0), 0.1f));
  assert(floatEqual(img.at(2, 2), 0.9f)); // 2 * 5 + 2 = 12

  std::println("PASSED");
}

int main() {
  std::println("==========================================================");
  std::println("Basic Image Operations Tests");
  std::println("==========================================================\n");

  try {
    test_image_construction();
    test_pixel_access();
    test_fill();
    test_clamp();
    test_memory_layout();
    test_data_access();

    std::println(
        "\n==========================================================");
    std::println("All basic tests PASSED!");
    std::println("==========================================================");

    return 0;
  } catch (const std::exception &e) {
    std::println(stderr, "\nFAILED: {}", e.what());
    return 1;
  }
}
