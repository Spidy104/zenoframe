#include "ImageEngine.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <print>
#include <thread>
#include <vector>

class PrecisionTimer {
private:
  std::chrono::high_resolution_clock::time_point start_;

public:
  PrecisionTimer() : start_(std::chrono::high_resolution_clock::now()) {}
  double elapsed_ms() const {
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(end - start_).count();
  }
};

struct BenchmarkStats {
  double min_ms, max_ms, avg_ms, median_ms, stddev_ms, p99_ms, variance_pct;
  size_t total_frames;

  void print(const char *name) const {
    std::println("\n{} Statistics:", name);
    std::println("  ├─ Frames:     {}", total_frames);
    std::println("  ├─ Min:        {:.3f} ms ({:.1f} FPS)", min_ms,
                 1000.0 / min_ms);
    std::println("  ├─ Max:        {:.3f} ms ({:.1f} FPS)", max_ms,
                 1000.0 / max_ms);
    std::println("  ├─ Mean:       {:.3f} ms ({:.1f} FPS)", avg_ms,
                 1000.0 / avg_ms);
    std::println("  ├─ Median:     {:.3f} ms ({:.1f} FPS)", median_ms,
                 1000.0 / median_ms);
    std::println("  ├─ Std Dev:    {:.3f} ms", stddev_ms);
    std::println("  ├─ P99:        {:.3f} ms ({:.1f} FPS)", p99_ms,
                 1000.0 / p99_ms);
    std::println("  └─ Variance:   {:.1f}%", variance_pct);

    if (variance_pct > 10.0) {
      std::println("  ⚠ HIGH VARIANCE - Check thermal throttling");
    } else {
      std::println("  ✓ STABLE - Consistent frame timing");
    }
  }
};

BenchmarkStats analyze(std::vector<double> &times) {
  BenchmarkStats stats{};
  stats.total_frames = times.size();
  if (times.empty())
    return stats;

  std::sort(times.begin(), times.end());
  stats.min_ms = times.front();
  stats.max_ms = times.back();
  stats.median_ms = times[times.size() / 2];
  stats.p99_ms = times[std::min(static_cast<size_t>(times.size() * 0.99),
                                times.size() - 1)];

  double sum = std::accumulate(times.begin(), times.end(), 0.0);
  stats.avg_ms = sum / times.size();

  double sq_sum = 0.0;
  for (double t : times)
    sq_sum += (t - stats.avg_ms) * (t - stats.avg_ms);
  stats.stddev_ms = std::sqrt(sq_sum / times.size());
  stats.variance_pct = (stats.stddev_ms / stats.avg_ms) * 100.0;

  return stats;
}

void print_efficiency(uint32_t width, uint32_t height, int ops_per_pixel,
                      double avg_ms) {
  uint64_t total_pixels = width * height;
  uint64_t total_ops = total_pixels * ops_per_pixel;
  double time_sec = avg_ms / 1000.0;

  double gflops = (total_ops / time_sec) / 1e9;
  double cycles_per_pixel = (3.5e9 * time_sec) / total_pixels;
  double bandwidth = (total_pixels * 26 * sizeof(float) / time_sec) / 1e9;

  std::println("\nEfficiency Metrics @ {}x{}:", width, height);
  std::println("  ├─ Throughput:  {:.2f} GFLOPS", gflops);
  std::println("  ├─ Efficiency:  {:.1f} cycles/pixel", cycles_per_pixel);
  std::println("  └─ Bandwidth:   {:.2f} GB/s", bandwidth);
}

void print_budget_assessment(const BenchmarkStats &stats, double target_ms) {
  if (stats.p99_ms <= target_ms) {
    std::println("  ✓ P99 PASSES 144Hz TARGET");
  } else if (stats.avg_ms <= target_ms) {
    std::println("  ⚠ AVERAGE PASSES 144Hz, P99 DOES NOT");
  } else {
    std::println("  ✗ MISSES 144Hz TARGET");
  }
}

void thermal_stress_test(dsp::Image &img, int iterations) {
  std::println("\n═══════════════════════════════════════════════════════════");
  std::println("THERMAL STRESS TEST: {} iterations", iterations);
  std::println("═══════════════════════════════════════════════════════════\n");

  std::vector<double> fused_times, separate_times;
  fused_times.reserve(iterations);
  separate_times.reserve(iterations);
  dsp::Image fused_output(img.width(), img.height());
  dsp::Image blur_output(img.width(), img.height());
  dsp::Image sobel_output(img.width(), img.height());

  // Warmup
  for (int i = 0; i < 10; ++i) {
    dsp::blurAndSobelFusedInto(img, fused_output);
    dsp::gaussianBlurInto(img, blur_output);
    dsp::sobelMagnitudeInto(blur_output, sobel_output);
  }

  std::println("Running {} iterations...\n", iterations);
  int checkpoint = iterations / 10;

  for (int i = 0; i < iterations; ++i) {
    {
      PrecisionTimer t;
      dsp::blurAndSobelFusedInto(img, fused_output);
      fused_times.push_back(t.elapsed_ms());
    }
    {
      PrecisionTimer t;
      dsp::gaussianBlurInto(img, blur_output);
      dsp::sobelMagnitudeInto(blur_output, sobel_output);
      separate_times.push_back(t.elapsed_ms());
    }

    if (checkpoint > 0 && (i + 1) % checkpoint == 0) {
      std::println("  {}%: Fused={:.2f}ms, Separate={:.2f}ms",
                   ((i + 1) * 100) / iterations, fused_times.back(),
                   separate_times.back());
    }
  }

  analyze(fused_times).print("FUSED 5x5 KERNEL");
  analyze(separate_times).print("SEPARATE 3x3 PIPELINE");

  // Thermal analysis
  int tenth = iterations / 10;
  double early =
      std::accumulate(fused_times.begin(), fused_times.begin() + tenth, 0.0) /
      tenth;
  double late =
      std::accumulate(fused_times.end() - tenth, fused_times.end(), 0.0) /
      tenth;
  double delta = ((late - early) / early) * 100;

  std::println("\n───────────────────────────────────────────────────────────");
  std::println("THERMAL ANALYSIS:");
  std::println("  Early (first 10%%): {:.3f} ms", early);
  std::println("  Late (last 10%%):   {:.3f} ms", late);
  std::println("  Delta:              {:.1f}%", delta);
  if (delta > 5) {
    std::println("  ⚠ THERMAL THROTTLING");
  } else {
    std::println("  ✓ THERMALLY STABLE");
  }

  double avg_fused =
      std::accumulate(fused_times.begin(), fused_times.end(), 0.0) /
      fused_times.size();
  print_efficiency(img.width(), img.height(), 103, avg_fused);
}

void paced_144hz_test(dsp::Image &img, int iterations) {
  std::println("\n═══════════════════════════════════════════════════════════");
  std::println("PACED 144 HZ RUN: {} frames", iterations);
  std::println("═══════════════════════════════════════════════════════════\n");

  constexpr double TARGET_MS = 6.944;
  const auto target_interval = std::chrono::duration_cast<
      std::chrono::steady_clock::duration>(
      std::chrono::duration<double, std::milli>(TARGET_MS));

  std::vector<double> compute_times;
  compute_times.reserve(iterations);
  dsp::Image fused_output(img.width(), img.height());

  for (int i = 0; i < 20; ++i)
    dsp::blurAndSobelFusedInto(img, fused_output);

  size_t compute_budget_misses = 0;
  size_t scheduler_late_starts = 0;
  size_t skipped_frame_slots = 0;

  auto next_frame = std::chrono::steady_clock::now() + target_interval;
  const auto run_start = std::chrono::steady_clock::now();
  const int checkpoint = iterations / 10;

  for (int i = 0; i < iterations; ++i) {
    std::this_thread::sleep_until(next_frame);
    const auto scheduled_start = next_frame;
    const auto actual_start = std::chrono::steady_clock::now();
    const auto start_jitter_ms =
        std::chrono::duration<double, std::milli>(actual_start -
                                                  scheduled_start)
            .count();
    if (start_jitter_ms > 0.25)
      ++scheduler_late_starts;

    PrecisionTimer t;
    dsp::blurAndSobelFusedInto(img, fused_output);
    const double compute_ms = t.elapsed_ms();
    compute_times.push_back(compute_ms);
    if (compute_ms > TARGET_MS)
      ++compute_budget_misses;

    auto frame_deadline = scheduled_start + target_interval;
    const auto done = std::chrono::steady_clock::now();
    while (frame_deadline < done) {
      frame_deadline += target_interval;
      ++skipped_frame_slots;
    }
    next_frame = frame_deadline;

    if (checkpoint > 0 && (i + 1) % checkpoint == 0) {
      std::println("  {}%: compute={:.3f}ms, budget_misses={}",
                   ((i + 1) * 100) / iterations, compute_times.back(),
                   compute_budget_misses);
    }
  }

  const auto run_end = std::chrono::steady_clock::now();
  const double wall_ms =
      std::chrono::duration<double, std::milli>(run_end - run_start).count();
  auto stats = analyze(compute_times);
  stats.print("PACED FUSED 5x5 KERNEL");

  std::println("\nPacing @ 144Hz:");
  std::println("  ├─ Target budget:       {:.3f} ms/frame", TARGET_MS);
  std::println("  ├─ Wall time:           {:.3f} s", wall_ms / 1000.0);
  std::println("  ├─ Effective cadence:   {:.1f} FPS",
               (iterations * 1000.0) / wall_ms);
  std::println("  ├─ Compute misses:      {} / {} ({:.2f}%)",
               compute_budget_misses, iterations,
               (compute_budget_misses * 100.0) / iterations);
  std::println("  ├─ Late OS wakeups:     {} / {} ({:.2f}%)",
               scheduler_late_starts, iterations,
               (scheduler_late_starts * 100.0) / iterations);
  std::println("  └─ Skipped frame slots: {}", skipped_frame_slots);

  if (stats.p99_ms <= TARGET_MS && compute_budget_misses == 0) {
    std::println("  ✓ PACED 144Hz TARGET PASSED");
  } else if (stats.p99_ms <= TARGET_MS) {
    std::println("  ⚠ P99 PASSES 144Hz, RARE MAX OUTLIERS REMAIN");
  } else if (stats.avg_ms <= TARGET_MS) {
    std::println("  ⚠ AVERAGE PASSES, BUT P99/MISSES NEED ATTENTION");
  } else {
    std::println("  ✗ PACED 144Hz TARGET MISSED");
  }
}

void benchmark_single_ops(dsp::Image &img, int iterations) {
  std::println("\n═══════════════════════════════════════════════════════════");
  std::println("SINGLE OPERATION BENCHMARKS");
  std::println("═══════════════════════════════════════════════════════════");

  constexpr double TARGET_MS = 6.944;

  // Gaussian Blur
  {
    std::vector<double> times;
    dsp::Image output(img.width(), img.height());
    for (int i = 0; i < 5; ++i)
      dsp::gaussianBlurInto(img, output);
    for (int i = 0; i < iterations; ++i) {
      PrecisionTimer t;
      dsp::gaussianBlurInto(img, output);
      times.push_back(t.elapsed_ms());
    }
    auto stats = analyze(times);
    stats.print("GAUSSIAN BLUR (AVX2)");
    print_budget_assessment(stats, TARGET_MS);
  }

  // Sobel Magnitude
  {
    std::vector<double> times;
    dsp::Image output(img.width(), img.height());
    for (int i = 0; i < 5; ++i)
      dsp::sobelMagnitudeInto(img, output);
    for (int i = 0; i < iterations; ++i) {
      PrecisionTimer t;
      dsp::sobelMagnitudeInto(img, output);
      times.push_back(t.elapsed_ms());
    }
    auto stats = analyze(times);
    stats.print("SOBEL MAGNITUDE (AVX2)");
    print_budget_assessment(stats, TARGET_MS);
  }
}

int main() {
  std::println("╔═══════════════════════════════════════════════════════════╗");
  std::println("║  ZenoFrame DSP Benchmark Suite                           ║");
  std::println(
      "╚═══════════════════════════════════════════════════════════╝\n");

  constexpr uint32_t WIDTH = 1920, HEIGHT = 1080;
  std::println("Config: {}x{} @ 144Hz target ({:.3f} ms/frame)", WIDTH, HEIGHT,
               6.944);

  dsp::Image img(WIDTH, HEIGHT);
  for (uint32_t y = 0; y < HEIGHT; ++y)
    for (uint32_t x = 0; x < WIDTH; ++x)
      img.at(x, y) = static_cast<float>(x + y) / (WIDTH + HEIGHT);

  benchmark_single_ops(img, 100);
  paced_144hz_test(img, 1000);
  thermal_stress_test(img, 1000);

  std::println("\n═══════════════════════════════════════════════════════════");
  std::println("BENCHMARK COMPLETE");
  std::println("═══════════════════════════════════════════════════════════\n");

  return 0;
}
