#include "ImageEngine.hpp"
#include <chrono>
#include <print>

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

int main() {
  constexpr uint32_t WIDTH = 1920;
  constexpr uint32_t HEIGHT = 1080;
  constexpr double TARGET_MS = 6.944;
  constexpr int WARMUP = 3;
  constexpr int ITERATIONS = 20;

  std::println("==========================================================");
  std::println("Phase 8: FUSED Pipeline Benchmarks @ 1920x1080");
  std::println("==========================================================\n");

  dsp::Image img(WIDTH, HEIGHT);
  img.fill(0.5f);

  // ========================================================================
  // GAUSSIAN BLUR (AVX2)
  // ========================================================================
  std::println("GAUSSIAN BLUR (AVX2):");
  for (int i = 0; i < WARMUP; ++i)
    dsp::gaussianBlur(img);

  double total = 0, min_time = 1e9, max_time = 0;
  for (int i = 0; i < ITERATIONS; ++i) {
    Timer t;
    dsp::Image result = dsp::gaussianBlur(img);
    double elapsed = t.elapsed_ms();
    total += elapsed;
    min_time = std::min(min_time, elapsed);
    max_time = std::max(max_time, elapsed);
  }
  std::println("  Best: {:.3f} ms ({:.1f} FPS) {}", min_time, 1000.0 / min_time,
               min_time <= TARGET_MS ? "✓" : "");

  // ========================================================================
  // SOBEL MAGNITUDE (AVX2)
  // ========================================================================
  std::println("SOBEL MAGNITUDE (AVX2):");
  for (int i = 0; i < WARMUP; ++i)
    dsp::sobelMagnitude(img);

  total = 0;
  min_time = 1e9;
  max_time = 0;
  for (int i = 0; i < ITERATIONS; ++i) {
    Timer t;
    dsp::Image result = dsp::sobelMagnitude(img);
    double elapsed = t.elapsed_ms();
    total += elapsed;
    min_time = std::min(min_time, elapsed);
    max_time = std::max(max_time, elapsed);
  }
  std::println("  Best: {:.3f} ms ({:.1f} FPS) {}", min_time, 1000.0 / min_time,
               min_time <= TARGET_MS ? "✓" : "");

  // ========================================================================
  // FUSED BLUR+SOBEL (Single Pass!)
  // ========================================================================
  std::println("\nFUSED BLUR+SOBEL (Phase 8 - Single Pass):");
  for (int i = 0; i < WARMUP; ++i)
    dsp::blurAndSobelFused(img);

  total = 0;
  min_time = 1e9;
  max_time = 0;
  for (int i = 0; i < ITERATIONS; ++i) {
    Timer t;
    dsp::Image result = dsp::blurAndSobelFused(img);
    double elapsed = t.elapsed_ms();
    total += elapsed;
    min_time = std::min(min_time, elapsed);
    max_time = std::max(max_time, elapsed);
  }
  double avg = total / ITERATIONS;
  std::println("  Best:    {:.3f} ms ({:.1f} FPS)", min_time,
               1000.0 / min_time);
  std::println("  Average: {:.3f} ms ({:.1f} FPS)", avg, 1000.0 / avg);

  if (min_time <= TARGET_MS) {
    std::println("  ✓ PASSES 144Hz @ {:.1f}% of budget!",
                 (min_time / TARGET_MS) * 100);
  } else {
    std::println("  ⚠ {:.1f}% of target ({:.2f}ms to go)",
                 (min_time / TARGET_MS) * 100, min_time - TARGET_MS);
  }

  // ========================================================================
  // SEPARATE (for comparison)
  // ========================================================================
  std::println("\nSEPARATE BLUR+SOBEL (comparison):");
  for (int i = 0; i < WARMUP; ++i) {
    dsp::Image blur = dsp::gaussianBlur(img);
    dsp::Image edges = dsp::sobelMagnitude(blur);
  }

  total = 0;
  min_time = 1e9;
  for (int i = 0; i < ITERATIONS; ++i) {
    Timer t;
    dsp::Image blur = dsp::gaussianBlur(img);
    dsp::Image edges = dsp::sobelMagnitude(blur);
    double elapsed = t.elapsed_ms();
    total += elapsed;
    min_time = std::min(min_time, elapsed);
  }
  avg = total / ITERATIONS;
  std::println("  Best:    {:.3f} ms ({:.1f} FPS)", min_time,
               1000.0 / min_time);
  std::println("  Average: {:.3f} ms ({:.1f} FPS)", avg, 1000.0 / avg);

  std::println("\n==========================================================");
  std::println("Target: 144 FPS = {:.3f} ms per frame", TARGET_MS);
  std::println("==========================================================");

  return 0;
}
