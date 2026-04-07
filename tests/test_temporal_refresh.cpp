#include "TemporalRefresh.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <print>
#include <vector>

namespace {

std::vector<float> make_frame(uint32_t width, uint32_t height, float seed) {
  std::vector<float> frame(static_cast<size_t>(width) * height, 0.0f);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      frame[static_cast<size_t>(y) * width + x] =
          seed + static_cast<float>(y * width + x);
    }
  }
  return frame;
}

void test_dir_schedule_without_wrap() {
  std::print("Test: DIR Schedule Without Wrap... ");

  const NetDSP::DistributedIntraRefreshScheduler scheduler(12, 4);
  assert(scheduler.fullCycleFrameCount() == 3);

  const auto first = scheduler.planForFrame(0);
  assert(first.span_count == 1);
  assert(first.spans[0].start_row == 0);
  assert(first.spans[0].row_count == 4);

  const auto second = scheduler.planForFrame(1);
  assert(second.span_count == 1);
  assert(second.spans[0].start_row == 4);
  assert(second.spans[0].row_count == 4);

  const auto third = scheduler.planForFrame(2);
  assert(third.span_count == 1);
  assert(third.spans[0].start_row == 8);
  assert(third.spans[0].row_count == 4);

  std::array<uint32_t, 12> row_hits{};
  for (uint64_t frame_index = 0; frame_index < scheduler.fullCycleFrameCount();
       ++frame_index) {
    const auto plan = scheduler.planForFrame(frame_index);
    for (uint32_t row = 0; row < scheduler.frameHeight(); ++row) {
      if (plan.coversRow(row)) {
        ++row_hits[row];
      }
    }
  }

  for (const uint32_t hits : row_hits) {
    assert(hits == 1);
  }

  std::println("PASSED");
}

void test_dir_schedule_with_wrap_and_full_coverage() {
  std::print("Test: DIR Schedule Wrap + Full Coverage... ");

  const NetDSP::DistributedIntraRefreshScheduler scheduler(10, 4);
  assert(scheduler.fullCycleFrameCount() == 3);

  const auto wrapped = scheduler.planForFrame(2);
  assert(wrapped.span_count == 2);
  assert(wrapped.spans[0].start_row == 8);
  assert(wrapped.spans[0].row_count == 2);
  assert(wrapped.spans[1].start_row == 0);
  assert(wrapped.spans[1].row_count == 2);

  std::array<uint32_t, 10> row_hits{};
  for (uint64_t frame_index = 0; frame_index < scheduler.fullCycleFrameCount();
       ++frame_index) {
    const auto plan = scheduler.planForFrame(frame_index);
    for (uint32_t row = 0; row < scheduler.frameHeight(); ++row) {
      if (plan.coversRow(row)) {
        ++row_hits[row];
      }
    }
  }

  for (uint32_t row = 0; row < scheduler.frameHeight(); ++row) {
    assert(row_hits[row] >= 1);
  }
  assert(row_hits[0] == 2);
  assert(row_hits[1] == 2);

  std::println("PASSED");
}

void test_reconstructor_updates_only_selected_rows() {
  std::print("Test: Reconstructor Updates Only Selected Rows... ");

  constexpr uint32_t width = 6;
  constexpr uint32_t height = 10;
  NetDSP::DistributedIntraRefreshScheduler scheduler(height, 4);
  NetDSP::TemporalRefreshReconstructor reconstructor(width, height, -1.0f);

  const auto source_a = make_frame(width, height, 100.0f);
  const auto source_b = make_frame(width, height, 500.0f);

  const auto plan0 = scheduler.planForFrame(0);
  reconstructor.applyRefreshPlan(plan0, source_a.data());

  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      if (y < 4) {
        assert(reconstructor.at(x, y) == source_a[static_cast<size_t>(y) * width + x]);
      } else {
        assert(reconstructor.at(x, y) == -1.0f);
      }
    }
  }

  const auto wrapped = scheduler.planForFrame(2);
  reconstructor.applyRefreshPlan(wrapped, source_b.data());

  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const float value = reconstructor.at(x, y);
      if (y <= 1 || y >= 8) {
        assert(value == source_b[static_cast<size_t>(y) * width + x]);
      } else if (y <= 3) {
        assert(value == source_a[static_cast<size_t>(y) * width + x]);
      } else {
        assert(value == -1.0f);
      }
    }
  }

  std::println("PASSED");
}

void test_reconstructor_duplicate_apply_is_idempotent() {
  std::print("Test: Reconstructor Duplicate Apply Is Idempotent... ");

  constexpr uint32_t width = 4;
  constexpr uint32_t height = 8;
  NetDSP::DistributedIntraRefreshScheduler scheduler(height, 3);
  NetDSP::TemporalRefreshReconstructor reconstructor(width, height, 0.0f);
  const auto source = make_frame(width, height, 42.0f);
  const auto plan = scheduler.planForFrame(1);

  reconstructor.applyRefreshPlan(plan, source.data());
  const std::vector<float> first_pass(reconstructor.data(),
                                      reconstructor.data() + width * height);
  reconstructor.applyRefreshPlan(plan, source.data());

  for (size_t index = 0; index < first_pass.size(); ++index) {
    assert(reconstructor.data()[index] == first_pass[index]);
  }

  std::println("PASSED");
}

void test_row_window_payload_is_tight_and_ordered() {
  std::print("Test: Row-Window Payload Is Tight And Ordered... ");

  constexpr uint32_t width = 5;
  constexpr uint32_t height = 10;
  NetDSP::DistributedIntraRefreshScheduler scheduler(height, 4);
  const auto source = make_frame(width, height, 10.0f);
  const auto wrapped = scheduler.planForFrame(2);
  const auto layout = NetDSP::makeRefreshPayloadLayout(wrapped, width, 32);
  const auto payload = NetDSP::extractRefreshPayload(wrapped, source.data(), width);

  assert(layout.slice_count == 2);
  assert(layout.total_payload_rows == 4);
  assert(layout.totalPayloadBytes() == 4u * width * sizeof(float));
  assert(layout.slices[0].start_row == 8);
  assert(layout.slices[0].row_count == 2);
  assert(layout.slices[0].payload_offset_rows == 0);
  assert(layout.slices[1].start_row == 0);
  assert(layout.slices[1].row_count == 2);
  assert(layout.slices[1].payload_offset_rows == 2);

  for (uint32_t row = 0; row < 2; ++row) {
    for (uint32_t x = 0; x < width; ++x) {
      assert(payload[static_cast<size_t>(row) * width + x] ==
             source[static_cast<size_t>(8 + row) * width + x]);
      assert(payload[static_cast<size_t>(row + 2) * width + x] ==
             source[static_cast<size_t>(row) * width + x]);
    }
  }

  NetDSP::TemporalRefreshReconstructor reconstructor(width, height, -1.0f);
  reconstructor.applyRefreshPayload(layout, payload.data());
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      if (y <= 1 || y >= 8) {
        assert(reconstructor.at(x, y) == source[static_cast<size_t>(y) * width + x]);
      } else {
        assert(reconstructor.at(x, y) == -1.0f);
      }
    }
  }

  std::println("PASSED");
}

void test_phase_healing_preserves_stale_rows_until_refresh() {
  std::print("Test: Phase Healing Preserves Stale Rows Until Refresh... ");

  constexpr uint32_t width = 4;
  constexpr uint32_t height = 8;
  NetDSP::DistributedIntraRefreshScheduler scheduler(height, 2);
  NetDSP::TemporalRefreshReconstructor reconstructor(width, height, -1.0f);

  const auto frame0 = make_frame(width, height, 100.0f);
  const auto frame1 = make_frame(width, height, 500.0f);
  const auto frame2 = make_frame(width, height, 900.0f);
  const auto frame3 = make_frame(width, height, 1300.0f);
  const auto frame4 = make_frame(width, height, 1700.0f);

  reconstructor.applyRefreshPayload(
      NetDSP::makeRefreshPayloadLayout(scheduler.planForFrame(0), width, 32),
      NetDSP::extractRefreshPayload(scheduler.planForFrame(0), frame0.data(), width)
          .data());
  reconstructor.applyRefreshPayload(
      NetDSP::makeRefreshPayloadLayout(scheduler.planForFrame(1), width, 32),
      NetDSP::extractRefreshPayload(scheduler.planForFrame(1), frame1.data(), width)
          .data());
  reconstructor.applyRefreshPayload(
      NetDSP::makeRefreshPayloadLayout(scheduler.planForFrame(3), width, 32),
      NetDSP::extractRefreshPayload(scheduler.planForFrame(3), frame3.data(), width)
          .data());

  for (uint32_t x = 0; x < width; ++x) {
    assert(reconstructor.at(x, 0) == frame0[x]);
    assert(reconstructor.at(x, 1) == frame0[width + x]);
    assert(reconstructor.at(x, 2) == frame1[2 * width + x]);
    assert(reconstructor.at(x, 3) == frame1[3 * width + x]);
    assert(reconstructor.at(x, 4) == -1.0f);
    assert(reconstructor.at(x, 5) == -1.0f);
    assert(reconstructor.at(x, 6) == frame3[6 * width + x]);
    assert(reconstructor.at(x, 7) == frame3[7 * width + x]);
  }

  const auto healing_plan = scheduler.planForFrame(6);
  const auto healing_payload =
      NetDSP::extractRefreshPayload(healing_plan, frame4.data(), width);
  reconstructor.applyRefreshPayload(
      NetDSP::makeRefreshPayloadLayout(healing_plan, width, 32),
      healing_payload.data());

  for (uint32_t x = 0; x < width; ++x) {
    assert(reconstructor.at(x, 4) == frame4[4 * width + x]);
    assert(reconstructor.at(x, 5) == frame4[5 * width + x]);
    assert(reconstructor.at(x, 0) == frame0[x]);
    assert(reconstructor.at(x, 2) == frame1[2 * width + x]);
    assert(reconstructor.at(x, 6) == frame3[6 * width + x]);
  }

  const auto late_cycle_plan = scheduler.planForFrame(4);
  const auto late_cycle_payload =
      NetDSP::extractRefreshPayload(late_cycle_plan, frame2.data(), width);
  reconstructor.applyRefreshPayload(
      NetDSP::makeRefreshPayloadLayout(late_cycle_plan, width, 32),
      late_cycle_payload.data());

  for (uint32_t x = 0; x < width; ++x) {
    assert(reconstructor.at(x, 0) == frame2[x]);
    assert(reconstructor.at(x, 1) == frame2[width + x]);
    assert(reconstructor.at(x, 4) == frame4[4 * width + x]);
    assert(reconstructor.at(x, 6) == frame3[6 * width + x]);
  }

  std::println("PASSED");
}

} // namespace

int main() {
  std::println("==========================================================");
  std::println("Temporal Refresh Tests");
  std::println("==========================================================\n");

  test_dir_schedule_without_wrap();
  test_dir_schedule_with_wrap_and_full_coverage();
  test_reconstructor_updates_only_selected_rows();
  test_reconstructor_duplicate_apply_is_idempotent();
  test_row_window_payload_is_tight_and_ordered();
  test_phase_healing_preserves_stale_rows_until_refresh();

  std::println("\n==========================================================");
  std::println("All temporal refresh tests PASSED!");
  std::println("==========================================================");
  return 0;
}