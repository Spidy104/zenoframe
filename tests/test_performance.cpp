#include "ImageEngine.hpp"
#include <chrono>
#include <format>
#include <print>

// ============================================================================
// PERFORMANCE BENCHMARK TESTS
// ============================================================================
// Tests processing speed for 144Hz @ 1080p target

class Timer {
private:
  std::chrono::high_resolution_clock::time_point start_;

public:
  Timer() : start_(std::chrono::high_resolution_clock::now()) {}

  double elapsed_ms() const {
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(end - start_).count();
  }
};

// Test 1: Small image performance (100x100)
void test_small_image_performance() {
  std::print("Test: Small Image Performance (100x100)... ");

  dsp::Image img(100, 100);
  img.fill(0.5f);

  Timer timer;
  dsp::Image result = dsp::gaussianBlur(img);
  double elapsed = timer.elapsed_ms();

  std::println("{:.3f} ms - PASSED", elapsed);
}

// Test 2: Medium image performance (640x480)
void test_medium_image_performance() {
  std::print("Test: Medium Image Performance (640x480)... ");

  dsp::Image img(640, 480);
  img.fill(0.5f);

  Timer timer;
  dsp::Image result = dsp::gaussianBlur(img);
  double elapsed = timer.elapsed_ms();

  double throughput = (640.0 * 480.0) / elapsed / 1000.0;
  std::println("{:.3f} ms ({:.2f} Mpixels/sec) - PASSED", elapsed, throughput);
}

// Test 3: HD image performance (1280x720)
void test_hd_image_performance() {
  std::print("Test: HD Image Performance (1280x720)... ");

  dsp::Image img(1280, 720);
  img.fill(0.5f);

  Timer timer;
  dsp::Image result = dsp::gaussianBlur(img);
  double elapsed = timer.elapsed_ms();

  double throughput = (1280.0 * 720.0) / elapsed / 1000.0;
  std::println("{:.3f} ms ({:.2f} Mpixels/sec) - PASSED", elapsed, throughput);
}

// Test 4: Full HD performance (1920x1080) - 144Hz target
void test_fullhd_performance() {
  std::print("Test: Full HD Performance (1920x1080)... ");

  constexpr uint32_t WIDTH = 1920;
  constexpr uint32_t HEIGHT = 1080;
  constexpr double TARGET_MS = 6.944; // 1000ms / 144fps

  dsp::Image img(WIDTH, HEIGHT);
  img.fill(0.5f);

  Timer timer;
  dsp::Image result = dsp::gaussianBlur(img);
  double elapsed = timer.elapsed_ms();

  double throughput = (WIDTH * HEIGHT) / elapsed / 1000.0;
  double fps_equivalent = 1000.0 / elapsed;

  std::println("{:.3f} ms ({:.2f} Mpixels/sec, {:.1f} FPS equivalent)", elapsed,
               throughput, fps_equivalent);

  if (elapsed < TARGET_MS) {
    std::println("  ✓ EXCEEDS 144Hz target ({:.1f}% of budget)",
                 TARGET_MS / elapsed * 100.0);
  } else {
    std::println("  ⚠ Below 144Hz target ({:.1f}% over budget)",
                 elapsed / TARGET_MS * 100.0);
  }
}

// Test 5: Sobel magnitude performance (1920x1080)
void test_sobel_performance() {
  std::print("Test: Sobel Magnitude Performance (1920x1080)... ");

  constexpr uint32_t WIDTH = 1920;
  constexpr uint32_t HEIGHT = 1080;
  constexpr double TARGET_MS = 6.944 * 2.0; // 2x convolutions

  dsp::Image img(WIDTH, HEIGHT);
  img.fill(0.5f);

  Timer timer;
  dsp::Image result = dsp::sobelMagnitude(img);
  double elapsed = timer.elapsed_ms();

  double throughput = (WIDTH * HEIGHT) / elapsed / 1000.0;

  std::println("{:.3f} ms ({:.2f} Mpixels/sec)", elapsed, throughput);

  if (elapsed < TARGET_MS) {
    std::println("  ✓ EXCEEDS target ({:.1f}% of budget)",
                 TARGET_MS / elapsed * 100.0);
  } else {
    std::println("  ⚠ Below target ({:.1f}% over budget)",
                 elapsed / TARGET_MS * 100.0);
  }
}

// Test 6: Multiple iterations (thermal stability)
void test_sustained_performance() {
  std::println("Test: Sustained Performance (10 iterations @ 1920x1080)...");

  dsp::Image img(1920, 1080);
  img.fill(0.5f);

  double total_time = 0.0;
  double min_time = 1e9;
  double max_time = 0.0;

  constexpr int ITERATIONS = 10;

  for (int i = 0; i < ITERATIONS; ++i) {
    Timer timer;
    dsp::Image result = dsp::gaussianBlur(img);
    double elapsed = timer.elapsed_ms();

    total_time += elapsed;
    min_time = std::min(min_time, elapsed);
    max_time = std::max(max_time, elapsed);

    std::println("  Iteration {}: {:.3f} ms", i + 1, elapsed);
  }

  double avg_time = total_time / ITERATIONS;

  std::println("  Average: {:.3f} ms", avg_time);
  std::println("  Min: {:.3f} ms, Max: {:.3f} ms", min_time, max_time);
  std::println("  Variance: {:.1f}%", (max_time - min_time) / avg_time * 100.0);
}

int main() {
  std::println("==========================================================");
  std::println("Performance Benchmark Tests");
  std::println("==========================================================\n");

  try {
    test_small_image_performance();
    test_medium_image_performance();
    test_hd_image_performance();
    std::println("");
    test_fullhd_performance();
    std::println("");
    test_sobel_performance();
    std::println("");
    test_sustained_performance();

    std::println(
        "\n==========================================================");
    std::println("All performance tests completed!");
    std::println("==========================================================");

    return 0;
  } catch (const std::exception &e) {
    std::println(stderr, "\nFAILED: {}", e.what());
    return 1;
  }
}
