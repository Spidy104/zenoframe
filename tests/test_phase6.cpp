#include "ImageEngine.hpp"
#include <chrono>
#include <format>
#include <print>

// Test Phase 6 optimizations: Separable Gaussian and Fused Sobel

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
  constexpr double TARGET_MS = 6.944; // 144 FPS

  std::println("==========================================================");
  std::println("Phase 6: Separable Convolution & Loop Fusion Benchmarks");
  std::println("==========================================================\n");

  // Create test image
  dsp::Image img(WIDTH, HEIGHT);
  img.fill(0.5f);

  // ========================================================================
  // Test 1: GAUSSIAN BLUR - Original 2D vs Separable
  // ========================================================================

  std::println("1. GAUSSIAN BLUR COMPARISON\n");

  // Original 2D Gaussian (9 multiplications per pixel)
  std::print("   Original 2D Gaussian (9 ops/pixel)... ");
  Timer timer1;
  dsp::Image blur_2d = dsp::gaussianBlur(img);
  double time_2d = timer1.elapsed_ms();
  std::println("{:.3f} ms ({:.1f} FPS)", time_2d, 1000.0 / time_2d);

  // Separable Gaussian (6 multiplications per pixel)
  std::print("   Separable Gaussian (6 ops/pixel)... ");
  Timer timer2;
  dsp::Image blur_sep = dsp::gaussianBlurSeparable(img);
  double time_sep = timer2.elapsed_ms();
  std::println("{:.3f} ms ({:.1f} FPS)", time_sep, 1000.0 / time_sep);

  double speedup_blur = time_2d / time_sep;
  std::println("   → Speedup: {:.2f}x (Expected: ~1.33x)", speedup_blur);

  if (time_sep < TARGET_MS) {
    std::println("   ✓ PASSES 144Hz target ({:.1f}% of budget)\n",
                 TARGET_MS / time_sep * 100.0);
  } else {
    std::println("   ⚠ Below 144Hz target ({:.1f}% over budget)\n",
                 time_sep / TARGET_MS * 100.0);
  }

  // ========================================================================
  // Test 2: SOBEL MAGNITUDE - Original (2-pass) vs Fused (1-pass)
  // ========================================================================

  std::println("2. SOBEL MAGNITUDE COMPARISON\n");

  // Original Sobel (2 separate convolutions + magnitude calculation)
  std::print("   Original Sobel (2-pass)... ");
  Timer timer3;
  dsp::Image sobel_orig = dsp::sobelMagnitude(img);
  double time_orig = timer3.elapsed_ms();
  std::println("{:.3f} ms ({:.1f} FPS)", time_orig, 1000.0 / time_orig);

  // Fused Sobel (single-pass, calculate both gradients + magnitude inline)
  std::print("   Fused Sobel (1-pass)... ");
  Timer timer4;
  dsp::Image sobel_fused = dsp::sobelMagnitudeFused(img);
  double time_fused = timer4.elapsed_ms();
  std::println("{:.3f} ms ({:.1f} FPS)", time_fused, 1000.0 / time_fused);

  double speedup_sobel = time_orig / time_fused;
  std::println("   → Speedup: {:.2f}x (Expected: ~2.0x)", speedup_sobel);

  if (time_fused < TARGET_MS * 2.0) {
    std::println("   ✓ EXCELLENT performance!\n");
  } else {
    std::println("   ⚠ Still needs optimization\n");
  }

  // ========================================================================
  // Summary
  // ========================================================================

  std::println("==========================================================");
  std::println("PHASE 6 RESULTS SUMMARY");
  std::println("==========================================================");
  std::println("Gaussian Blur:");
  std::println("  2D (original):  {:.3f} ms", time_2d);
  std::println("  Separable:      {:.3f} ms ({:.1f}x faster)", time_sep,
               speedup_blur);
  std::println("\nSobel Magnitude:");
  std::println("  2-pass (original): {:.3f} ms", time_orig);
  std::println("  1-pass (fused):    {:.3f} ms ({:.1f}x faster)", time_fused,
               speedup_sobel);
  std::println("\nTarget: 144 FPS ({:.3f} ms per frame)", TARGET_MS);
  std::println("Best Gaussian: {:.1f} FPS ({})", 1000.0 / time_sep,
               time_sep < TARGET_MS ? "✓ PASSES" : "✗ FAILS");
  std::println("==========================================================");

  return 0;
}
