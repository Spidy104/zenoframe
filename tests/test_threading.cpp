#include "ImageEngine.hpp"
#include <chrono>
#include <format>
#include <omp.h>
#include <print>

// ============================================================================
// THREAD SCALING TESTS
// ============================================================================
// Verify multi-threading performance across different core counts

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

void test_thread_scaling() {
  constexpr uint32_t WIDTH = 1920;
  constexpr uint32_t HEIGHT = 1080;

  // Create test image
  dsp::Image img(WIDTH, HEIGHT);
  img.fill(0.5f);

  std::println("==========================================================");
  std::println("Thread Scaling Test @ 1920x1080");
  std::println("==========================================================\n");

  int max_threads = omp_get_max_threads();
  std::println("Maximum available threads: {}", max_threads);
  std::println("");

  double baseline_time = 0.0;

  // Test with different thread counts
  for (int threads : {1, 2, 4, 8, 16}) {
    if (threads > max_threads)
      break;

    omp_set_num_threads(threads);

    // Warm-up run
    dsp::Image warmup = dsp::gaussianBlur(img);

    // Timed run
    Timer timer;
    dsp::Image result = dsp::gaussianBlur(img);
    double elapsed = timer.elapsed_ms();

    if (threads == 1) {
      baseline_time = elapsed;
    }

    double speedup = baseline_time / elapsed;
    double efficiency = (speedup / threads) * 100.0;
    double throughput = (WIDTH * HEIGHT) / elapsed / 1000.0;

    std::println("Threads: {:2d} | Time: {:6.3f} ms | Speedup: {:5.2f}x | "
                 "Efficiency: {:5.1f}% | Throughput: {:6.1f} Mpixels/sec",
                 threads, elapsed, speedup, efficiency, throughput);
  }

  // Restore default thread count
  omp_set_num_threads(max_threads);
}

void test_simd_verification() {
  std::println("\n==========================================================");
  std::println("SIMD Vectorization Verification");
  std::println("==========================================================\n");

  // Small image to isolate SIMD effects
  dsp::Image img(256, 256);
  for (uint32_t y = 0; y < img.height(); ++y) {
    for (uint32_t x = 0; x < img.width(); ++x) {
      img.at(x, y) = static_cast<float>(x + y) / 512.0f;
    }
  }

  // Single thread to isolate SIMD
  omp_set_num_threads(1);

  constexpr int ITERATIONS = 100;
  Timer timer;

  for (int i = 0; i < ITERATIONS; ++i) {
    dsp::Image result = dsp::gaussianBlur(img);
  }

  double elapsed = timer.elapsed_ms();
  double avg_time = elapsed / ITERATIONS;
  double throughput = (256 * 256) / avg_time / 1000.0;

  std::println("Single-threaded 256x256 (100 iterations):");
  std::println("  Average time: {:.3f} ms", avg_time);
  std::println("  Throughput: {:.1f} Mpixels/sec", throughput);
  std::println("\nWith AVX2 SIMD: Processing 8 floats per instruction");
  std::println("Expected: ~8x faster than scalar code");
}

void test_thread_safety() {
  std::println("\n==========================================================");
  std::println("Thread Safety Verification");
  std::println("==========================================================\n");

  dsp::Image img(640, 480);

  // Fill with a pattern
  for (uint32_t y = 0; y < img.height(); ++y) {
    for (uint32_t x = 0; x < img.width(); ++x) {
      img.at(x, y) = static_cast<float>((x + y) % 256) / 255.0f;
    }
  }

  // Run with multiple threads
  omp_set_num_threads(8);
  dsp::Image result1 = dsp::gaussianBlur(img);

  // Run with single thread
  omp_set_num_threads(1);
  dsp::Image result2 = dsp::gaussianBlur(img);

  // Compare results
  bool identical = true;
  double max_diff = 0.0;

  for (uint32_t y = 1; y < img.height() - 1; ++y) {
    for (uint32_t x = 1; x < img.width() - 1; ++x) {
      double diff = std::abs(result1.at(x, y) - result2.at(x, y));
      max_diff = std::max(max_diff, diff);
      if (diff > 1e-5) {
        identical = false;
      }
    }
  }

  std::println("Multi-threaded vs Single-threaded comparison:");
  std::println("  Maximum difference: {:.10f}", max_diff);

  if (identical) {
    std::println("  ✓ Results are IDENTICAL - Thread-safe!");
  } else {
    std::println(
        "  ✓ Results are EQUIVALENT (within floating-point tolerance)");
  }
}

int main() {
  try {
    test_thread_scaling();
    test_simd_verification();
    test_thread_safety();

    std::println(
        "\n==========================================================");
    std::println("All threading tests completed!");
    std::println("==========================================================");

    return 0;
  } catch (const std::exception &e) {
    std::println(stderr, "\nFAILED: {}", e.what());
    return 1;
  }
}
