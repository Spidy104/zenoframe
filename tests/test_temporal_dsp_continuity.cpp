#include "ImageEngine.hpp"
#include "ReceiverEngine.hpp"
#include "SenderEngine.hpp"
#include "TemporalRefresh.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <print>
#include <vector>

namespace {

struct CapturedTemporalFragment {
  NetDSP::PacketHeader header{};
  std::vector<std::byte> payload;
  size_t payload_bytes{0};
};

std::vector<float> make_phase2_frame(uint32_t frame_index) {
  std::vector<float> frame(NetDSP::SHADOW_PIXEL_COUNT, 0.0f);
  const float phase = static_cast<float>(frame_index) * 0.15f;
  for (uint32_t y = 0; y < NetDSP::SHADOW_FRAME_HEIGHT; ++y) {
    for (uint32_t x = 0; x < NetDSP::SHADOW_FRAME_WIDTH; ++x) {
      const float fx =
          static_cast<float>(x) / static_cast<float>(NetDSP::SHADOW_FRAME_WIDTH);
      const float fy = static_cast<float>(y) /
                       static_cast<float>(NetDSP::SHADOW_FRAME_HEIGHT);
      frame[static_cast<size_t>(y) * NetDSP::SHADOW_FRAME_WIDTH + x] =
          std::clamp(0.45f + 0.2f * std::sin(10.0f * fx + phase) +
                         0.2f * std::cos(6.0f * fy + 0.5f * phase),
                     0.0f, 1.0f);
    }
  }
  return frame;
}

std::vector<CapturedTemporalFragment>
capture_phase1_frame(NetDSP::SenderEngine &sender, const std::vector<float> &frame,
                     uint16_t frame_id, uint64_t timestamp_us,
                     uint64_t refresh_frame_index,
                     const NetDSP::DistributedIntraRefreshScheduler &scheduler,
                     bool seed_with_full_frame) {
  std::vector<CapturedTemporalFragment> fragments;
  const bool sent = sender.sendDistributedIntraRefreshFrame(
      frame.data(), frame_id, timestamp_us, refresh_frame_index, scheduler,
      [&](const NetDSP::PacketHeader &header, const std::byte *payload,
          size_t payload_bytes) {
        fragments.push_back(CapturedTemporalFragment{
            .header = header,
            .payload = std::vector<std::byte>(payload, payload + payload_bytes),
            .payload_bytes = payload_bytes,
        });
        return true;
      },
      seed_with_full_frame);
  assert(sent);
  return fragments;
}

double mean_abs_error(const float *lhs, const float *rhs, size_t count) {
  double error = 0.0;
  for (size_t index = 0; index < count; ++index) {
    error += std::fabs(static_cast<double>(lhs[index]) -
                       static_cast<double>(rhs[index]));
  }
  return error / static_cast<double>(count);
}

dsp::Image fused_from_pixels(const float *pixels) {
  dsp::Image image(NetDSP::SHADOW_FRAME_WIDTH, NetDSP::SHADOW_FRAME_HEIGHT);
  std::memcpy(image.data(), pixels, NetDSP::SHADOW_BUFFER_BYTES);
  return dsp::blurAndSobelFused(image);
}

template <size_t PoolSize, size_t ReadyQueueCapacity>
std::optional<NetDSP::FrameDescriptor>
drain_single_ready(NetDSP::ReceiverEngine<PoolSize, ReadyQueueCapacity> &receiver) {
  const auto ready = receiver.tryAcquireReadyFrame();
  if (!ready.has_value()) {
    return std::nullopt;
  }

  const NetDSP::FrameDescriptor descriptor = ready->descriptor;
  receiver.releaseReadyFrame(ready->slot_index);
  assert(!receiver.tryAcquireReadyFrame().has_value());
  return descriptor;
}

bool fragment_overlaps_missing_payload_rows(const CapturedTemporalFragment &fragment,
                                            uint32_t missing_payload_row_start,
                                            uint32_t missing_payload_row_count) {
  const size_t row_bytes =
      static_cast<size_t>(NetDSP::SHADOW_FRAME_WIDTH) * sizeof(float);
  const size_t missing_begin =
      static_cast<size_t>(missing_payload_row_start) * row_bytes;
  const size_t missing_end =
      static_cast<size_t>(missing_payload_row_start + missing_payload_row_count) *
      row_bytes;
  const size_t fragment_begin =
      static_cast<size_t>(fragment.header.fragment_index) *
      NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES;
  const size_t fragment_end = fragment_begin + fragment.payload_bytes;
  return fragment_begin < missing_end && fragment_end > missing_begin;
}

bool should_induce_loss(uint32_t frame_index) {
  return frame_index % 4u == 0u || frame_index % 7u == 3u;
}

void test_main_receiver_dsp_continuity_prefers_phase_concealment() {
  std::print("Test: Main Receiver DSP Continuity Prefers Phase Concealment... ");

  NetDSP::SenderEngine sender;
  const NetDSP::DistributedIntraRefreshScheduler scheduler(
      NetDSP::SHADOW_FRAME_HEIGHT, 32);
  NetDSP::ReceiverEngine<4, 16> truth_receiver(
      5000, NetDSP::TimeoutPolicy::DropPartial, 0.0f);
  NetDSP::ReceiverEngine<4, 16> stale_receiver(
      5000, NetDSP::TimeoutPolicy::DropPartial, 0.0f);
  NetDSP::ReceiverEngine<4, 16> conceal_receiver(
      5000, NetDSP::TimeoutPolicy::ForceCommitPartial, 0.0f);

  const std::vector<float> seed_frame = make_phase2_frame(0);
  auto seed_fragments = capture_phase1_frame(
      sender, seed_frame, 1200, 100000, 0, scheduler, true);
  std::reverse(seed_fragments.begin(), seed_fragments.end());

  for (size_t index = 0; index < seed_fragments.size(); ++index) {
    const auto &fragment = seed_fragments[index];
    const uint64_t arrival_time = 200000;
    assert(truth_receiver
               .onPacket(fragment.header, fragment.payload.data(),
                         fragment.payload_bytes, arrival_time)
               .accepted());
    assert(stale_receiver
               .onPacket(fragment.header, fragment.payload.data(),
                         fragment.payload_bytes, arrival_time)
               .accepted());
    assert(conceal_receiver
               .onPacket(fragment.header, fragment.payload.data(),
                         fragment.payload_bytes, arrival_time)
               .accepted());
  }

  const auto truth_seed = drain_single_ready(truth_receiver);
  const auto stale_seed = drain_single_ready(stale_receiver);
  const auto conceal_seed = drain_single_ready(conceal_receiver);
  assert(truth_seed.has_value());
  assert(stale_seed.has_value());
  assert(conceal_seed.has_value());

  constexpr uint32_t temporal_frames = 24;
  constexpr uint32_t missing_payload_rows = 4;
  size_t induced_losses = 0;
  size_t stale_drops = 0;
  size_t concealed_partials = 0;
  double stale_pixel_mae_sum = 0.0;
  double conceal_pixel_mae_sum = 0.0;
  double stale_dsp_mae_sum = 0.0;
  double conceal_dsp_mae_sum = 0.0;

  const auto benchmark_start = std::chrono::steady_clock::now();

  for (uint32_t frame_index = 1; frame_index <= temporal_frames; ++frame_index) {
    const std::vector<float> frame = make_phase2_frame(frame_index);
    auto fragments = capture_phase1_frame(
        sender, frame, static_cast<uint16_t>(1200 + frame_index),
        101000 + frame_index * 1000u, frame_index, scheduler, false);
    std::reverse(fragments.begin(), fragments.end());

    const bool induce_loss = should_induce_loss(frame_index);
    const uint32_t missing_payload_row_start =
        4u + (frame_index * 3u) % (scheduler.rowsPerFrame() - 8u);

    for (size_t fragment_index = 0; fragment_index < fragments.size();
         ++fragment_index) {
      const auto &fragment = fragments[fragment_index];
      const uint64_t arrival_time =
          300000 + static_cast<uint64_t>(frame_index) * 50000u + fragment_index;

      assert(truth_receiver
                 .onPacket(fragment.header, fragment.payload.data(),
                           fragment.payload_bytes, arrival_time)
                 .accepted());

      const bool drop_for_loss =
          induce_loss &&
          fragment_overlaps_missing_payload_rows(fragment,
                                                missing_payload_row_start,
                                                missing_payload_rows);
      if (drop_for_loss) {
        continue;
      }

      assert(stale_receiver
                 .onPacket(fragment.header, fragment.payload.data(),
                           fragment.payload_bytes, arrival_time)
                 .accepted());
      assert(conceal_receiver
                 .onPacket(fragment.header, fragment.payload.data(),
                           fragment.payload_bytes, arrival_time)
                 .accepted());
    }

    const uint64_t sweep_time =
        300000 + static_cast<uint64_t>(frame_index) * 50000u +
        conceal_receiver.frameTimeoutUs() + 128u;
    const auto truth_sweep = truth_receiver.pollExpiredFrames(sweep_time);
    const auto stale_sweep = stale_receiver.pollExpiredFrames(sweep_time);
    const auto conceal_sweep = conceal_receiver.pollExpiredFrames(sweep_time);

    assert(truth_sweep.committed == 0);
    assert(truth_sweep.dropped == 0);

    const auto truth_ready = drain_single_ready(truth_receiver);
    const auto stale_ready = drain_single_ready(stale_receiver);
    const auto conceal_ready = drain_single_ready(conceal_receiver);

    assert(truth_ready.has_value());
    assert(conceal_ready.has_value());
    assert(!truth_ready->isPartial());

    if (induce_loss) {
      ++induced_losses;
      assert(!stale_ready.has_value());
      assert(stale_sweep.dropped == 1);
      assert(stale_sweep.committed == 0);
      assert(conceal_sweep.committed == 1);
      assert(conceal_sweep.dropped == 0);
      assert(conceal_ready->isPartial());
      ++stale_drops;
      ++concealed_partials;

      const double stale_pixel_mae = mean_abs_error(
          stale_receiver.reference().data(), truth_receiver.reference().data(),
          NetDSP::SHADOW_PIXEL_COUNT);
      const double conceal_pixel_mae = mean_abs_error(
          conceal_receiver.reference().data(), truth_receiver.reference().data(),
          NetDSP::SHADOW_PIXEL_COUNT);

      const dsp::Image truth_dsp = fused_from_pixels(truth_receiver.reference().data());
      const dsp::Image stale_dsp = fused_from_pixels(stale_receiver.reference().data());
      const dsp::Image conceal_dsp =
          fused_from_pixels(conceal_receiver.reference().data());

      const double stale_dsp_mae =
          mean_abs_error(stale_dsp.data(), truth_dsp.data(), truth_dsp.size());
      const double conceal_dsp_mae =
          mean_abs_error(conceal_dsp.data(), truth_dsp.data(), truth_dsp.size());

      stale_pixel_mae_sum += stale_pixel_mae;
      conceal_pixel_mae_sum += conceal_pixel_mae;
      stale_dsp_mae_sum += stale_dsp_mae;
      conceal_dsp_mae_sum += conceal_dsp_mae;
      continue;
    }

    assert(stale_ready.has_value());
    assert(stale_sweep.dropped == 0);
    assert(stale_sweep.committed == 0);
    assert(conceal_sweep.dropped == 0);
    assert(conceal_sweep.committed == 0);
    assert(!stale_ready->isPartial());
    assert(!conceal_ready->isPartial());
  }

  const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() -
                                benchmark_start)
                                .count();
  const double avg_stale_pixel_mae =
      stale_pixel_mae_sum / static_cast<double>(induced_losses);
  const double avg_conceal_pixel_mae =
      conceal_pixel_mae_sum / static_cast<double>(induced_losses);
  const double avg_stale_dsp_mae =
      stale_dsp_mae_sum / static_cast<double>(induced_losses);
  const double avg_conceal_dsp_mae =
      conceal_dsp_mae_sum / static_cast<double>(induced_losses);

  std::println(
      "\n  debug: temporal_frames={} induced_losses={} stale_drops={} "
      "concealed_partials={} avg_stale_pixel_mae={:.8f} "
      "avg_conceal_pixel_mae={:.8f} avg_stale_dsp_mae={:.8f} "
      "avg_conceal_dsp_mae={:.8f} elapsed_ms={:.3f} avg_ms_per_frame={:.3f}",
      temporal_frames, induced_losses, stale_drops, concealed_partials,
      avg_stale_pixel_mae, avg_conceal_pixel_mae, avg_stale_dsp_mae,
      avg_conceal_dsp_mae, elapsed_ms,
      elapsed_ms / static_cast<double>(temporal_frames));

  assert(induced_losses > 0);
  assert(stale_drops == induced_losses);
  assert(concealed_partials == induced_losses);
  assert(avg_conceal_pixel_mae < avg_stale_pixel_mae);
  assert(avg_conceal_dsp_mae < avg_stale_dsp_mae);

  std::println("PASSED");
}

} // namespace

int main() {
  std::println("==========================================================");
  std::println("Temporal DSP Continuity Tests");
  std::println("==========================================================\n");

  test_main_receiver_dsp_continuity_prefers_phase_concealment();

  std::println("\n==========================================================");
  std::println("All temporal DSP continuity tests PASSED!");
  std::println("==========================================================");
  return 0;
}
