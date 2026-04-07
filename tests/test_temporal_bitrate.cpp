#include "FrameBufferPool.hpp"
#include "Protocol.hpp"
#include "SenderEngine.hpp"
#include "TemporalRefresh.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <print>
#include <vector>

namespace {

struct FrameTraffic {
  size_t packets{0};
  size_t bytes{0};
};

struct TrafficStats {
  double mean_packets{0.0};
  double stddev_packets{0.0};
  size_t max_packets{0};
  size_t min_packets{0};
  double mean_bytes{0.0};
  double stddev_bytes{0.0};
  size_t max_bytes{0};
  size_t min_bytes{0};
};

std::vector<float> make_frame(float seed) {
  std::vector<float> frame(NetDSP::SHADOW_PIXEL_COUNT, 0.0f);
  for (uint32_t y = 0; y < NetDSP::SHADOW_FRAME_HEIGHT; ++y) {
    for (uint32_t x = 0; x < NetDSP::SHADOW_FRAME_WIDTH; ++x) {
      const float fx = static_cast<float>(x) / static_cast<float>(NetDSP::SHADOW_FRAME_WIDTH);
      const float fy = static_cast<float>(y) / static_cast<float>(NetDSP::SHADOW_FRAME_HEIGHT);
      frame[static_cast<size_t>(y) * NetDSP::SHADOW_FRAME_WIDTH + x] =
          std::clamp(seed + 0.5f * fx + 0.5f * fy, 0.0f, 1.0f);
    }
  }
  return frame;
}

TrafficStats compute_stats(const std::vector<FrameTraffic> &traffic) {
  TrafficStats stats{};
  assert(!traffic.empty());

  stats.min_packets = traffic.front().packets;
  stats.max_packets = traffic.front().packets;
  stats.min_bytes = traffic.front().bytes;
  stats.max_bytes = traffic.front().bytes;

  for (const auto &sample : traffic) {
    stats.mean_packets += static_cast<double>(sample.packets);
    stats.mean_bytes += static_cast<double>(sample.bytes);
    stats.min_packets = std::min(stats.min_packets, sample.packets);
    stats.max_packets = std::max(stats.max_packets, sample.packets);
    stats.min_bytes = std::min(stats.min_bytes, sample.bytes);
    stats.max_bytes = std::max(stats.max_bytes, sample.bytes);
  }

  stats.mean_packets /= static_cast<double>(traffic.size());
  stats.mean_bytes /= static_cast<double>(traffic.size());

  for (const auto &sample : traffic) {
    const double dp = static_cast<double>(sample.packets) - stats.mean_packets;
    const double db = static_cast<double>(sample.bytes) - stats.mean_bytes;
    stats.stddev_packets += dp * dp;
    stats.stddev_bytes += db * db;
  }

  stats.stddev_packets =
      std::sqrt(stats.stddev_packets / static_cast<double>(traffic.size()));
  stats.stddev_bytes =
      std::sqrt(stats.stddev_bytes / static_cast<double>(traffic.size()));
  return stats;
}

void test_temporal_refresh_reduces_i_frame_spike_variance() {
  std::print("Test: Temporal Refresh Reduces I-Frame Spike Variance... ");

  constexpr uint32_t frames = 72;
  constexpr uint32_t dir_rows_per_frame = 32;
  constexpr uint32_t periodic_residual_rows = 4;

  NetDSP::SenderEngine periodic_sender;
  NetDSP::SenderEngine dir_sender;
  const NetDSP::DistributedIntraRefreshScheduler dir_scheduler(
      NetDSP::SHADOW_FRAME_HEIGHT, dir_rows_per_frame);
  const NetDSP::DistributedIntraRefreshScheduler periodic_residual_scheduler(
      NetDSP::SHADOW_FRAME_HEIGHT, periodic_residual_rows);

  const uint64_t cycle = dir_scheduler.fullCycleFrameCount();

  std::vector<FrameTraffic> periodic_traffic;
  std::vector<FrameTraffic> dir_traffic;
  periodic_traffic.reserve(frames);
  dir_traffic.reserve(frames);

  for (uint32_t frame_index = 0; frame_index < frames; ++frame_index) {
    const auto frame = make_frame(static_cast<float>(frame_index % 8) * 0.05f);

    FrameTraffic periodic_sample{};
    if (frame_index % cycle == 0) {
      const bool sent = periodic_sender.sendFrame(
          frame.data(), static_cast<uint16_t>(2000 + frame_index),
          1000000 + frame_index,
          [&](const NetDSP::PacketHeader &, const std::byte *, size_t payload_bytes) {
            ++periodic_sample.packets;
            periodic_sample.bytes += NetDSP::HEADER_SIZE + payload_bytes;
            return true;
          });
      assert(sent);
    } else {
      const auto plan = periodic_residual_scheduler.planForFrame(frame_index);
      const bool sent = periodic_sender.sendTemporalRefresh(
          frame.data(), static_cast<uint16_t>(2000 + frame_index),
          1000000 + frame_index, plan,
          [&](const NetDSP::PacketHeader &, const std::byte *, size_t payload_bytes) {
            ++periodic_sample.packets;
            periodic_sample.bytes += NetDSP::HEADER_SIZE + payload_bytes;
            return true;
          });
      assert(sent);
    }
    periodic_traffic.push_back(periodic_sample);

    FrameTraffic dir_sample{};
    const auto dir_plan = dir_scheduler.planForFrame(frame_index);
    const bool dir_sent = dir_sender.sendTemporalRefresh(
        frame.data(), static_cast<uint16_t>(3000 + frame_index),
        2000000 + frame_index, dir_plan,
        [&](const NetDSP::PacketHeader &, const std::byte *, size_t payload_bytes) {
          ++dir_sample.packets;
          dir_sample.bytes += NetDSP::HEADER_SIZE + payload_bytes;
          return true;
        });
    assert(dir_sent);
    dir_traffic.push_back(dir_sample);
  }

  const TrafficStats periodic = compute_stats(periodic_traffic);
  const TrafficStats dir = compute_stats(dir_traffic);

  std::println("\n  debug periodic: packets mean={:.2f} sd={:.2f} min={} max={} "
               "bytes mean={:.2f} sd={:.2f} min={} max={}",
               periodic.mean_packets, periodic.stddev_packets,
               periodic.min_packets, periodic.max_packets, periodic.mean_bytes,
               periodic.stddev_bytes, periodic.min_bytes, periodic.max_bytes);
  std::println("  debug dir:      packets mean={:.2f} sd={:.2f} min={} max={} "
               "bytes mean={:.2f} sd={:.2f} min={} max={}",
               dir.mean_packets, dir.stddev_packets, dir.min_packets,
               dir.max_packets, dir.mean_bytes, dir.stddev_bytes,
               dir.min_bytes, dir.max_bytes);

  assert(dir.max_packets < periodic.max_packets);
  assert(dir.stddev_packets < periodic.stddev_packets);
  assert(dir.max_bytes < periodic.max_bytes);
  assert(dir.stddev_bytes < periodic.stddev_bytes);

  std::println("PASSED");
}

} // namespace

int main() {
  std::println("==========================================================");
  std::println("Temporal Bitrate Tests");
  std::println("==========================================================\n");

  test_temporal_refresh_reduces_i_frame_spike_variance();

  std::println("\n==========================================================");
  std::println("All temporal bitrate tests PASSED!");
  std::println("==========================================================");
  return 0;
}