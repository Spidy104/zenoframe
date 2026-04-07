#include "CompressiveReceiverEngine.hpp"
#include "CompressiveSampling.hpp"
#include "ImageEngine.hpp"
#include "Protocol.hpp"
#include "ReceiverEngine.hpp"
#include "SenderEngine.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <immintrin.h>
#include <limits>
#include <netinet/in.h>
#include <numeric>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

enum class TransportMode : uint8_t {
  FullFrame = 0,
  DistributedIntraRefresh = 1,
  CompressiveSampling = 2,
};

enum class TransportBackend : uint8_t {
  UdpLoopback = 0,
  InProcess = 1,
};

class UdpSocket {
  int fd_{-1};

public:
  explicit UdpSocket(int fd = -1) : fd_(fd) {}

  UdpSocket(const UdpSocket &) = delete;
  UdpSocket &operator=(const UdpSocket &) = delete;

  UdpSocket(UdpSocket &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  UdpSocket &operator=(UdpSocket &&other) noexcept {
    if (this != &other) {
      reset();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  ~UdpSocket() { reset(); }

  [[nodiscard]] bool valid() const { return fd_ >= 0; }
  [[nodiscard]] int get() const { return fd_; }

  void reset() {
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }
};

UdpSocket create_udp_socket() { return UdpSocket(socket(AF_INET, SOCK_DGRAM, 0)); }

bool set_receive_timeout(int fd, int timeout_ms) {
  timeval timeout{};
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
  return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
         0;
}

bool set_receive_buffer_size(int fd, int bytes) {
  return setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) == 0;
}

sockaddr_in loopback_address(uint16_t port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  return address;
}

uint16_t bind_ephemeral_loopback(int fd) {
  const sockaddr_in address = loopback_address(0);
  if (bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) !=
      0) {
    return 0;
  }

  sockaddr_in bound{};
  socklen_t length = sizeof(bound);
  if (getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &length) != 0) {
    return 0;
  }

  return ntohs(bound.sin_port);
}

uint64_t monotonic_us() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

bool should_log_fragment(uint16_t fragment_index, uint16_t total_fragments) {
  return fragment_index < 3 || fragment_index + 3 >= total_fragments ||
         (fragment_index % 1000) == 0;
}

struct DemoConfig {
  uint16_t port{0};
  uint32_t frames{1};
  uint64_t timeout_us{500000};
  NetDSP::TimeoutPolicy policy{NetDSP::TimeoutPolicy::ForceCommitPartial};
  TransportBackend transport{TransportBackend::InProcess};
  TransportMode mode{TransportMode::FullFrame};
  uint32_t refresh_rows{32};
  float sampling_ratio{0.10f};
  uint8_t omp_atoms{8};
  uint8_t dictionary_atoms{24};
  uint8_t min_samples_per_tile{6};
  uint16_t log_every{1000};
  uint32_t benchmark_warmup{5};
  uint32_t benchmark_iterations{20};
  uint32_t benchmark_every{0};
  uint32_t progress_every{100};
  uint32_t jitter_us{0};
  uint32_t drop_every{0};
  std::optional<uint16_t> drop_fragment{};
  bool reverse_order{true};
};

struct CapturedFragment {
  NetDSP::PacketHeader header{};
  std::vector<std::byte> payload;
  size_t payload_bytes{0};
};

struct StageStats {
  double best_ms{0.0};
  double avg_ms{0.0};
  float min_value{0.0f};
  float max_value{0.0f};
  double mean_value{0.0};
};

struct ImageDiffStats {
  double mean_abs_diff{0.0};
  float max_abs_diff{0.0f};
  size_t differing_samples{0};
};

struct StageRun {
  StageStats stats{};
  dsp::Image image{};
};

StageStats summarize_image(const dsp::Image &image) {
  if (image.size() == 0) {
    return {};
  }

  float min_value = std::numeric_limits<float>::max();
  float max_value = std::numeric_limits<float>::lowest();
  double sum = 0.0;
  for (size_t i = 0; i < image.size(); ++i) {
    const float value = image.data()[i];
    min_value = std::min(min_value, value);
    max_value = std::max(max_value, value);
    sum += value;
  }

  return StageStats{
      .best_ms = 0.0,
      .avg_ms = 0.0,
      .min_value = min_value,
      .max_value = max_value,
      .mean_value = sum / static_cast<double>(image.size()),
  };
}

template <typename Fn>
StageRun benchmark_stage(const char *name, uint32_t warmup,
                         uint32_t iterations, Fn &&fn) {
  for (uint32_t i = 0; i < warmup; ++i) {
    [[maybe_unused]] dsp::Image warm = fn();
  }

  double total_ms = 0.0;
  double best_ms = std::numeric_limits<double>::max();
  dsp::Image final_image{};
  for (uint32_t i = 0; i < iterations; ++i) {
    const auto start = std::chrono::steady_clock::now();
    dsp::Image image = fn();
    const auto end = std::chrono::steady_clock::now();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    total_ms += elapsed_ms;
    best_ms = std::min(best_ms, elapsed_ms);
    if (i + 1 == iterations) {
      final_image = std::move(image);
    }
  }

  float min_value = std::numeric_limits<float>::max();
  float max_value = std::numeric_limits<float>::lowest();
  double sum = 0.0;
  for (size_t i = 0; i < final_image.size(); ++i) {
    const float value = final_image.data()[i];
    min_value = std::min(min_value, value);
    max_value = std::max(max_value, value);
    sum += value;
  }

  const StageStats stats{
      .best_ms = best_ms,
      .avg_ms = total_ms / iterations,
      .min_value = min_value,
      .max_value = max_value,
      .mean_value = sum / static_cast<double>(final_image.size()),
  };

  std::println("    [{}] best={:.3f} ms avg={:.3f} ms | min={:.6f} max={:.6f} "
               "mean={:.6f}",
               name, stats.best_ms, stats.avg_ms, stats.min_value,
               stats.max_value, stats.mean_value);
  return StageRun{.stats = stats, .image = std::move(final_image)};
}

template <typename Fn>
StageRun run_stage_once(const char *name, bool log_output, Fn &&fn) {
  dsp::Image image = fn();
  const StageStats stats = summarize_image(image);
  if (log_output) {
    std::println("    [{}] validation-only | min={:.6f} max={:.6f} mean={:.6f}",
                 name, stats.min_value, stats.max_value, stats.mean_value);
  }
  return StageRun{.stats = stats, .image = std::move(image)};
}

bool should_run_full_benchmark(uint32_t frame_number,
                               const DemoConfig &config) {
  return frame_number == 0 ||
         (config.benchmark_every > 0 &&
          ((frame_number + 1) % config.benchmark_every) == 0);
}

bool should_log_frame_details(uint32_t frame_number, const DemoConfig &config) {
  return should_run_full_benchmark(frame_number, config) ||
         (config.progress_every > 0 &&
          ((frame_number + 1) % config.progress_every) == 0) ||
         (frame_number + 1) == config.frames;
}

std::optional<uint64_t> parse_u64(std::string_view text) {
  uint64_t value = 0;
  const auto *begin = text.data();
  const auto *end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    return std::nullopt;
  }
  return value;
}

bool parse_arg(std::string_view arg, DemoConfig &config) {
  const auto parse_value = [&](std::string_view prefix) -> std::string_view {
    return arg.substr(prefix.size());
  };

  if (arg == "--help") {
    std::println("Usage: ./udp_demo [--port=PORT] [--frames=N] "
                 "[--timeout-us=MICROS] [--policy=force|drop] "
                 "[--transport=udp|inproc] "
                 "[--mode=full|dir|cs] [--refresh-rows=N] "
                 "[--sampling-ratio=FLOAT] [--omp-atoms=N] "
                 "[--dictionary-atoms=N] [--min-samples=N] "
                 "[--log-every=N] [--benchmark-warmup=N] "
                 "[--benchmark-iterations=N] [--benchmark-every=N] "
                 "[--progress-every=N] "
                 "[--jitter-us=MICROS] "
                 "[--drop-every=N] [--drop-fragment=INDEX] "
                 "[--send-order=forward|reverse]");
    return false;
  }

  if (arg.starts_with("--transport=")) {
    const std::string_view value = parse_value("--transport=");
    if (value == "udp") {
      config.transport = TransportBackend::UdpLoopback;
      return true;
    }
    if (value == "inproc") {
      config.transport = TransportBackend::InProcess;
      return true;
    }
    return false;
  }

  if (arg.starts_with("--port=")) {
    const auto value = parse_u64(parse_value("--port="));
    if (!value.has_value() || *value > UINT16_MAX) {
      return false;
    }
    config.port = static_cast<uint16_t>(*value);
    return true;
  }

  if (arg.starts_with("--frames=")) {
    const auto value = parse_u64(parse_value("--frames="));
    if (!value.has_value() || *value == 0 || *value > UINT32_MAX) {
      return false;
    }
    config.frames = static_cast<uint32_t>(*value);
    return true;
  }

  if (arg.starts_with("--timeout-us=")) {
    const auto value = parse_u64(parse_value("--timeout-us="));
    if (!value.has_value() || *value == 0) {
      return false;
    }
    config.timeout_us = *value;
    return true;
  }

  if (arg.starts_with("--policy=")) {
    const std::string_view value = parse_value("--policy=");
    if (value == "force") {
      config.policy = NetDSP::TimeoutPolicy::ForceCommitPartial;
      return true;
    }
    if (value == "drop") {
      config.policy = NetDSP::TimeoutPolicy::DropPartial;
      return true;
    }
    return false;
  }

  if (arg.starts_with("--mode=")) {
    const std::string_view value = parse_value("--mode=");
    if (value == "full") {
      config.mode = TransportMode::FullFrame;
      return true;
    }
    if (value == "dir") {
      config.mode = TransportMode::DistributedIntraRefresh;
      return true;
    }
    if (value == "cs") {
      config.mode = TransportMode::CompressiveSampling;
      return true;
    }
    return false;
  }

  if (arg.starts_with("--refresh-rows=")) {
    const auto value = parse_u64(parse_value("--refresh-rows="));
    if (!value.has_value() || *value == 0 ||
        *value > NetDSP::SHADOW_FRAME_HEIGHT) {
      return false;
    }
    config.refresh_rows = static_cast<uint32_t>(*value);
    return true;
  }

  if (arg.starts_with("--sampling-ratio=")) {
    const auto text = parse_value("--sampling-ratio=");
    float value = 0.0f;
    const auto result = std::from_chars(text.data(), text.data() + text.size(),
                                        value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        value <= 0.0f || value > 1.0f) {
      return false;
    }
    config.sampling_ratio = value;
    return true;
  }

  if (arg.starts_with("--omp-atoms=")) {
    const auto value = parse_u64(parse_value("--omp-atoms="));
    if (!value.has_value() || *value == 0 || *value > UINT8_MAX) {
      return false;
    }
    config.omp_atoms = static_cast<uint8_t>(*value);
    return true;
  }

  if (arg.starts_with("--dictionary-atoms=")) {
    const auto value = parse_u64(parse_value("--dictionary-atoms="));
    if (!value.has_value() || *value == 0 || *value > UINT8_MAX) {
      return false;
    }
    config.dictionary_atoms = static_cast<uint8_t>(*value);
    return true;
  }

  if (arg.starts_with("--min-samples=")) {
    const auto value = parse_u64(parse_value("--min-samples="));
    if (!value.has_value() || *value == 0 || *value > UINT8_MAX) {
      return false;
    }
    config.min_samples_per_tile = static_cast<uint8_t>(*value);
    return true;
  }

  if (arg.starts_with("--log-every=")) {
    const auto value = parse_u64(parse_value("--log-every="));
    if (!value.has_value() || *value > UINT16_MAX) {
      return false;
    }
    config.log_every = static_cast<uint16_t>(*value);
    return true;
  }

  if (arg.starts_with("--benchmark-warmup=")) {
    const auto value = parse_u64(parse_value("--benchmark-warmup="));
    if (!value.has_value() || *value > UINT32_MAX) {
      return false;
    }
    config.benchmark_warmup = static_cast<uint32_t>(*value);
    return true;
  }

  if (arg.starts_with("--benchmark-iterations=")) {
    const auto value = parse_u64(parse_value("--benchmark-iterations="));
    if (!value.has_value() || *value == 0 || *value > UINT32_MAX) {
      return false;
    }
    config.benchmark_iterations = static_cast<uint32_t>(*value);
    return true;
  }

  if (arg.starts_with("--benchmark-every=")) {
    const auto value = parse_u64(parse_value("--benchmark-every="));
    if (!value.has_value() || *value > UINT32_MAX) {
      return false;
    }
    config.benchmark_every = static_cast<uint32_t>(*value);
    return true;
  }

  if (arg.starts_with("--progress-every=")) {
    const auto value = parse_u64(parse_value("--progress-every="));
    if (!value.has_value() || *value > UINT32_MAX) {
      return false;
    }
    config.progress_every = static_cast<uint32_t>(*value);
    return true;
  }

  if (arg.starts_with("--jitter-us=")) {
    const auto value = parse_u64(parse_value("--jitter-us="));
    if (!value.has_value() || *value > UINT32_MAX) {
      return false;
    }
    config.jitter_us = static_cast<uint32_t>(*value);
    return true;
  }

  if (arg.starts_with("--drop-every=")) {
    const auto value = parse_u64(parse_value("--drop-every="));
    if (!value.has_value() || *value > UINT32_MAX) {
      return false;
    }
    config.drop_every = static_cast<uint32_t>(*value);
    return true;
  }

  if (arg.starts_with("--drop-fragment=")) {
    const auto value = parse_u64(parse_value("--drop-fragment="));
    if (!value.has_value() || *value >= NetDSP::SenderEngine::totalFragments()) {
      return false;
    }
    config.drop_fragment = static_cast<uint16_t>(*value);
    return true;
  }

  if (arg.starts_with("--send-order=")) {
    const std::string_view value = parse_value("--send-order=");
    if (value == "reverse") {
      config.reverse_order = true;
      return true;
    }
    if (value == "forward") {
      config.reverse_order = false;
      return true;
    }
    return false;
  }

  return false;
}

bool should_log_fragment(uint16_t fragment_index, uint16_t total_fragments,
                         uint16_t log_every) {
  return fragment_index < 3 || fragment_index + 3 >= total_fragments ||
         (log_every > 0 && (fragment_index % log_every) == 0);
}

void fill_frame(std::vector<float> &frame, uint32_t frame_number) {
  frame.resize(NetDSP::SHADOW_PIXEL_COUNT);
  const float denom = static_cast<float>(
      NetDSP::SHADOW_FRAME_WIDTH + NetDSP::SHADOW_FRAME_HEIGHT + frame_number +
      1);
  const __m256 inv_denom = _mm256_set1_ps(1.0f / denom);
  const __m256 x_offsets =
      _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f);

  for (uint32_t y = 0; y < NetDSP::SHADOW_FRAME_HEIGHT; ++y) {
    const __m256 row_base =
        _mm256_set1_ps(static_cast<float>(y + frame_number));
    uint32_t x = 0;
    for (; x + 7 < NetDSP::SHADOW_FRAME_WIDTH; x += 8) {
      const __m256 x_base = _mm256_set1_ps(static_cast<float>(x));
      const __m256 values =
          _mm256_mul_ps(_mm256_add_ps(_mm256_add_ps(x_base, x_offsets),
                                      row_base),
                        inv_denom);
      _mm256_storeu_ps(frame.data() +
                           static_cast<size_t>(y) *
                               NetDSP::SHADOW_FRAME_WIDTH +
                           x,
                       values);
    }
    for (; x < NetDSP::SHADOW_FRAME_WIDTH; ++x) {
      frame[static_cast<size_t>(y) * NetDSP::SHADOW_FRAME_WIDTH + x] =
          static_cast<float>(x + y + frame_number) / denom;
    }
  }
}

std::vector<float> build_frame(uint32_t frame_number) {
  std::vector<float> frame;
  fill_frame(frame, frame_number);
  return frame;
}

ImageDiffStats compare_images(const float *lhs, const float *rhs, size_t count) {
  double sum_abs = 0.0;
  float max_abs = 0.0f;
  size_t differing = 0;

  for (size_t i = 0; i < count; ++i) {
    const float diff = std::fabs(lhs[i] - rhs[i]);
    sum_abs += diff;
    max_abs = std::max(max_abs, diff);
    if (diff > 1e-7f) {
      ++differing;
    }
  }

  return ImageDiffStats{
      .mean_abs_diff = sum_abs / static_cast<double>(count),
      .max_abs_diff = max_abs,
      .differing_samples = differing,
  };
}

ImageDiffStats compare_sampled_images(const float *lhs, const float *rhs,
                                      size_t count, size_t sample_count) {
  if (count == 0 || sample_count == 0) {
    return {};
  }

  double sum_abs = 0.0;
  float max_abs = 0.0f;
  size_t differing = 0;
  const size_t stride = std::max<size_t>(1, count / sample_count);
  size_t sampled = 0;
  for (size_t i = 0; i < count && sampled < sample_count; i += stride) {
    const float diff = std::fabs(lhs[i] - rhs[i]);
    sum_abs += diff;
    max_abs = std::max(max_abs, diff);
    if (diff > 1e-7f) {
      ++differing;
    }
    ++sampled;
  }

  return ImageDiffStats{
      .mean_abs_diff = sum_abs / static_cast<double>(sampled),
      .max_abs_diff = max_abs,
      .differing_samples = differing,
  };
}

std::vector<float>
snapshot_reference(const NetDSP::TemporalRefreshReconstructor &reference) {
  return std::vector<float>(reference.data(),
                            reference.data() + NetDSP::SHADOW_PIXEL_COUNT);
}

template <typename ReadyFrameT>
bool validate_and_process_frame(const ReadyFrameT &ready,
                                std::span<const float> expected_frame,
                                uint32_t expected_sequence,
                                uint16_t expected_frame_id,
                                const DemoConfig &config,
                                uint32_t frame_number,
                                bool verbose_frame) {
  if (verbose_frame) {
    std::println("  slot={} sequence={} frame_id={} bytes_used={} partial={} "
                 "missing={} temporal={} refresh_rows=[{}, {})",
                 ready.slot_index, ready.descriptor.sequence,
                 ready.descriptor.frame_id, ready.descriptor.bytes_used,
                 ready.descriptor.isPartial(), ready.descriptor.missing_fragments,
                 ready.descriptor.usesTemporalRefresh(),
                 ready.descriptor.refresh_start_row,
                 ready.descriptor.refresh_start_row +
                     ready.descriptor.refresh_row_count);
  }

  if (ready.descriptor.sequence != expected_sequence ||
      ready.descriptor.frame_id != expected_frame_id) {
    std::println(stderr,
                 "  unexpected ready frame: expected sequence={} frame_id={} "
                 "but received sequence={} frame_id={}",
                 expected_sequence, expected_frame_id,
                 ready.descriptor.sequence, ready.descriptor.frame_id);
    return false;
  }

  if (verbose_frame) {
    std::println("  validating ready frame against expected reference");
  }
  const bool run_full_benchmark =
      should_run_full_benchmark(frame_number, config);
  const bool run_detailed_validation = verbose_frame || run_full_benchmark;
  const auto *expected_bytes =
      reinterpret_cast<const std::byte *>(expected_frame.data());
  const bool full_match =
      run_detailed_validation &&
      std::memcmp(ready.bytes, expected_bytes, NetDSP::SHADOW_BUFFER_BYTES) == 0;
  const bool first_sample_ok = ready.pixels[0] == expected_frame[0];
  const bool second_sample_ok = ready.pixels[1] == expected_frame[1];
  const bool last_sample_ok =
      ready.pixels[NetDSP::SHADOW_PIXEL_COUNT - 1] == expected_frame.back();
  const ImageDiffStats input_diff = run_detailed_validation
                                        ? compare_images(ready.pixels,
                                                         expected_frame.data(),
                                                         NetDSP::SHADOW_PIXEL_COUNT)
                                        : compare_sampled_images(
                                              ready.pixels, expected_frame.data(),
                                              NetDSP::SHADOW_PIXEL_COUNT, 64);
  if (verbose_frame) {
    std::println("  full frame byte match={}", full_match);
    std::println("  sample [0] ok={} sample [1] ok={} sample [last] ok={}",
                 first_sample_ok, second_sample_ok, last_sample_ok);
    std::println("  input diff: mean_abs={:.9f} max_abs={:.9f} differing_samples={}",
                 input_diff.mean_abs_diff, input_diff.max_abs_diff,
                 input_diff.differing_samples);
  }

  const bool exact_or_sampled_match =
      run_detailed_validation
          ? (full_match && first_sample_ok && second_sample_ok && last_sample_ok)
          : (first_sample_ok && second_sample_ok && last_sample_ok);
  const bool payload_ok =
      config.mode == TransportMode::CompressiveSampling
          ? (input_diff.mean_abs_diff < 0.06 &&
             input_diff.max_abs_diff < 1.05f)
          : (ready.descriptor.isPartial()
                 ? true
                 : exact_or_sampled_match);
  if (!payload_ok) {
    return false;
  }

  if (!run_detailed_validation) {
    return true;
  }

  if (run_full_benchmark) {
    std::println("  benchmarking DSP pipeline with warmup={} iterations={}",
                 config.benchmark_warmup, config.benchmark_iterations);
  } else if (verbose_frame) {
    std::println("  running validation-only DSP pass "
                 "(full benchmark every {} frames)",
                 config.benchmark_every == 0 ? 0 : config.benchmark_every);
  }

  dsp::Image expected_input(NetDSP::SHADOW_FRAME_WIDTH,
                            NetDSP::SHADOW_FRAME_HEIGHT);
  std::memcpy(expected_input.data(), expected_frame.data(),
              NetDSP::SHADOW_BUFFER_BYTES);
  dsp::Image received_input(NetDSP::SHADOW_FRAME_WIDTH,
                            NetDSP::SHADOW_FRAME_HEIGHT);
  std::memcpy(received_input.data(), ready.pixels, NetDSP::SHADOW_BUFFER_BYTES);

  auto benchmark_pipeline = [&](const char *label, const dsp::Image &input) {
    if (verbose_frame || run_full_benchmark) {
      std::println("  {} input samples: [0]={:.6f} [1]={:.6f} [last]={:.6f}",
                   label, input.data()[0], input.data()[1],
                   input.data()[input.size() - 1]);
    }
    if (run_full_benchmark) {
      const StageRun blur_run = benchmark_stage(
          "gaussianBlur", config.benchmark_warmup, config.benchmark_iterations,
          [&]() { return dsp::gaussianBlur(input); });
      const StageRun sobel_run = benchmark_stage(
          "sobelMagnitude", config.benchmark_warmup,
          config.benchmark_iterations,
          [&]() { return dsp::sobelMagnitude(input); });
      const StageRun fused_run = benchmark_stage(
          "blurAndSobelFused", config.benchmark_warmup,
          config.benchmark_iterations,
          [&]() { return dsp::blurAndSobelFused(input); });

      const bool blur_ok = std::isfinite(blur_run.stats.mean_value) &&
                           blur_run.stats.min_value >= 0.0f &&
                           blur_run.stats.max_value <= 1.0f;
      const bool sobel_ok = std::isfinite(sobel_run.stats.mean_value) &&
                            sobel_run.stats.min_value >= 0.0f &&
                            sobel_run.stats.max_value <= 1.0f;
      const bool fused_ok = std::isfinite(fused_run.stats.mean_value) &&
                            fused_run.stats.min_value >= 0.0f &&
                            fused_run.stats.max_value <= 1.0f;
      std::println("    validation: blur_ok={} sobel_ok={} fused_ok={}",
                   blur_ok, sobel_ok, fused_ok);
      return std::pair{blur_ok && sobel_ok && fused_ok,
                       std::array<StageRun, 3>{std::move(blur_run),
                                               std::move(sobel_run),
                                               std::move(fused_run)}};
    }

    const StageRun fused_run = run_stage_once(
        "blurAndSobelFused", verbose_frame,
        [&]() { return dsp::blurAndSobelFused(input); });
    const bool fused_ok = std::isfinite(fused_run.stats.mean_value) &&
                          fused_run.stats.min_value >= 0.0f &&
                          fused_run.stats.max_value <= 1.0f;
    if (verbose_frame) {
      std::println("    validation: fused_ok={}", fused_ok);
    }
    return std::pair{fused_ok,
                     std::array<StageRun, 3>{
                         StageRun{}, StageRun{}, std::move(fused_run)}};
  };

  if (run_full_benchmark) {
    std::println("  expected reference benchmark:");
  } else if (verbose_frame) {
    std::println("  expected reference validation:");
  }
  auto [source_ok, source_runs] =
      benchmark_pipeline("expected", expected_input);
  if (run_full_benchmark) {
    std::println("  received frame benchmark:");
  } else if (verbose_frame) {
    std::println("  received frame validation:");
  }
  auto [received_ok, received_runs] =
      benchmark_pipeline("received", received_input);

  const ImageDiffStats fused_diff = compare_images(
      source_runs[2].image.data(), received_runs[2].image.data(),
      source_runs[2].image.size());

  if (verbose_frame || run_full_benchmark) {
    std::println("  output diff vs source:");
  }
  if (run_full_benchmark) {
    const ImageDiffStats blur_diff = compare_images(
        source_runs[0].image.data(), received_runs[0].image.data(),
        source_runs[0].image.size());
    const ImageDiffStats sobel_diff = compare_images(
        source_runs[1].image.data(), received_runs[1].image.data(),
        source_runs[1].image.size());
    std::println("    blur:  mean_abs={:.9f} max_abs={:.9f} differing={}",
                 blur_diff.mean_abs_diff, blur_diff.max_abs_diff,
                 blur_diff.differing_samples);
    std::println("    sobel: mean_abs={:.9f} max_abs={:.9f} differing={}",
                 sobel_diff.mean_abs_diff, sobel_diff.max_abs_diff,
                 sobel_diff.differing_samples);
  }
  if (verbose_frame || run_full_benchmark) {
    std::println("    fused: mean_abs={:.9f} max_abs={:.9f} differing={}",
                 fused_diff.mean_abs_diff, fused_diff.max_abs_diff,
                 fused_diff.differing_samples);
  }

  return source_ok && received_ok;
}

} // namespace

int main(int argc, char **argv) {
  std::println("==========================================================");
  std::println("UDP Transport Demo");
  std::println("==========================================================\n");

  DemoConfig config;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (!parse_arg(arg, config)) {
      if (arg == "--help") {
        return 0;
      }
      std::println(stderr, "Invalid argument: {}", arg);
      std::println(stderr, "Use --help for usage.");
      return 1;
    }
  }

  std::println("[0/10] Runtime configuration");
  std::println("  port={} frames={} timeout_us={} policy={} transport={} log_every={} "
               "mode={} refresh_rows={} benchmark_warmup={} "
               "benchmark_iterations={} benchmark_every={} progress_every={} "
               "jitter_us={} "
               "drop_every={} drop_fragment={} send_order={}",
               config.port, config.frames, config.timeout_us,
               config.policy == NetDSP::TimeoutPolicy::ForceCommitPartial
                   ? "force"
                   : "drop",
               config.transport == TransportBackend::UdpLoopback ? "udp"
                                                                 : "inproc",
               config.log_every,
               config.mode == TransportMode::FullFrame
                   ? "full"
                   : (config.mode == TransportMode::DistributedIntraRefresh
                          ? "dir"
                          : "cs"),
               config.refresh_rows, config.benchmark_warmup,
               config.benchmark_iterations, config.benchmark_every,
               config.progress_every, config.jitter_us,
               config.drop_every,
               config.drop_fragment.has_value()
                   ? std::to_string(*config.drop_fragment)
                   : std::string("none"),
               config.reverse_order ? "reverse" : "forward");
  std::println("  cs_sampling_ratio={:.3f} cs_omp_atoms={} "
               "cs_dictionary_atoms={} cs_min_samples={}",
               config.sampling_ratio, config.omp_atoms,
               config.dictionary_atoms, config.min_samples_per_tile);

  UdpSocket receiver_socket{};
  UdpSocket sender_socket{};
  sockaddr_in destination{};
  if (config.transport == TransportBackend::UdpLoopback) {
    std::println("\n[1/10] Creating UDP sockets");
    receiver_socket = create_udp_socket();
    sender_socket = create_udp_socket();
    if (!receiver_socket.valid() || !sender_socket.valid()) {
      std::println(stderr, "Socket creation failed");
      return 1;
    }
    std::println("  receiver fd={}, sender fd={}", receiver_socket.get(),
                 sender_socket.get());

    std::println("\n[2/10] Configuring receiver socket");
    if (!set_receive_timeout(receiver_socket.get(), 500) ||
        !set_receive_buffer_size(receiver_socket.get(), 1 << 23)) {
      std::println(stderr, "Receiver socket configuration failed");
      return 1;
    }
    const uint16_t port = [&]() -> uint16_t {
      if (config.port != 0) {
        const sockaddr_in address = loopback_address(config.port);
        if (bind(receiver_socket.get(),
                 reinterpret_cast<const sockaddr *>(&address),
                 sizeof(address)) != 0) {
          return 0;
        }
        return config.port;
      }
      return bind_ephemeral_loopback(receiver_socket.get());
    }();
    if (port == 0) {
      std::println(stderr, "Receiver bind failed");
      return 1;
    }
    destination = loopback_address(port);
    std::println("  loopback receiver bound to 127.0.0.1:{}", port);
  } else {
    std::println("\n[1/10] Transport backend");
    std::println("  using in-process packet injection");
  }

  std::println("\n[3/10] Starting receiver engine");
  NetDSP::ReceiverEngine<2, 32> receiver(config.timeout_us, config.policy);
  NetDSP::CompressiveReceiverEngine<2, 32> compressive_receiver(
      config.timeout_us);
  const NetDSP::DistributedIntraRefreshScheduler dir_scheduler(
      NetDSP::SHADOW_FRAME_HEIGHT, config.refresh_rows);
  const NetDSP::CompressiveSamplingConfig compressive_config{
      .sampling_ratio = config.sampling_ratio,
      .min_samples_per_tile = config.min_samples_per_tile,
      .max_omp_atoms = config.omp_atoms,
      .dictionary_atoms = config.dictionary_atoms,
      .tile_width = NetDSP::CS_TILE_WIDTH,
      .tile_height = NetDSP::CS_TILE_HEIGHT,
      .sampling_seed = 0xC05E1234u,
  };
  NetDSP::TemporalRefreshReconstructor expected_reference(
      NetDSP::SHADOW_FRAME_WIDTH, NetDSP::SHADOW_FRAME_HEIGHT, 0.0f);
  std::atomic<bool> receive_done{false};
  std::atomic<bool> verbose_fragment_logging{false};
  std::atomic<uint32_t> rx_full_completions{0};
  uint32_t validated_frames = 0;
  uint32_t benchmarked_frames = 0;
  uint32_t partial_frames = 0;
  uint32_t discarded_ready_frames = 0;
  size_t sweep_committed_total = 0;
  size_t sweep_dropped_total = 0;
  size_t sweep_queue_full_total = 0;
  double cs_encode_ms_total = 0.0;
  double cs_encode_ms_best = std::numeric_limits<double>::max();
  double cs_encode_ms_worst = 0.0;
  double frame_build_ms_total = 0.0;
  double receiver_ingest_ms_total = 0.0;
  double ready_wait_ms_total = 0.0;
  double validation_ms_total = 0.0;
  auto consume_packet = [&](const NetDSP::PacketHeader &header,
                            const std::byte *payload,
                            size_t payload_bytes,
                            size_t packet_count) {
    if (verbose_fragment_logging.load(std::memory_order_acquire) &&
        should_log_fragment(header.fragment_index, header.total_fragments,
                            config.log_every)) {
      std::println("  [RX] fragment {:4}/{} payload={} bytes",
                   header.fragment_index + 1, header.total_fragments,
                   payload_bytes);
    }

    const auto result =
        config.mode == TransportMode::CompressiveSampling
            ? compressive_receiver.onPacket(header, payload, payload_bytes,
                                            monotonic_us())
            : receiver.onPacket(header, payload, payload_bytes, monotonic_us());
    if (result.status == NetDSP::PacketStatus::FrameCompleted &&
        verbose_fragment_logging.load(std::memory_order_acquire)) {
      std::println("  [RX] frame completed in slot {} after {} packets",
                   result.slot_index, packet_count);
      rx_full_completions.fetch_add(1, std::memory_order_release);
    } else if (result.status == NetDSP::PacketStatus::FrameCompleted) {
      rx_full_completions.fetch_add(1, std::memory_order_release);
    }
  };

  std::thread receiver_thread;
  if (config.transport == TransportBackend::UdpLoopback) {
    receiver_thread = std::thread([&]() {
      std::array<std::byte, NetDSP::DEFAULT_PACKET_BYTES> rx_packet{};
      size_t packet_count = 0;
      std::println("  [RX] Receiver thread active");

      while (!receive_done.load(std::memory_order_acquire)) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        const ssize_t bytes_received =
            recvfrom(receiver_socket.get(), rx_packet.data(), rx_packet.size(), 0,
                     reinterpret_cast<sockaddr *>(&peer), &peer_len);
        if (bytes_received < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
          }
          std::println(stderr, "  [RX] recvfrom failed with errno={}", errno);
          break;
        }

        NetDSP::PacketHeader header{};
        const std::byte *payload = nullptr;
        size_t payload_bytes = 0;
        if (!NetDSP::parseDatagram(rx_packet.data(),
                                   static_cast<size_t>(bytes_received), header,
                                   payload, payload_bytes)) {
          std::println(stderr, "  [RX] Failed to parse datagram");
          continue;
        }

        ++packet_count;
        consume_packet(header, payload, payload_bytes, packet_count);
      }
    });
  }

  std::println("\n[4/10] Creating sender engine");
  NetDSP::SenderEngine sender;
  std::array<std::byte, NetDSP::DEFAULT_PACKET_BYTES> packet{};
  std::vector<float> frame(NetDSP::SHADOW_PIXEL_COUNT, 0.0f);
  for (uint32_t frame_number = 0; frame_number < config.frames; ++frame_number) {
    const bool verbose_frame = should_log_frame_details(frame_number, config);
    verbose_fragment_logging.store(verbose_frame, std::memory_order_release);
    if (verbose_frame) {
      std::println("\n[5/10] Building source frame {}", frame_number + 1);
    }
    const auto build_start = std::chrono::steady_clock::now();
    fill_frame(frame, frame_number);
    frame_build_ms_total += std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() -
                                build_start)
                                .count();
    if (verbose_frame) {
      std::println("  frame bytes={} pixels={} total_fragments={}",
                   NetDSP::SHADOW_BUFFER_BYTES, frame.size(),
                   NetDSP::SenderEngine::totalFragments());
      std::println("  source samples: [0]={:.6f} [1]={:.6f} [last]={:.6f}",
                   frame[0], frame[1], frame.back());
    }

    if (verbose_frame) {
      std::println("\n[6/10] Sending frame {}", frame_number + 1);
    }
    const uint16_t frame_id = static_cast<uint16_t>(42 + frame_number);
    const uint64_t send_timestamp = monotonic_us();
    const bool send_full_frame =
        config.mode == TransportMode::FullFrame || frame_number == 0;
    const bool send_compressive_frame =
        config.mode == TransportMode::CompressiveSampling;
    const uint64_t refresh_frame_index =
        (send_full_frame || send_compressive_frame)
            ? 0
            : static_cast<uint64_t>(frame_number - 1);

    std::vector<CapturedFragment> fragments;
    std::vector<std::byte> compressive_payload;
    NetDSP::CompressiveFrameStats compressive_encode_stats{};
    uint16_t compressive_total_fragments = 0;
    uint32_t sequence = 0;
    const bool captured =
        send_compressive_frame
            ? [&]() {
                compressive_payload = NetDSP::encodeCompressiveFramePayload(
                    frame.data(), NetDSP::SHADOW_FRAME_WIDTH,
                    NetDSP::SHADOW_FRAME_HEIGHT, compressive_config, frame_id,
                    &compressive_encode_stats);
                if (compressive_payload.empty()) {
                  return false;
                }
                cs_encode_ms_total += compressive_encode_stats.elapsed_ms;
                cs_encode_ms_best =
                    std::min(cs_encode_ms_best,
                             compressive_encode_stats.elapsed_ms);
                cs_encode_ms_worst =
                    std::max(cs_encode_ms_worst,
                             compressive_encode_stats.elapsed_ms);
                compressive_total_fragments =
                    NetDSP::fragmentsForPayloadBytes(compressive_payload.size());
                if (compressive_total_fragments == 0) {
                  return false;
                }
                sequence = sender.reserveSequence();
                return true;
              }()
            : send_full_frame
            ? sender.sendFrame(
                  frame.data(), frame_id, send_timestamp,
                  [&](const NetDSP::PacketHeader &header, const std::byte *payload,
                      size_t payload_bytes) {
                    fragments.push_back(CapturedFragment{
                        .header = header,
                        .payload =
                            std::vector<std::byte>(payload, payload + payload_bytes),
                        .payload_bytes = payload_bytes,
                    });
                    return true;
                  })
            : sender.sendDistributedIntraRefreshFrame(
                  frame.data(), frame_id, send_timestamp, refresh_frame_index,
                  dir_scheduler,
                  [&](const NetDSP::PacketHeader &header, const std::byte *payload,
                      size_t payload_bytes) {
                    fragments.push_back(CapturedFragment{
                        .header = header,
                        .payload =
                            std::vector<std::byte>(payload, payload + payload_bytes),
                        .payload_bytes = payload_bytes,
                    });
                    return true;
                  });
    if (!captured || fragments.empty()) {
      if (!send_compressive_frame) {
        receive_done.store(true, std::memory_order_release);
        if (receiver_thread.joinable()) {
          receiver_thread.join();
        }
        std::println(stderr, "Failed to capture outgoing fragments");
        return 1;
      }
    }

    if (!captured) {
      receive_done.store(true, std::memory_order_release);
      if (receiver_thread.joinable()) {
        receiver_thread.join();
      }
      std::println(stderr, "Failed to prepare outgoing payload");
      return 1;
    }

    if (!send_compressive_frame) {
      sequence = fragments.front().header.sequence;
    }
    const size_t frame_payload_bytes =
        send_compressive_frame
            ? compressive_payload.size()
            : std::accumulate(
                  fragments.begin(), fragments.end(), size_t{0},
                  [](size_t total, const CapturedFragment &fragment) {
                    return total + fragment.payload_bytes;
                  });
    size_t sent_packets = 0;
    std::vector<float> expected_frame_storage{};
    std::span<const float> expected_frame{};

    if (send_compressive_frame) {
      expected_frame = std::span<const float>(frame.data(), frame.size());
      if (verbose_frame) {
        std::println("  CS payload_bytes={} ({:.2f}% of full frame) encode_ms={:.3f}",
                     frame_payload_bytes,
                     (100.0 * static_cast<double>(frame_payload_bytes)) /
                         static_cast<double>(NetDSP::SHADOW_BUFFER_BYTES),
                     compressive_encode_stats.elapsed_ms);
      }
    } else if (send_full_frame) {
      std::copy_n(frame.data(), NetDSP::SHADOW_PIXEL_COUNT,
                  expected_reference.data());
      expected_frame = std::span<const float>(frame.data(), frame.size());
    } else {
      const NetDSP::RefreshPlan plan =
          dir_scheduler.planForFrame(refresh_frame_index);
      expected_reference.applyRefreshPlan(plan, frame.data());
      expected_frame_storage = snapshot_reference(expected_reference);
      expected_frame = std::span<const float>(expected_frame_storage.data(),
                                              expected_frame_storage.size());
      if (verbose_frame) {
        std::println("  DIR payload rows={} payload_bytes={} ({:.2f}% of full frame)",
                     plan.payloadRowCount(), frame_payload_bytes,
                     (100.0 * static_cast<double>(frame_payload_bytes)) /
                         static_cast<double>(NetDSP::SHADOW_BUFFER_BYTES));
      }
    }

    if (send_compressive_frame &&
        config.transport == TransportBackend::InProcess) {
      const auto ingest_start = std::chrono::steady_clock::now();
      const auto result = compressive_receiver.onPayload(
          sequence, frame_id, send_timestamp,
          NetDSP::FLAG_P_FRAME | NetDSP::FLAG_CS_ENABLED,
          compressive_payload.data(), compressive_payload.size());
      receiver_ingest_ms_total += std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() -
                                      ingest_start)
                                      .count();
      if (result.status != NetDSP::PacketStatus::FrameCompleted) {
        std::println(stderr, "In-process CS ingest failed for frame {}",
                     frame_number + 1);
        return 1;
      }
      ++rx_full_completions;
      sent_packets = 1;
      if (verbose_frame) {
        std::println("  [TX] frame {} ingested in-process as a full CS payload",
                     frame_number + 1);
      }
    } else {
      const uint16_t fragment_count =
          send_compressive_frame ? compressive_total_fragments
                                 : static_cast<uint16_t>(fragments.size());
      std::vector<uint16_t> send_order(fragment_count);
      for (uint16_t index = 0; index < send_order.size(); ++index) {
        send_order[index] = config.reverse_order
                                ? static_cast<uint16_t>(send_order.size() - 1 - index)
                                : index;
      }

      size_t dropped_packets = 0;
      for (size_t order_index = 0; order_index < send_order.size(); ++order_index) {
        const uint16_t fragment_index = send_order[order_index];
        NetDSP::PacketHeader fragment_header{};
        const std::byte *fragment_payload = nullptr;
        size_t fragment_payload_bytes = 0;
        if (send_compressive_frame) {
          const size_t payload_offset = static_cast<size_t>(fragment_index) *
                                        NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES;
          fragment_payload_bytes = std::min(NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES,
                                            compressive_payload.size() -
                                                payload_offset);
          fragment_payload = compressive_payload.data() + payload_offset;
          fragment_header = NetDSP::SenderEngine::makeCompressiveHeader(
              sequence, frame_id, fragment_index, compressive_total_fragments,
              send_timestamp,
              NetDSP::FLAG_P_FRAME | NetDSP::FLAG_CS_ENABLED |
                  NetDSP::FLAG_COMPRESSIVE_SAMPLING);
        } else {
          const auto &fragment = fragments[fragment_index];
          fragment_header = fragment.header;
          fragment_payload = fragment.payload.data();
          fragment_payload_bytes = fragment.payload_bytes;
        }
        const bool should_drop =
            (config.drop_every > 0 &&
             ((order_index + 1) % config.drop_every) == 0) ||
            (config.drop_fragment.has_value() &&
             *config.drop_fragment == fragment_header.fragment_index);
        if (should_drop) {
          ++dropped_packets;
          if (verbose_frame &&
              should_log_fragment(fragment_header.fragment_index,
                                  fragment_header.total_fragments,
                                  config.log_every)) {
            std::println("  [TX] frame {} fragment {:4}/{} DROPPED",
                         frame_number + 1, fragment_header.fragment_index + 1,
                         fragment_header.total_fragments);
          }
          continue;
        }

        if (config.transport == TransportBackend::UdpLoopback) {
          const size_t packet_bytes = NetDSP::serializeDatagram(
              fragment_header, fragment_payload, fragment_payload_bytes,
              packet.data(), packet.size());
          if (packet_bytes == 0) {
            std::println(stderr, "Failed to serialize fragment {}", fragment_index);
            receive_done.store(true, std::memory_order_release);
            if (receiver_thread.joinable()) {
              receiver_thread.join();
            }
            return 1;
          }
          const ssize_t sent =
              sendto(sender_socket.get(), packet.data(), packet_bytes, 0,
                     reinterpret_cast<const sockaddr *>(&destination),
                     sizeof(destination));
          if (sent != static_cast<ssize_t>(packet_bytes)) {
            std::println(stderr, "sendto failed at fragment {} errno={}",
                         fragment_index, errno);
            receive_done.store(true, std::memory_order_release);
            if (receiver_thread.joinable()) {
              receiver_thread.join();
            }
            return 1;
          }
        } else {
          consume_packet(fragment_header, fragment_payload,
                         fragment_payload_bytes, sent_packets + 1);
        }

        ++sent_packets;
        if (verbose_frame &&
            should_log_fragment(fragment_header.fragment_index,
                                fragment_header.total_fragments,
                                config.log_every)) {
          std::println("  [TX] frame {} fragment {:4}/{} payload={} bytes",
                       frame_number + 1, fragment_header.fragment_index + 1,
                       fragment_header.total_fragments, fragment_payload_bytes);
        }

        if (config.jitter_us > 0) {
          std::this_thread::sleep_for(
              std::chrono::microseconds(config.jitter_us));
        }
      }
      if (verbose_frame) {
        std::println("  [TX] frame {} sent {} UDP datagrams (dropped={})",
                     frame_number + 1, sent_packets, dropped_packets);
      }
    }

    if (verbose_frame) {
      std::println("\n[7/10] Waiting for frame {} completion", frame_number + 1);
    }
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::microseconds(std::max<uint64_t>(config.timeout_us * 2,
                                                     2000000));
    std::optional<NetDSP::ReadyFrame> ready;
    const auto wait_start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
      const NetDSP::SweepResult sweep =
          config.mode == TransportMode::CompressiveSampling
              ? compressive_receiver.pollExpiredFrames(monotonic_us())
              : receiver.pollExpiredFrames(monotonic_us());
      if (sweep.committed > 0 || sweep.dropped > 0 || sweep.queue_full > 0) {
        sweep_committed_total += sweep.committed;
        sweep_dropped_total += sweep.dropped;
        sweep_queue_full_total += sweep.queue_full;
        if (verbose_frame) {
          std::println("  [RX] sweep committed={} dropped={} queue_full={}",
                       sweep.committed, sweep.dropped, sweep.queue_full);
        }
      }
      ready = config.mode == TransportMode::CompressiveSampling
                  ? compressive_receiver.tryAcquireReadyFrame()
                  : receiver.tryAcquireReadyFrame();
      if (ready.has_value() &&
          ready->descriptor.sequence == sequence &&
          ready->descriptor.frame_id == frame_id) {
        break;
      }
      if (ready.has_value()) {
        ++discarded_ready_frames;
        if (verbose_frame) {
          std::println("  [RX] discarding stale ready frame sequence={} frame_id={}",
                       ready->descriptor.sequence, ready->descriptor.frame_id);
        }
        if (config.mode == TransportMode::CompressiveSampling) {
          compressive_receiver.releaseReadyFrame(ready->slot_index);
        } else {
          receiver.releaseReadyFrame(ready->slot_index);
        }
      }
      if (config.transport == TransportBackend::UdpLoopback) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    ready_wait_ms_total += std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - wait_start)
                               .count();

    if (!ready.has_value()) {
      receive_done.store(true, std::memory_order_release);
      if (receiver_thread.joinable()) {
        receiver_thread.join();
      }
      std::println(stderr, "No completed frame available before timeout");
      return 1;
    }

    if (verbose_frame) {
      std::println("\n[8/10] Inspecting frame {}", frame_number + 1);
    }
    if (ready->descriptor.isPartial()) {
      ++partial_frames;
    }
    const auto validation_start = std::chrono::steady_clock::now();
    const bool valid_frame = validate_and_process_frame(
        *ready, expected_frame, sequence, frame_id, config, frame_number,
        verbose_frame);
    validation_ms_total += std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() -
                               validation_start)
                               .count();
    if (!valid_frame) {
      if (config.mode == TransportMode::CompressiveSampling) {
        compressive_receiver.releaseReadyFrame(ready->slot_index);
      } else {
        receiver.releaseReadyFrame(ready->slot_index);
      }
      receive_done.store(true, std::memory_order_release);
      if (receiver_thread.joinable()) {
        receiver_thread.join();
      }
      std::println(stderr, "Frame {} validation failed", frame_number + 1);
      return 1;
    }
    ++validated_frames;
    if (should_run_full_benchmark(frame_number, config)) {
      ++benchmarked_frames;
    }

    if (verbose_frame) {
      std::println("\n[9/10] Releasing frame {} slot", frame_number + 1);
    }
    if (config.mode == TransportMode::CompressiveSampling) {
      compressive_receiver.releaseReadyFrame(ready->slot_index);
    } else {
      receiver.releaseReadyFrame(ready->slot_index);
    }
    if (verbose_frame) {
      std::println("  slot {} released back to pool", ready->slot_index);
    }
  }

  std::println("\n[10/10] Shutting down receiver path");
  receive_done.store(true, std::memory_order_release);
  if (receiver_thread.joinable()) {
    receiver_thread.join();
  }

  std::println("  validated_frames={} benchmarked_frames={} partial_frames={} "
               "rx_full_completions={} discarded_ready_frames={} "
               "sweep_committed={} sweep_dropped={} "
               "sweep_queue_full={} queued_frames={}",
               validated_frames, benchmarked_frames, partial_frames,
               rx_full_completions.load(std::memory_order_acquire),
               discarded_ready_frames, sweep_committed_total, sweep_dropped_total,
               sweep_queue_full_total,
               config.mode == TransportMode::CompressiveSampling
                   ? compressive_receiver.queuedFrameCount()
                   : receiver.queuedFrameCount());
  if (config.mode == TransportMode::CompressiveSampling &&
      validated_frames > 0) {
    std::println("  cs_encode_ms best={:.3f} avg={:.3f} worst={:.3f}",
                 cs_encode_ms_best,
                 cs_encode_ms_total / static_cast<double>(validated_frames),
                 cs_encode_ms_worst);
    std::println("  avg_stage_ms build={:.3f} receiver_ingest={:.3f} "
                 "ready_wait={:.3f} validation={:.3f}",
                 frame_build_ms_total / static_cast<double>(validated_frames),
                 receiver_ingest_ms_total /
                     static_cast<double>(validated_frames),
                 ready_wait_ms_total / static_cast<double>(validated_frames),
                 validation_ms_total / static_cast<double>(validated_frames));
  }
  std::println("\nUDP transport + DSP demo completed successfully");
  return 0;
}
