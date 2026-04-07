#include "CompressiveReceiverEngine.hpp"
#include "CompressiveSampling.hpp"
#include "SenderEngine.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <print>
#include <vector>

namespace {

std::vector<float> build_phase3_frame(uint32_t frame_index) {
  std::vector<float> frame(NetDSP::SHADOW_PIXEL_COUNT, 0.0f);
  const float phase = static_cast<float>(frame_index) * 0.07f;
  for (uint32_t y = 0; y < NetDSP::SHADOW_FRAME_HEIGHT; ++y) {
    for (uint32_t x = 0; x < NetDSP::SHADOW_FRAME_WIDTH; ++x) {
      const float fx =
          static_cast<float>(x) / static_cast<float>(NetDSP::SHADOW_FRAME_WIDTH);
      const float fy = static_cast<float>(y) /
                       static_cast<float>(NetDSP::SHADOW_FRAME_HEIGHT);
      const float value = 0.5f + 0.18f * std::sin(6.0f * fx + phase) +
                          0.16f * std::cos(5.0f * fy + 0.5f * phase) +
                          0.06f * std::sin(4.0f * (fx + fy));
      frame[static_cast<size_t>(y) * NetDSP::SHADOW_FRAME_WIDTH + x] =
          std::clamp(value, 0.0f, 1.0f);
    }
  }
  return frame;
}

double mean_abs_error(const float *lhs, const float *rhs, size_t count) {
  double error = 0.0;
  for (size_t index = 0; index < count; ++index) {
    error += std::fabs(lhs[index] - rhs[index]);
  }
  return error / static_cast<double>(count);
}

float max_abs_error(const float *lhs, const float *rhs, size_t count) {
  float error = 0.0f;
  for (size_t index = 0; index < count; ++index) {
    error = std::max(error, std::fabs(lhs[index] - rhs[index]));
  }
  return error;
}

struct CapturedFragment {
  NetDSP::PacketHeader header{};
  std::vector<std::byte> payload;
  size_t payload_bytes{0};
};

void test_compressive_payload_reconstructs_smooth_frame() {
  std::print("Test: Phase 3 Compressive Payload Reconstructs Smooth Frame... ");

  const std::vector<float> frame = build_phase3_frame(3);
  const NetDSP::CompressiveSamplingConfig config{};
  NetDSP::CompressiveFrameStats encode_stats{};
  const std::vector<std::byte> payload = NetDSP::encodeCompressiveFramePayload(
      frame.data(), NetDSP::SHADOW_FRAME_WIDTH, NetDSP::SHADOW_FRAME_HEIGHT,
      config, 3, &encode_stats);
  assert(!payload.empty());

  std::vector<float> reconstructed(NetDSP::SHADOW_PIXEL_COUNT, 0.0f);
  NetDSP::CompressiveFrameStats decode_stats{};
  const bool decoded = NetDSP::reconstructCompressiveFramePayload(
      payload.data(), payload.size(), NetDSP::SHADOW_FRAME_WIDTH,
      NetDSP::SHADOW_FRAME_HEIGHT, reconstructed.data(), &decode_stats);
  assert(decoded);

  const double mae = mean_abs_error(frame.data(), reconstructed.data(),
                                    NetDSP::SHADOW_PIXEL_COUNT);
  const float max_abs = max_abs_error(frame.data(), reconstructed.data(),
                                      NetDSP::SHADOW_PIXEL_COUNT);

  std::println("\n  debug: payload_bytes={} sample_ratio={:.4f} "
               "payload_ratio={:.4f} mae={:.6f} max_abs={:.6f} "
               "encode_ms={:.3f} reconstruct_ms={:.3f}",
               encode_stats.payload_bytes, encode_stats.sample_ratio,
               encode_stats.payload_ratio, mae, max_abs,
               encode_stats.elapsed_ms,
               decode_stats.elapsed_ms);
  assert(encode_stats.payload_ratio < 0.12);
  assert(mae < 0.05);
  assert(max_abs < 0.95f);

  std::println("PASSED");
}

void test_compressive_sender_receiver_round_trip() {
  std::print("Test: Phase 3 Sender/Receiver Round Trip... ");

  NetDSP::SenderEngine sender;
  NetDSP::CompressiveReceiverEngine<4, 8> receiver(20000);
  const NetDSP::CompressiveSamplingConfig config{};
  const std::vector<float> frame = build_phase3_frame(7);
  std::vector<CapturedFragment> fragments;

  const bool sent = sender.sendCompressiveFrame(
      frame.data(), 904, 990000, config,
      [&](const NetDSP::PacketHeader &header, const std::byte *payload,
          size_t payload_bytes) {
        fragments.push_back(CapturedFragment{
            .header = header,
            .payload = std::vector<std::byte>(payload, payload + payload_bytes),
            .payload_bytes = payload_bytes,
        });
        return true;
      });
  assert(sent);
  assert(!fragments.empty());

  std::reverse(fragments.begin(), fragments.end());
  for (auto it = fragments.begin(); it != fragments.end(); ++it) {
    const auto result = receiver.onPacket(it->header, it->payload.data(),
                                          it->payload_bytes, 1000000);
    if (it + 1 == fragments.end()) {
      assert(result.status == NetDSP::PacketStatus::FrameCompleted);
    } else {
      assert(result.status == NetDSP::PacketStatus::AcceptedFragment);
    }
  }

  const auto ready = receiver.tryAcquireReadyFrame();
  assert(ready.has_value());
  assert(ready->descriptor.frame_id == 904);
  assert(NetDSP::hasFlag(ready->descriptor.type_flags,
                         NetDSP::FLAG_COMPRESSIVE_SAMPLING));
  assert(!ready->descriptor.usesTemporalRefresh());

  const double mae = mean_abs_error(ready->pixels, frame.data(),
                                    NetDSP::SHADOW_PIXEL_COUNT);
  const float max_abs =
      max_abs_error(ready->pixels, frame.data(), NetDSP::SHADOW_PIXEL_COUNT);
  std::println("\n  debug: ready_bytes={} fragments={} mae={:.6f} max_abs={:.6f}",
               ready->descriptor.bytes_used, ready->descriptor.total_fragments,
               mae, max_abs);
  assert(mae < 0.05);
  assert(max_abs < 0.95f);

  receiver.releaseReadyFrame(ready->slot_index);
  std::println("PASSED");
}

} // namespace

int main() {
  std::println("==========================================================");
  std::println("Phase 3 Compressive Sampling Tests");
  std::println("==========================================================\n");

  test_compressive_payload_reconstructs_smooth_frame();
  test_compressive_sender_receiver_round_trip();

  std::println("\n==========================================================");
  std::println("All Phase 3 compressive sampling tests PASSED!");
  std::println("==========================================================");
  return 0;
}
