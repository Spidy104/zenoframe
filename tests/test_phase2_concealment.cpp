#include "PhaseConcealment.hpp"
#include "FrameBufferPool.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <numeric>
#include <print>
#include <vector>

namespace {

std::vector<float> make_wave_frame(float phase_shift) {
  std::vector<float> frame(NetDSP::SHADOW_PIXEL_COUNT, 0.0f);
  for (uint32_t y = 0; y < NetDSP::SHADOW_FRAME_HEIGHT; ++y) {
    for (uint32_t x = 0; x < NetDSP::SHADOW_FRAME_WIDTH; ++x) {
      const float fx =
          static_cast<float>(x) / static_cast<float>(NetDSP::SHADOW_FRAME_WIDTH);
      const float fy = static_cast<float>(y) /
                       static_cast<float>(NetDSP::SHADOW_FRAME_HEIGHT);
      const float value = 0.5f + 0.2f * std::sin(12.0f * fx + phase_shift) +
                          0.2f * std::cos(7.0f * fy + 0.5f * phase_shift);
      frame[static_cast<size_t>(y) * NetDSP::SHADOW_FRAME_WIDTH + x] =
          std::clamp(value, 0.0f, 1.0f);
    }
  }
  return frame;
}

std::vector<float> make_warped_wave_frame(float phase_shift) {
  std::vector<float> frame(NetDSP::SHADOW_PIXEL_COUNT, 0.0f);
  for (uint32_t y = 0; y < NetDSP::SHADOW_FRAME_HEIGHT; ++y) {
    for (uint32_t x = 0; x < NetDSP::SHADOW_FRAME_WIDTH; ++x) {
      const float fx =
          static_cast<float>(x) / static_cast<float>(NetDSP::SHADOW_FRAME_WIDTH);
      const float fy = static_cast<float>(y) /
                       static_cast<float>(NetDSP::SHADOW_FRAME_HEIGHT);
      const float warped_phase = phase_shift + 4.0f * fy * fy;
      const float value = 0.5f + 0.18f * std::sin(16.0f * fx + warped_phase) +
                          0.14f * std::cos(9.0f * fy + 0.7f * warped_phase) +
                          0.05f * std::sin(22.0f * (fx + fy));
      frame[static_cast<size_t>(y) * NetDSP::SHADOW_FRAME_WIDTH + x] =
          std::clamp(value, 0.0f, 1.0f);
    }
  }
  return frame;
}

double mean_abs_error_for_rows(const std::vector<float> &lhs,
                               const std::vector<float> &rhs,
                               uint32_t start_row, uint32_t row_count) {
  double error = 0.0;
  size_t samples = 0;
  for (uint32_t row = start_row; row < start_row + row_count; ++row) {
    const size_t offset = static_cast<size_t>(row) * NetDSP::SHADOW_FRAME_WIDTH;
    for (uint32_t x = 0; x < NetDSP::SHADOW_FRAME_WIDTH; ++x) {
      error += std::fabs(lhs[offset + x] - rhs[offset + x]);
      ++samples;
    }
  }
  return error / static_cast<double>(samples);
}

void test_analytic_concealment_beats_stale_hold() {
  std::print("Test: Analytic Concealment Beats Stale Hold... ");

  const std::vector<float> original = make_wave_frame(0.4f);
  std::vector<float> stale_reference = original;
  std::vector<float> concealed = original;
  NetDSP::ConcealmentWorkspace workspace(NetDSP::SHADOW_FRAME_WIDTH,
                                         NetDSP::SHADOW_FRAME_HEIGHT);

  constexpr uint32_t missing_start = 320;
  constexpr uint32_t missing_rows = 24;

  for (uint32_t row = missing_start; row < missing_start + missing_rows; ++row) {
    const size_t dst = static_cast<size_t>(row) * NetDSP::SHADOW_FRAME_WIDTH;
    const size_t src =
        static_cast<size_t>(missing_start - 1) * NetDSP::SHADOW_FRAME_WIDTH;
    std::copy_n(original.data() + src, NetDSP::SHADOW_FRAME_WIDTH,
                stale_reference.data() + dst);
    std::copy_n(original.data() + src, NetDSP::SHADOW_FRAME_WIDTH,
                concealed.data() + dst);
  }

  const auto report = NetDSP::concealMissingRowsWithAnalyticContinuation(
      concealed.data(), NetDSP::SHADOW_FRAME_WIDTH, NetDSP::SHADOW_FRAME_HEIGHT,
      {NetDSP::ConcealmentSpan{.start_row = missing_start,
                               .row_count = missing_rows}},
      workspace);

  const double stale_mae =
      mean_abs_error_for_rows(stale_reference, original, missing_start, missing_rows);
  const double concealed_mae =
      mean_abs_error_for_rows(concealed, original, missing_start, missing_rows);

  std::println("\n  debug: stale_mae={:.8f} concealed_mae={:.8f} healed_rows={} "
               "elapsed_ms={:.4f}",
               stale_mae, concealed_mae, report.healed_rows, report.elapsed_ms);
  assert(report.healed_rows == missing_rows);
  assert(concealed_mae < stale_mae);

  std::println("PASSED");
}

void test_analytic_concealment_benchmark() {
  std::print("Test: Analytic Concealment Benchmark... ");

  std::vector<float> reference = make_wave_frame(0.9f);
  NetDSP::ConcealmentWorkspace workspace(NetDSP::SHADOW_FRAME_WIDTH,
                                         NetDSP::SHADOW_FRAME_HEIGHT);
  constexpr uint32_t missing_start = 512;
  constexpr uint32_t missing_rows = 32;
  constexpr uint32_t iterations = 16;

  std::vector<double> timings;
  timings.reserve(iterations);

  for (uint32_t iter = 0; iter < iterations; ++iter) {
    std::vector<float> working = reference;
    for (uint32_t row = missing_start; row < missing_start + missing_rows; ++row) {
      const size_t dst = static_cast<size_t>(row) * NetDSP::SHADOW_FRAME_WIDTH;
      const size_t src =
          static_cast<size_t>(missing_start - 1) * NetDSP::SHADOW_FRAME_WIDTH;
      std::copy_n(working.data() + src, NetDSP::SHADOW_FRAME_WIDTH,
                  working.data() + dst);
    }

    const auto start = std::chrono::steady_clock::now();
    const auto report = NetDSP::concealMissingRowsWithAnalyticContinuation(
        working.data(), NetDSP::SHADOW_FRAME_WIDTH, NetDSP::SHADOW_FRAME_HEIGHT,
        {NetDSP::ConcealmentSpan{.start_row = missing_start,
                                 .row_count = missing_rows}},
        workspace);
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
    assert(report.healed_rows == missing_rows);
    timings.push_back(elapsed_ms);
  }

  const double best_ms = *std::min_element(timings.begin(), timings.end());
  const double avg_ms = std::accumulate(timings.begin(), timings.end(), 0.0) /
                        static_cast<double>(timings.size());

  std::println("\n  debug: concealment best_ms={:.4f} avg_ms={:.4f} rows={} width={}",
               best_ms, avg_ms, missing_rows, NetDSP::SHADOW_FRAME_WIDTH);
  assert(std::isfinite(best_ms));
  assert(std::isfinite(avg_ms));

  std::println("PASSED");
}

void test_analytic_concealment_handles_multi_span_loss() {
  std::print("Test: Analytic Concealment Handles Multi-Span Loss... ");

  const std::vector<float> original = make_warped_wave_frame(0.3f);
  std::vector<float> stale_reference = original;
  std::vector<float> concealed = original;
  NetDSP::ConcealmentWorkspace workspace(NetDSP::SHADOW_FRAME_WIDTH,
                                         NetDSP::SHADOW_FRAME_HEIGHT);
  const std::vector<NetDSP::ConcealmentSpan> spans = {
      NetDSP::ConcealmentSpan{.start_row = 180, .row_count = 18},
      NetDSP::ConcealmentSpan{.start_row = 640, .row_count = 22},
  };

  for (const auto span : spans) {
    for (uint32_t row = span.start_row; row < span.start_row + span.row_count;
         ++row) {
      const size_t dst = static_cast<size_t>(row) * NetDSP::SHADOW_FRAME_WIDTH;
      const size_t src =
          static_cast<size_t>(span.start_row - 1) * NetDSP::SHADOW_FRAME_WIDTH;
      std::copy_n(original.data() + src, NetDSP::SHADOW_FRAME_WIDTH,
                  stale_reference.data() + dst);
      std::copy_n(original.data() + src, NetDSP::SHADOW_FRAME_WIDTH,
                  concealed.data() + dst);
    }
  }

  const auto report = NetDSP::concealMissingRowsWithAnalyticContinuation(
      concealed.data(), NetDSP::SHADOW_FRAME_WIDTH, NetDSP::SHADOW_FRAME_HEIGHT,
      spans, workspace);

  const double stale_mae =
      mean_abs_error_for_rows(stale_reference, original, 180, 18) +
      mean_abs_error_for_rows(stale_reference, original, 640, 22);
  const double concealed_mae =
      mean_abs_error_for_rows(concealed, original, 180, 18) +
      mean_abs_error_for_rows(concealed, original, 640, 22);

  std::println("\n  debug: multi_span stale_mae={:.8f} concealed_mae={:.8f} "
               "healed_rows={}",
               stale_mae, concealed_mae, report.healed_rows);
  assert(report.healed_rows == 40);
  assert(concealed_mae < stale_mae);

  std::println("PASSED");
}

} // namespace

int main() {
  std::println("==========================================================");
  std::println("Phase 2 Concealment Tests");
  std::println("==========================================================\n");

  test_analytic_concealment_beats_stale_hold();
  test_analytic_concealment_benchmark();
  test_analytic_concealment_handles_multi_span_loss();

  std::println("\n==========================================================");
  std::println("All Phase 2 concealment tests PASSED!");
  std::println("==========================================================");
  return 0;
}
