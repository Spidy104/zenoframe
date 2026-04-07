#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace NetDSP {

struct RefreshSpan {
  uint32_t start_row{0};
  uint32_t row_count{0};

  [[nodiscard]] constexpr bool contains(uint32_t row) const {
    return row >= start_row && row < start_row + row_count;
  }
};

struct RefreshPlan {
  uint64_t frame_index{0};
  uint32_t rows_per_frame{0};
  uint32_t total_rows{0};
  std::array<RefreshSpan, 2> spans{};
  uint8_t span_count{0};

  [[nodiscard]] bool coversRow(uint32_t row) const {
    for (uint8_t index = 0; index < span_count; ++index) {
      if (spans[index].contains(row)) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] uint32_t payloadRowCount() const {
    uint32_t rows = 0;
    for (uint8_t index = 0; index < span_count; ++index) {
      rows += spans[index].row_count;
    }
    return rows;
  }
};

struct RefreshPayloadSlice {
  uint32_t start_row{0};
  uint32_t row_count{0};
  size_t payload_offset_rows{0};
};

struct RefreshPayloadLayout {
  uint32_t width{0};
  uint8_t quantization{0};
  std::array<RefreshPayloadSlice, 2> slices{};
  uint8_t slice_count{0};
  size_t total_payload_rows{0};

  [[nodiscard]] size_t bytesPerSample() const {
    return quantization / 8u;
  }

  [[nodiscard]] size_t totalPayloadBytes() const {
    return total_payload_rows * static_cast<size_t>(width) * bytesPerSample();
  }
};

[[nodiscard]] inline RefreshPayloadLayout
makeRefreshPayloadLayout(const RefreshPlan &plan, uint32_t width,
                         uint8_t quantization) {
  if (width == 0 || (quantization != 8 && quantization != 16 && quantization != 32)) {
    throw std::invalid_argument("invalid width or quantization");
  }

  RefreshPayloadLayout layout{
      .width = width,
      .quantization = quantization,
      .slice_count = plan.span_count,
      .total_payload_rows = plan.payloadRowCount(),
  };

  size_t offset_rows = 0;
  for (uint8_t index = 0; index < plan.span_count; ++index) {
    layout.slices[index] = RefreshPayloadSlice{
        .start_row = plan.spans[index].start_row,
        .row_count = plan.spans[index].row_count,
        .payload_offset_rows = offset_rows,
    };
    offset_rows += plan.spans[index].row_count;
  }
  return layout;
}

class DistributedIntraRefreshScheduler {
  uint32_t frame_height_;
  uint32_t rows_per_frame_;

public:
  DistributedIntraRefreshScheduler(uint32_t frame_height,
                                   uint32_t rows_per_frame)
      : frame_height_(frame_height), rows_per_frame_(rows_per_frame) {
    if (frame_height_ == 0 || rows_per_frame_ == 0) {
      throw std::invalid_argument("frame_height and rows_per_frame must be > 0");
    }
    if (rows_per_frame_ > frame_height_) {
      throw std::invalid_argument(
          "rows_per_frame must not exceed frame_height");
    }
  }

  [[nodiscard]] uint32_t frameHeight() const { return frame_height_; }
  [[nodiscard]] uint32_t rowsPerFrame() const { return rows_per_frame_; }

  [[nodiscard]] uint64_t fullCycleFrameCount() const {
    return (frame_height_ + rows_per_frame_ - 1u) / rows_per_frame_;
  }

  [[nodiscard]] RefreshPlan planForFrame(uint64_t frame_index) const {
    const uint32_t start_row =
        static_cast<uint32_t>((frame_index * rows_per_frame_) % frame_height_);
    const uint32_t first_span_rows =
        std::min(rows_per_frame_, frame_height_ - start_row);
    const uint32_t wrapped_rows = rows_per_frame_ - first_span_rows;

    RefreshPlan plan{
        .frame_index = frame_index,
        .rows_per_frame = rows_per_frame_,
        .total_rows = rows_per_frame_,
        .spans = {{{start_row, first_span_rows}, {0, wrapped_rows}}},
        .span_count = static_cast<uint8_t>(wrapped_rows > 0 ? 2 : 1),
    };
    return plan;
  }
};

[[nodiscard]] inline RefreshPlan makeRefreshPlanFromWindow(
    uint32_t frame_height, uint32_t refresh_start_row,
    uint32_t refresh_row_count) {
  if (frame_height == 0 || refresh_row_count == 0 ||
      refresh_row_count > frame_height || refresh_start_row >= frame_height) {
    throw std::invalid_argument("invalid refresh window");
  }

  const uint32_t first_span_rows =
      std::min(refresh_row_count, frame_height - refresh_start_row);
  const uint32_t wrapped_rows = refresh_row_count - first_span_rows;

  return RefreshPlan{
      .frame_index = 0,
      .rows_per_frame = refresh_row_count,
      .total_rows = refresh_row_count,
      .spans = {{{refresh_start_row, first_span_rows}, {0, wrapped_rows}}},
      .span_count = static_cast<uint8_t>(wrapped_rows > 0 ? 2 : 1),
  };
}

class TemporalRefreshReconstructor {
  uint32_t width_;
  uint32_t height_;
  std::vector<float> pixels_;

public:
  TemporalRefreshReconstructor(uint32_t width, uint32_t height,
                               float initial_value = 0.0f)
      : width_(width), height_(height),
        pixels_(static_cast<size_t>(width) * height, initial_value) {
    if (width_ == 0 || height_ == 0) {
      throw std::invalid_argument("width and height must be > 0");
    }
  }

  [[nodiscard]] uint32_t width() const { return width_; }
  [[nodiscard]] uint32_t height() const { return height_; }
  [[nodiscard]] const float *data() const { return pixels_.data(); }
  [[nodiscard]] float *data() { return pixels_.data(); }

  [[nodiscard]] float at(uint32_t x, uint32_t y) const {
    return pixels_[static_cast<size_t>(y) * width_ + x];
  }

  void applyRefreshPlan(const RefreshPlan &plan, const float *source_frame) {
    if (source_frame == nullptr) {
      throw std::invalid_argument("source_frame must not be null");
    }

    for (uint8_t index = 0; index < plan.span_count; ++index) {
      const RefreshSpan span = plan.spans[index];
      for (uint32_t row = 0; row < span.row_count; ++row) {
        const uint32_t frame_row = span.start_row + row;
        const size_t offset = static_cast<size_t>(frame_row) * width_;
        std::copy_n(source_frame + offset, width_, pixels_.data() + offset);
      }
    }
  }

  void applyRefreshPayload(const RefreshPayloadLayout &layout,
                           const float *payload_rows) {
    if (payload_rows == nullptr) {
      throw std::invalid_argument("payload_rows must not be null");
    }
    if (layout.width != width_ || layout.quantization != 32) {
      throw std::invalid_argument("payload layout does not match reference buffer");
    }

    for (uint8_t index = 0; index < layout.slice_count; ++index) {
      const RefreshPayloadSlice slice = layout.slices[index];
      for (uint32_t row = 0; row < slice.row_count; ++row) {
        const uint32_t frame_row = slice.start_row + row;
        const size_t frame_offset = static_cast<size_t>(frame_row) * width_;
        const size_t payload_offset =
            (slice.payload_offset_rows + row) * static_cast<size_t>(width_);
        std::copy_n(payload_rows + payload_offset, width_,
                    pixels_.data() + frame_offset);
      }
    }
  }
};

[[nodiscard]] inline std::vector<float>
extractRefreshPayload(const RefreshPlan &plan, const float *source_frame,
                      uint32_t width) {
  if (source_frame == nullptr || width == 0) {
    throw std::invalid_argument("source_frame must not be null and width > 0");
  }

  std::vector<float> payload(static_cast<size_t>(plan.payloadRowCount()) * width,
                             0.0f);
  size_t payload_row = 0;

  for (uint8_t index = 0; index < plan.span_count; ++index) {
    const RefreshSpan span = plan.spans[index];
    for (uint32_t row = 0; row < span.row_count; ++row) {
      const size_t frame_offset =
          static_cast<size_t>(span.start_row + row) * width;
      const size_t payload_offset = payload_row * static_cast<size_t>(width);
      std::memcpy(payload.data() + payload_offset, source_frame + frame_offset,
                  static_cast<size_t>(width) * sizeof(float));
      ++payload_row;
    }
  }

  return payload;
}

} // namespace NetDSP