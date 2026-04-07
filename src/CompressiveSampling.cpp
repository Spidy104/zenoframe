#include "CompressiveSampling.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <numeric>
#include <vector>

namespace NetDSP {

namespace {

[[nodiscard]] uint32_t mix_bits(uint32_t value) {
  value ^= value >> 16;
  value *= 0x7feb352du;
  value ^= value >> 15;
  value *= 0x846ca68bu;
  value ^= value >> 16;
  return value;
}

void append_u8(std::vector<std::byte> &out, uint8_t value) {
  out.push_back(static_cast<std::byte>(value));
}

void append_u16(std::vector<std::byte> &out, uint16_t value) {
  out.push_back(static_cast<std::byte>(value & 0xFFu));
  out.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
}

void write_u8(std::byte *out, size_t &offset, uint8_t value) {
  out[offset++] = static_cast<std::byte>(value);
}

void write_u16(std::byte *out, size_t &offset, uint16_t value) {
  out[offset++] = static_cast<std::byte>(value & 0xFFu);
  out[offset++] = static_cast<std::byte>((value >> 8) & 0xFFu);
}

[[nodiscard]] bool read_u8(const std::byte *payload, size_t payload_size,
                           size_t &offset, uint8_t &value) {
  if (offset + 1 > payload_size) {
    return false;
  }
  value = static_cast<uint8_t>(payload[offset]);
  ++offset;
  return true;
}

[[nodiscard]] bool read_u16(const std::byte *payload, size_t payload_size,
                            size_t &offset, uint16_t &value) {
  if (offset + 2 > payload_size) {
    return false;
  }
  value = static_cast<uint16_t>(static_cast<uint8_t>(payload[offset])) |
          (static_cast<uint16_t>(static_cast<uint8_t>(payload[offset + 1]))
           << 8u);
  offset += 2;
  return true;
}

[[nodiscard]] uint16_t quantize_sample(float value) {
  const float clamped = std::clamp(value, 0.0f, 1.0f);
  return static_cast<uint16_t>(clamped * 65535.0f + 0.5f);
}

[[nodiscard]] float dequantize_sample(uint16_t value) {
  return static_cast<float>(value) / 65535.0f;
}

void choose_tile_samples(uint32_t seed, uint16_t tile_pixels,
                         uint8_t sample_count,
                         std::array<uint8_t, 255> &out) {
  uint8_t selected_count = 0;
  for (uint16_t j = static_cast<uint16_t>(tile_pixels - sample_count);
       j < tile_pixels; ++j) {
    const uint16_t candidate = static_cast<uint16_t>(
        mix_bits(seed ^ (static_cast<uint32_t>(j) * 0x9e3779b9u)) % (j + 1u));
    bool already_selected = false;
    for (uint8_t i = 0; i < selected_count; ++i) {
      if (out[i] == candidate) {
        already_selected = true;
        break;
      }
    }
    out[selected_count++] =
        static_cast<uint8_t>(already_selected ? j : candidate);
  }
}

[[nodiscard]] std::vector<std::pair<uint16_t, uint16_t>>
enumerate_frequency_pairs(uint16_t tile_width, uint16_t tile_height,
                          uint8_t dictionary_atoms) {
  std::vector<std::pair<uint16_t, uint16_t>> freqs;
  freqs.reserve(static_cast<size_t>(tile_width) * tile_height);
  for (uint16_t sum = 0; sum < tile_width + tile_height - 1; ++sum) {
    for (uint16_t fy = 0; fy < tile_height; ++fy) {
      if (fy > sum) {
        continue;
      }
      const uint16_t fx = static_cast<uint16_t>(sum - fy);
      if (fx >= tile_width) {
        continue;
      }
      if (fx == 0 && fy == 0) {
        continue;
      }
      freqs.emplace_back(fx, fy);
      if (freqs.size() == dictionary_atoms) {
        return freqs;
      }
    }
  }
  return freqs;
}

[[nodiscard]] std::vector<std::vector<float>>
build_dictionary(uint16_t tile_width, uint16_t tile_height,
                 uint8_t dictionary_atoms) {
  const size_t tile_pixels =
      static_cast<size_t>(tile_width) * static_cast<size_t>(tile_height);
  std::vector<std::vector<float>> dictionary;
  dictionary.reserve(dictionary_atoms);
  auto add_atom = [&](auto &&fn) {
    if (dictionary.size() >= dictionary_atoms) {
      return;
    }
    std::vector<float> atom(tile_pixels, 0.0f);
    for (uint16_t y = 0; y < tile_height; ++y) {
      for (uint16_t x = 0; x < tile_width; ++x) {
        atom[static_cast<size_t>(y) * tile_width + x] = fn(x, y);
      }
    }
    dictionary.push_back(std::move(atom));
  };

  const float inv_pixels = 1.0f / std::sqrt(static_cast<float>(tile_pixels));
  add_atom([&](uint16_t, uint16_t) { return inv_pixels; });

  const float x_norm =
      std::sqrt(12.0f / static_cast<float>(tile_width * tile_height *
                                           (tile_width * tile_width - 1)));
  add_atom([&](uint16_t x, uint16_t) {
    return (static_cast<float>(x) -
            0.5f * static_cast<float>(tile_width - 1)) *
           x_norm;
  });

  const float y_norm =
      std::sqrt(12.0f / static_cast<float>(tile_width * tile_height *
                                           (tile_height * tile_height - 1)));
  add_atom([&](uint16_t, uint16_t y) {
    return (static_cast<float>(y) -
            0.5f * static_cast<float>(tile_height - 1)) *
           y_norm;
  });

  const auto freqs = enumerate_frequency_pairs(
      tile_width, tile_height,
      static_cast<uint8_t>(dictionary_atoms - dictionary.size()));
  for (const auto [fx, fy] : freqs) {
    std::vector<float> atom(tile_pixels, 0.0f);
    const float alpha_x = fx == 0
                              ? std::sqrt(1.0f / static_cast<float>(tile_width))
                              : std::sqrt(2.0f / static_cast<float>(tile_width));
    const float alpha_y =
        fy == 0 ? std::sqrt(1.0f / static_cast<float>(tile_height))
                : std::sqrt(2.0f / static_cast<float>(tile_height));
    for (uint16_t y = 0; y < tile_height; ++y) {
      for (uint16_t x = 0; x < tile_width; ++x) {
        const float basis =
            alpha_x * alpha_y *
            std::cos(std::numbers::pi_v<float> *
                     (static_cast<float>(2 * x + 1) * static_cast<float>(fx)) /
                     (2.0f * static_cast<float>(tile_width))) *
            std::cos(std::numbers::pi_v<float> *
                     (static_cast<float>(2 * y + 1) * static_cast<float>(fy)) /
                     (2.0f * static_cast<float>(tile_height)));
        atom[static_cast<size_t>(y) * tile_width + x] = basis;
      }
    }
    dictionary.push_back(std::move(atom));
  }
  return dictionary;
}

struct DictionaryCache {
  uint16_t tile_width{0};
  uint16_t tile_height{0};
  uint8_t dictionary_atoms{0};
  std::vector<std::vector<float>> dictionary;
};

[[nodiscard]] const std::vector<std::vector<float>> &
get_cached_dictionary(uint16_t tile_width, uint16_t tile_height,
                      uint8_t dictionary_atoms) {
  static DictionaryCache cache{};
  if (cache.tile_width != tile_width || cache.tile_height != tile_height ||
      cache.dictionary_atoms != dictionary_atoms || cache.dictionary.empty()) {
    cache.tile_width = tile_width;
    cache.tile_height = tile_height;
    cache.dictionary_atoms = dictionary_atoms;
    cache.dictionary =
        build_dictionary(tile_width, tile_height, dictionary_atoms);
  }
  return cache.dictionary;
}

[[nodiscard]] bool solve_linear_system(std::vector<float> &matrix,
                                       std::vector<float> &rhs, uint32_t n,
                                       std::vector<float> &out) {
  for (uint32_t pivot = 0; pivot < n; ++pivot) {
    uint32_t best = pivot;
    float best_abs = std::fabs(matrix[pivot * n + pivot]);
    for (uint32_t row = pivot + 1; row < n; ++row) {
      const float candidate = std::fabs(matrix[row * n + pivot]);
      if (candidate > best_abs) {
        best_abs = candidate;
        best = row;
      }
    }
    if (best_abs < 1e-8f) {
      return false;
    }
    if (best != pivot) {
      for (uint32_t col = 0; col < n; ++col) {
        std::swap(matrix[pivot * n + col], matrix[best * n + col]);
      }
      std::swap(rhs[pivot], rhs[best]);
    }

    const float inv_pivot = 1.0f / matrix[pivot * n + pivot];
    for (uint32_t col = pivot; col < n; ++col) {
      matrix[pivot * n + col] *= inv_pivot;
    }
    rhs[pivot] *= inv_pivot;

    for (uint32_t row = 0; row < n; ++row) {
      if (row == pivot) {
        continue;
      }
      const float factor = matrix[row * n + pivot];
      if (std::fabs(factor) < 1e-12f) {
        continue;
      }
      for (uint32_t col = pivot; col < n; ++col) {
        matrix[row * n + col] -= factor * matrix[pivot * n + col];
      }
      rhs[row] -= factor * rhs[pivot];
    }
  }

  out.resize(n);
  std::copy_n(rhs.begin(), n, out.begin());
  return true;
}

struct TileReconstructionScratch {
  std::vector<float> sampled_dictionary;
  std::vector<float> residual;
  std::vector<float> coefficients;
  std::vector<float> best_coefficients;
  std::vector<uint16_t> selected_atoms;
  std::vector<uint16_t> best_atoms;
  std::vector<uint8_t> atom_selected;
  std::vector<float> gram;
  std::vector<float> rhs;

  void reset(size_t dictionary_atoms, size_t sample_count, size_t max_atoms) {
    sampled_dictionary.resize(dictionary_atoms * sample_count);
    residual.resize(sample_count);
    coefficients.reserve(max_atoms);
    best_coefficients.reserve(max_atoms);
    selected_atoms.reserve(max_atoms);
    best_atoms.reserve(max_atoms);
    atom_selected.assign(dictionary_atoms, 0);
    gram.resize(max_atoms * max_atoms);
    rhs.resize(max_atoms);
  }
};

struct TilePayloadView {
  uint16_t tile_index{0};
  uint8_t sample_count{0};
  size_t sample_offset{0};
};

[[nodiscard]] bool solve_3x3(float matrix[3][3], float rhs[3],
                             float out[3]) {
  for (uint32_t pivot = 0; pivot < 3; ++pivot) {
    uint32_t best = pivot;
    float best_abs = std::fabs(matrix[pivot][pivot]);
    for (uint32_t row = pivot + 1; row < 3; ++row) {
      const float candidate = std::fabs(matrix[row][pivot]);
      if (candidate > best_abs) {
        best_abs = candidate;
        best = row;
      }
    }
    if (best_abs < 1e-8f) {
      return false;
    }
    if (best != pivot) {
      for (uint32_t col = 0; col < 3; ++col) {
        std::swap(matrix[pivot][col], matrix[best][col]);
      }
      std::swap(rhs[pivot], rhs[best]);
    }

    const float inv_pivot = 1.0f / matrix[pivot][pivot];
    for (uint32_t col = pivot; col < 3; ++col) {
      matrix[pivot][col] *= inv_pivot;
    }
    rhs[pivot] *= inv_pivot;

    for (uint32_t row = 0; row < 3; ++row) {
      if (row == pivot) {
        continue;
      }
      const float factor = matrix[row][pivot];
      if (std::fabs(factor) < 1e-12f) {
        continue;
      }
      for (uint32_t col = pivot; col < 3; ++col) {
        matrix[row][col] -= factor * matrix[pivot][col];
      }
      rhs[row] -= factor * rhs[pivot];
    }
  }

  out[0] = rhs[0];
  out[1] = rhs[1];
  out[2] = rhs[2];
  return true;
}

[[nodiscard]] bool try_reconstruct_affine_tile(
    const std::vector<std::vector<float>> &dictionary,
    const uint8_t *sample_positions, const float *sample_values,
    size_t sample_count, size_t tile_pixels, float sample_min,
    float sample_max, float sample_range, float *out_tile, float &sample_mae) {
  if (dictionary.size() < 3 || sample_count < 3) {
    return false;
  }

  float matrix[3][3]{};
  float rhs[3]{};
  for (size_t sample = 0; sample < sample_count; ++sample) {
    const uint8_t local = sample_positions[sample];
    const float basis[3] = {
        dictionary[0][local],
        dictionary[1][local],
        dictionary[2][local],
    };
    const float value = sample_values[sample];
    for (uint32_t row = 0; row < 3; ++row) {
      rhs[row] += basis[row] * value;
      for (uint32_t col = 0; col < 3; ++col) {
        matrix[row][col] += basis[row] * basis[col];
      }
    }
  }

  float coeffs[3]{};
  if (!solve_3x3(matrix, rhs, coeffs)) {
    return false;
  }

  sample_mae = 0.0f;
  float sample_max_abs = 0.0f;
  for (size_t sample = 0; sample < sample_count; ++sample) {
    const uint8_t local = sample_positions[sample];
    const float estimate = coeffs[0] * dictionary[0][local] +
                           coeffs[1] * dictionary[1][local] +
                           coeffs[2] * dictionary[2][local];
    const float error = std::fabs(sample_values[sample] - estimate);
    sample_mae += error;
    sample_max_abs = std::max(sample_max_abs, error);
  }
  sample_mae /= static_cast<float>(sample_count);

  const float smooth_mae_threshold =
      std::max(0.005f, 0.08f * std::max(sample_range, 0.02f));
  const float smooth_max_threshold =
      std::max(0.00035f, 0.025f * std::max(sample_range, 0.02f));
  if (sample_mae > smooth_mae_threshold ||
      sample_max_abs > smooth_max_threshold) {
    return false;
  }

  const float low_bound = std::max(0.0f, sample_min - 0.08f);
  const float high_bound = std::min(1.0f, sample_max + 0.08f);
  for (size_t pixel = 0; pixel < tile_pixels; ++pixel) {
    const float value = coeffs[0] * dictionary[0][pixel] +
                        coeffs[1] * dictionary[1][pixel] +
                        coeffs[2] * dictionary[2][pixel];
    out_tile[pixel] = std::clamp(value, low_bound, high_bound);
  }
  for (size_t sample = 0; sample < sample_count; ++sample) {
    out_tile[sample_positions[sample]] = sample_values[sample];
  }
  return true;
}

[[nodiscard]] float reconstruct_tile_omp(
    const std::vector<std::vector<float>> &dictionary, uint8_t max_omp_atoms,
    uint16_t tile_width, uint16_t tile_height, const uint8_t *sample_positions,
    const float *sample_values, size_t sample_count, float *out_tile,
    TileReconstructionScratch &scratch) {
  const size_t tile_pixels =
      static_cast<size_t>(tile_width) * static_cast<size_t>(tile_height);
  std::fill_n(out_tile, tile_pixels, 0.0f);
  if (sample_positions == nullptr || sample_values == nullptr ||
      sample_count == 0) {
    return 0.0f;
  }

  const size_t dictionary_atoms = dictionary.size();
  const uint32_t max_atoms = std::min<uint32_t>(
      {max_omp_atoms, static_cast<uint8_t>(dictionary_atoms),
       static_cast<uint8_t>(sample_count)});
  const uint32_t smooth_basis_count =
      std::min<uint32_t>({3u, static_cast<uint32_t>(dictionary_atoms),
                          static_cast<uint32_t>(sample_count)});

  float sample_min = sample_values[0];
  float sample_max = sample_values[0];
  for (size_t sample = 0; sample < sample_count; ++sample) {
    const float value = sample_values[sample];
    sample_min = std::min(sample_min, value);
    sample_max = std::max(sample_max, value);
  }
  const float sample_range = sample_max - sample_min;

  float affine_sample_mae = std::numeric_limits<float>::max();
  if (try_reconstruct_affine_tile(dictionary, sample_positions, sample_values,
                                  sample_count, tile_pixels, sample_min,
                                  sample_max, sample_range, out_tile,
                                  affine_sample_mae)) {
    return affine_sample_mae;
  }

  scratch.reset(dictionary_atoms, sample_count,
                std::max(max_atoms, smooth_basis_count));
  scratch.coefficients.clear();
  scratch.best_coefficients.clear();
  scratch.selected_atoms.clear();
  scratch.best_atoms.clear();

  const auto gather_sampled_atoms = [&](size_t begin_atom, size_t end_atom) {
    for (size_t atom = begin_atom; atom < end_atom; ++atom) {
      float *sampled_row =
          scratch.sampled_dictionary.data() + atom * sample_count;
      for (size_t sample = 0; sample < sample_count; ++sample) {
        sampled_row[sample] = dictionary[atom][sample_positions[sample]];
      }
    }
  };

  std::copy_n(sample_values, sample_count, scratch.residual.begin());
  float best_sample_mae = std::numeric_limits<float>::max();

  const auto solve_for_atoms = [&](const std::vector<uint16_t> &atoms,
                                   std::vector<float> &coeffs,
                                   float &sample_mae) -> bool {
    if (atoms.empty()) {
      return false;
    }

    const uint32_t active_atoms = static_cast<uint32_t>(atoms.size());
    std::fill_n(scratch.gram.data(), active_atoms * active_atoms, 0.0f);
    std::fill_n(scratch.rhs.data(), active_atoms, 0.0f);
    for (uint32_t row = 0; row < active_atoms; ++row) {
      const float *row_values =
          scratch.sampled_dictionary.data() +
          static_cast<size_t>(atoms[row]) * sample_count;
      for (size_t sample = 0; sample < sample_count; ++sample) {
        scratch.rhs[row] += row_values[sample] * sample_values[sample];
      }
      for (uint32_t col = 0; col < active_atoms; ++col) {
        const float *col_values =
            scratch.sampled_dictionary.data() +
            static_cast<size_t>(atoms[col]) * sample_count;
        float value = 0.0f;
        for (size_t sample = 0; sample < sample_count; ++sample) {
          value += row_values[sample] * col_values[sample];
        }
        scratch.gram[row * active_atoms + col] = value;
      }
    }

    if (!solve_linear_system(scratch.gram, scratch.rhs, active_atoms, coeffs)) {
      return false;
    }

    sample_mae = 0.0f;
    for (size_t sample = 0; sample < sample_count; ++sample) {
      float estimate = 0.0f;
      for (uint32_t atom = 0; atom < active_atoms; ++atom) {
        const float *atom_values =
            scratch.sampled_dictionary.data() +
            static_cast<size_t>(atoms[atom]) * sample_count;
        estimate += coeffs[atom] * atom_values[sample];
      }
      sample_mae += std::fabs(sample_values[sample] - estimate);
    }
    sample_mae /= static_cast<float>(sample_count);
    return true;
  };

  const auto render_from_atoms = [&](const std::vector<uint16_t> &atoms,
                                     const std::vector<float> &coeffs) {
    const float low_bound = std::max(0.0f, sample_min - 0.08f);
    const float high_bound = std::min(1.0f, sample_max + 0.08f);
    for (size_t pixel = 0; pixel < tile_pixels; ++pixel) {
      float value = 0.0f;
      for (size_t atom = 0; atom < atoms.size(); ++atom) {
        value += coeffs[atom] * dictionary[atoms[atom]][pixel];
      }
      out_tile[pixel] = std::clamp(value, low_bound, high_bound);
    }
    for (size_t sample = 0; sample < sample_count; ++sample) {
      out_tile[sample_positions[sample]] = sample_values[sample];
    }
  };

  scratch.selected_atoms.clear();
  for (uint16_t atom = 0; atom < smooth_basis_count; ++atom) {
    scratch.selected_atoms.push_back(atom);
  }
  gather_sampled_atoms(0, smooth_basis_count);
  float smooth_sample_mae = std::numeric_limits<float>::max();
  const bool smooth_ok =
      solve_for_atoms(scratch.selected_atoms, scratch.coefficients,
                      smooth_sample_mae);
  const bool tiny_system =
      sample_count <= static_cast<size_t>(smooth_basis_count + 1);
  const float smooth_threshold =
      std::max(0.005f, 0.08f * std::max(sample_range, 0.02f));
  if (smooth_ok && (tiny_system || smooth_sample_mae <= smooth_threshold)) {
    render_from_atoms(scratch.selected_atoms, scratch.coefficients);
    return smooth_sample_mae;
  }
  if (smooth_ok) {
    best_sample_mae = smooth_sample_mae;
    scratch.best_atoms = scratch.selected_atoms;
    scratch.best_coefficients = scratch.coefficients;
  }

  scratch.selected_atoms.clear();
  scratch.coefficients.clear();
  gather_sampled_atoms(smooth_basis_count, dictionary_atoms);
  for (uint32_t iter = 0; iter < max_atoms; ++iter) {
    int best_atom = -1;
    float best_corr = 0.0f;
    for (uint16_t atom = 0; atom < dictionary_atoms; ++atom) {
      if (scratch.atom_selected[atom]) {
        continue;
      }
      const float *atom_values =
          scratch.sampled_dictionary.data() + static_cast<size_t>(atom) * sample_count;
      float corr = 0.0f;
      for (size_t sample = 0; sample < sample_count; ++sample) {
        corr += atom_values[sample] * scratch.residual[sample];
      }
      if (std::fabs(corr) > std::fabs(best_corr)) {
        best_corr = corr;
        best_atom = atom;
      }
    }

    if (best_atom < 0 || std::fabs(best_corr) < 1e-5f) {
      break;
    }

    scratch.atom_selected[static_cast<size_t>(best_atom)] = true;
    scratch.selected_atoms.push_back(static_cast<uint16_t>(best_atom));
    float sample_mae = 0.0f;
    if (!solve_for_atoms(scratch.selected_atoms, scratch.coefficients,
                         sample_mae)) {
      break;
    }

    float residual_energy = 0.0f;
    for (size_t sample = 0; sample < sample_count; ++sample) {
      float estimate = 0.0f;
      for (uint32_t atom = 0; atom < scratch.selected_atoms.size(); ++atom) {
        const float *atom_values =
            scratch.sampled_dictionary.data() +
            static_cast<size_t>(scratch.selected_atoms[atom]) * sample_count;
        estimate += scratch.coefficients[atom] * atom_values[sample];
      }
      scratch.residual[sample] = sample_values[sample] - estimate;
      residual_energy += scratch.residual[sample] * scratch.residual[sample];
    }

    if (sample_mae <= best_sample_mae) {
      best_sample_mae = sample_mae;
      scratch.best_coefficients = scratch.coefficients;
      scratch.best_atoms = scratch.selected_atoms;
    }
    if (residual_energy / static_cast<float>(sample_count) < 1e-5f) {
      break;
    }
  }

  if (scratch.best_atoms.empty() || scratch.best_coefficients.empty()) {
    const float mean_value =
        std::accumulate(sample_values, sample_values + sample_count, 0.0f) /
        static_cast<float>(sample_count);
    std::fill_n(out_tile, tile_pixels, mean_value);
    for (size_t sample = 0; sample < sample_count; ++sample) {
      out_tile[sample_positions[sample]] = sample_values[sample];
    }
  } else {
    render_from_atoms(scratch.best_atoms, scratch.best_coefficients);
  }

  float mae = 0.0f;
  for (size_t sample = 0; sample < sample_count; ++sample) {
    mae += std::fabs(out_tile[sample_positions[sample]] - sample_values[sample]);
  }
  return mae / static_cast<float>(sample_count);
}

} // namespace

bool isValidCompressiveConfig(const CompressiveSamplingConfig &config,
                              uint32_t width, uint32_t height) {
  return config.sampling_ratio > 0.0f && config.sampling_ratio <= 1.0f &&
         config.min_samples_per_tile > 0 &&
         config.max_omp_atoms > 0 &&
         config.dictionary_atoms >= config.max_omp_atoms &&
         config.tile_width > 0 && config.tile_height > 0 &&
         width % config.tile_width == 0 && height % config.tile_height == 0 &&
         config.tile_width * config.tile_height <= 255;
}

std::vector<std::byte>
encodeCompressiveFramePayload(const float *frame_pixels, uint32_t width,
                              uint32_t height,
                              const CompressiveSamplingConfig &config,
                              uint32_t frame_seed,
                              CompressiveFrameStats *stats) {
  if (frame_pixels == nullptr || !isValidCompressiveConfig(config, width, height)) {
    return {};
  }

  const auto start = std::chrono::steady_clock::now();
  const uint16_t tiles_x = static_cast<uint16_t>(width / config.tile_width);
  const uint16_t tiles_y = static_cast<uint16_t>(height / config.tile_height);
  const uint16_t tile_count = static_cast<uint16_t>(tiles_x * tiles_y);
  const uint16_t tile_pixels =
      static_cast<uint16_t>(config.tile_width * config.tile_height);
  const size_t target_samples = std::max<size_t>(
      static_cast<size_t>(tile_count) * config.min_samples_per_tile,
      static_cast<size_t>(std::lround(config.sampling_ratio *
                                      static_cast<float>(width * height))));
  std::vector<float> tile_activity(tile_count, 0.0f);
  double total_activity = 0.0;
#pragma omp parallel for schedule(static) reduction(+ : total_activity)
  for (uint16_t tile_y = 0; tile_y < tiles_y; ++tile_y) {
    for (uint16_t tile_x = 0; tile_x < tiles_x; ++tile_x) {
      const uint16_t tile_index = static_cast<uint16_t>(tile_y * tiles_x + tile_x);
      const uint32_t base_x = static_cast<uint32_t>(tile_x) * config.tile_width;
      const uint32_t base_y = static_cast<uint32_t>(tile_y) * config.tile_height;
      float activity = 0.0f;
      for (uint16_t y = 0; y < config.tile_height; ++y) {
        for (uint16_t x = 0; x < config.tile_width; ++x) {
          const size_t index =
              static_cast<size_t>(base_y + y) * width + base_x + x;
          if (x + 1 < config.tile_width) {
            activity += std::fabs(frame_pixels[index] - frame_pixels[index + 1]);
          }
          if (y + 1 < config.tile_height) {
            activity += std::fabs(frame_pixels[index] - frame_pixels[index + width]);
          }
        }
      }
      tile_activity[tile_index] = activity + 1e-6f;
      total_activity += tile_activity[tile_index];
    }
  }

  std::vector<uint8_t> tile_sample_counts(tile_count,
                                          config.min_samples_per_tile);
  size_t assigned_samples =
      static_cast<size_t>(tile_count) * config.min_samples_per_tile;
  std::vector<double> tile_remainders(tile_count, 0.0);
  if (assigned_samples < target_samples) {
    const size_t remaining_budget = target_samples - assigned_samples;
    for (uint16_t tile_index = 0; tile_index < tile_count; ++tile_index) {
      const double ideal_extra =
          remaining_budget *
          (static_cast<double>(tile_activity[tile_index]) / total_activity);
      const uint8_t extra =
          static_cast<uint8_t>(std::min<double>(tile_pixels - tile_sample_counts[tile_index],
                                                std::floor(ideal_extra)));
      tile_sample_counts[tile_index] =
          static_cast<uint8_t>(tile_sample_counts[tile_index] + extra);
      tile_remainders[tile_index] = ideal_extra - std::floor(ideal_extra);
      assigned_samples += extra;
    }

    std::vector<uint16_t> remainder_order(tile_count);
    std::iota(remainder_order.begin(), remainder_order.end(), uint16_t{0});
    std::sort(remainder_order.begin(), remainder_order.end(),
              [&](uint16_t lhs, uint16_t rhs) {
                if (tile_remainders[lhs] == tile_remainders[rhs]) {
                  return tile_activity[lhs] > tile_activity[rhs];
                }
                return tile_remainders[lhs] > tile_remainders[rhs];
              });

    for (const uint16_t tile_index : remainder_order) {
      if (assigned_samples >= target_samples) {
        break;
      }
      if (tile_sample_counts[tile_index] < tile_pixels) {
        ++tile_sample_counts[tile_index];
        ++assigned_samples;
      }
    }

    if (assigned_samples < target_samples) {
      std::sort(remainder_order.begin(), remainder_order.end(),
                [&](uint16_t lhs, uint16_t rhs) {
                  return tile_activity[lhs] > tile_activity[rhs];
                });
      for (const uint16_t tile_index : remainder_order) {
        if (assigned_samples >= target_samples) {
          break;
        }
        const size_t capacity =
            static_cast<size_t>(tile_pixels - tile_sample_counts[tile_index]);
        const size_t fill =
            std::min(capacity, target_samples - assigned_samples);
        tile_sample_counts[tile_index] =
            static_cast<uint8_t>(tile_sample_counts[tile_index] + fill);
        assigned_samples += fill;
      }
    }
  }

  const size_t total_samples = std::accumulate(
      tile_sample_counts.begin(), tile_sample_counts.end(), size_t{0});
  std::vector<size_t> tile_payload_offsets(tile_count, 0);
  size_t payload_size = 14;
  for (uint16_t tile_index = 0; tile_index < tile_count; ++tile_index) {
    tile_payload_offsets[tile_index] = payload_size;
    payload_size += 4 + static_cast<size_t>(tile_sample_counts[tile_index]) * 3;
  }

  std::vector<std::byte> payload(payload_size);
  size_t header_offset = 0;
  write_u16(payload.data(), header_offset, config.tile_width);
  write_u16(payload.data(), header_offset, config.tile_height);
  write_u16(payload.data(), header_offset, tiles_x);
  write_u16(payload.data(), header_offset, tiles_y);
  write_u8(payload.data(), header_offset, config.max_omp_atoms);
  write_u8(payload.data(), header_offset, config.dictionary_atoms);
  write_u16(payload.data(), header_offset, tile_count);
  write_u16(payload.data(), header_offset, 0);

#pragma omp parallel for schedule(static)
  for (uint32_t tile_index = 0; tile_index < tile_count; ++tile_index) {
    const uint16_t tile_x = static_cast<uint16_t>(tile_index % tiles_x);
    const uint16_t tile_y = static_cast<uint16_t>(tile_index / tiles_x);
    const uint8_t sample_count = tile_sample_counts[tile_index];
    const uint32_t tile_seed =
        mix_bits(frame_seed ^ config.sampling_seed ^
                 (static_cast<uint32_t>(tile_index) * 1315423911u));
    std::array<uint8_t, 255> sampled_positions{};
    choose_tile_samples(tile_seed, tile_pixels, sample_count,
                        sampled_positions);

    size_t write_offset = tile_payload_offsets[tile_index];
    write_u16(payload.data(), write_offset, static_cast<uint16_t>(tile_index));
    write_u8(payload.data(), write_offset, sample_count);
    write_u8(payload.data(), write_offset, 0);

    const uint32_t base_x = static_cast<uint32_t>(tile_x) * config.tile_width;
    const uint32_t base_y = static_cast<uint32_t>(tile_y) * config.tile_height;
    for (uint8_t sample_index = 0; sample_index < sample_count; ++sample_index) {
      const uint16_t local = sampled_positions[sample_index];
      const uint32_t local_x = local % config.tile_width;
      const uint32_t local_y = local / config.tile_width;
      const size_t frame_offset =
          static_cast<size_t>(base_y + local_y) * width + base_x + local_x;
      write_u8(payload.data(), write_offset, static_cast<uint8_t>(local));
      write_u16(payload.data(), write_offset,
                quantize_sample(frame_pixels[frame_offset]));
    }
  }

  if (stats != nullptr) {
    stats->tile_count = static_cast<size_t>(tiles_x) * tiles_y;
    stats->sample_count = total_samples;
    stats->payload_bytes = payload.size();
    stats->sample_ratio =
        static_cast<double>(total_samples) /
        static_cast<double>(static_cast<size_t>(width) * height);
    stats->payload_ratio =
        static_cast<double>(payload.size()) /
        static_cast<double>(static_cast<size_t>(width) * height * sizeof(float));
    stats->elapsed_ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start)
                            .count();
  }

  return payload;
}

bool reconstructCompressiveFramePayload(const std::byte *payload_bytes,
                                        size_t payload_size, uint32_t width,
                                        uint32_t height, float *out_pixels,
                                        CompressiveFrameStats *stats) {
  if (payload_bytes == nullptr || out_pixels == nullptr || payload_size < 14) {
    return false;
  }

  const auto start = std::chrono::steady_clock::now();
  size_t offset = 0;
  CompressivePayloadHeader header{};
  if (!read_u16(payload_bytes, payload_size, offset, header.tile_width) ||
      !read_u16(payload_bytes, payload_size, offset, header.tile_height) ||
      !read_u16(payload_bytes, payload_size, offset, header.tiles_x) ||
      !read_u16(payload_bytes, payload_size, offset, header.tiles_y) ||
      !read_u8(payload_bytes, payload_size, offset, header.max_omp_atoms) ||
      !read_u8(payload_bytes, payload_size, offset, header.dictionary_atoms) ||
      !read_u16(payload_bytes, payload_size, offset, header.tile_count) ||
      !read_u16(payload_bytes, payload_size, offset, header.reserved)) {
    return false;
  }

  if (header.tile_width == 0 || header.tile_height == 0 ||
      header.tiles_x == 0 || header.tiles_y == 0 ||
      header.tile_width * header.tiles_x != width ||
      header.tile_height * header.tiles_y != height ||
      header.dictionary_atoms < header.max_omp_atoms) {
    return false;
  }

  const auto &dictionary = get_cached_dictionary(
      header.tile_width, header.tile_height, header.dictionary_atoms);
  const size_t tile_pixels =
      static_cast<size_t>(header.tile_width) * header.tile_height;
  if (tile_pixels > 255) {
    return false;
  }
  std::vector<TilePayloadView> tile_views;
  tile_views.reserve(header.tile_count);
  std::vector<uint8_t> tile_seen(header.tile_count, 0);
  size_t total_samples = 0;
  double total_mae = 0.0;
  float max_abs_error = 0.0f;

  for (uint16_t tile = 0; tile < header.tile_count; ++tile) {
    CompressiveTileHeader tile_header{};
    if (!read_u16(payload_bytes, payload_size, offset, tile_header.tile_index) ||
        !read_u8(payload_bytes, payload_size, offset, tile_header.sample_count) ||
        !read_u8(payload_bytes, payload_size, offset, tile_header.reserved)) {
      return false;
    }
    if (tile_header.tile_index >= header.tile_count ||
        tile_header.sample_count == 0 || tile_seen[tile_header.tile_index]) {
      return false;
    }
    tile_seen[tile_header.tile_index] = 1;
    const size_t tile_sample_bytes =
        static_cast<size_t>(tile_header.sample_count) * 3;
    if (offset + tile_sample_bytes > payload_size) {
      return false;
    }
    size_t validate_offset = offset;
    for (uint8_t sample = 0; sample < tile_header.sample_count; ++sample) {
      uint8_t local_index = 0;
      uint16_t quantized_value = 0;
      if (!read_u8(payload_bytes, payload_size, validate_offset, local_index) ||
          !read_u16(payload_bytes, payload_size, validate_offset,
                    quantized_value) ||
          local_index >= tile_pixels) {
        return false;
      }
    }
    tile_views.push_back(TilePayloadView{
        .tile_index = tile_header.tile_index,
        .sample_count = tile_header.sample_count,
        .sample_offset = offset,
    });
    total_samples += tile_header.sample_count;
    offset += tile_sample_bytes;
  }

  if (offset != payload_size) {
    return false;
  }

#pragma omp parallel for schedule(static) reduction(+ : total_mae) reduction(max : max_abs_error)
  for (int tile = 0; tile < static_cast<int>(tile_views.size()); ++tile) {
    const TilePayloadView &view = tile_views[static_cast<size_t>(tile)];
    std::array<uint8_t, 255> sample_positions{};
    std::array<float, 255> sample_values{};
    std::array<float, 255> tile_buffer{};
    static thread_local TileReconstructionScratch scratch;

    size_t sample_offset = view.sample_offset;
    for (uint8_t sample = 0; sample < view.sample_count; ++sample) {
      uint8_t local_index = static_cast<uint8_t>(payload_bytes[sample_offset]);
      const uint16_t quantized_value =
          static_cast<uint16_t>(static_cast<uint8_t>(payload_bytes[sample_offset + 1])) |
          (static_cast<uint16_t>(static_cast<uint8_t>(payload_bytes[sample_offset + 2]))
           << 8u);
      sample_offset += 3;
      sample_positions[sample] = local_index;
      sample_values[sample] = dequantize_sample(quantized_value);
    }

    const float sample_mae = reconstruct_tile_omp(
        dictionary, header.max_omp_atoms, header.tile_width, header.tile_height,
        sample_positions.data(), sample_values.data(), view.sample_count,
        tile_buffer.data(), scratch);
    total_mae += sample_mae;

    const uint32_t tile_x = view.tile_index % header.tiles_x;
    const uint32_t tile_y = view.tile_index / header.tiles_x;
    const uint32_t base_x = tile_x * header.tile_width;
    const uint32_t base_y = tile_y * header.tile_height;
    for (uint16_t y = 0; y < header.tile_height; ++y) {
      const size_t src_offset = static_cast<size_t>(y) * header.tile_width;
      const size_t dst_offset =
          static_cast<size_t>(base_y + y) * width + base_x;
      std::copy_n(tile_buffer.data() + src_offset, header.tile_width,
                  out_pixels + dst_offset);
    }

    for (uint8_t sample = 0; sample < view.sample_count; ++sample) {
      max_abs_error =
          std::max(max_abs_error,
                   std::fabs(tile_buffer[sample_positions[sample]] -
                             sample_values[sample]));
    }
  }

  if (stats != nullptr) {
    stats->tile_count = header.tile_count;
    stats->sample_count = total_samples;
    stats->payload_bytes = payload_size;
    stats->sample_ratio =
        static_cast<double>(total_samples) /
        static_cast<double>(static_cast<size_t>(width) * height);
    stats->payload_ratio =
        static_cast<double>(payload_size) /
        static_cast<double>(static_cast<size_t>(width) * height * sizeof(float));
    stats->mean_abs_error =
        header.tile_count == 0 ? 0.0
                               : total_mae / static_cast<double>(header.tile_count);
    stats->max_abs_error = max_abs_error;
    stats->elapsed_ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start)
                            .count();
  }

  return true;
}

} // namespace NetDSP
