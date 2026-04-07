#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace NetDSP {

inline constexpr size_t CACHE_LINE_BYTES = 64;

/**
 * Bounded single-producer/single-consumer ring buffer.
 *
 * Design notes:
 * - Capacity must be a power of two for mask-based indexing.
 * - The queue uses monotonic head/tail counters, so all Capacity slots are
 *   usable without sacrificing a sentinel slot.
 * - T is constrained to trivially copyable payloads, which matches descriptor
 *   passing in the transport pipeline.
 */
template <typename T, size_t Capacity> class SPSCQueue {
  static_assert(Capacity > 1, "SPSCQueue Capacity must be greater than 1");
  static_assert(std::has_single_bit(Capacity),
                "SPSCQueue Capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>,
                "SPSCQueue requires trivially copyable T");

  static constexpr size_t INDEX_MASK = Capacity - 1;

  struct alignas(CACHE_LINE_BYTES) ProducerState {
    std::atomic<size_t> tail{0};
  };

  struct alignas(CACHE_LINE_BYTES) ConsumerState {
    std::atomic<size_t> head{0};
  };

  ProducerState producer_{};
  ConsumerState consumer_{};
  alignas(CACHE_LINE_BYTES) std::array<T, Capacity> buffer_{};

public:
  [[nodiscard]] static constexpr size_t capacity() { return Capacity; }

  [[nodiscard]] bool push(const T &item) {
    return pushImpl(item);
  }

  [[nodiscard]] bool push(T &&item) {
    return pushImpl(std::move(item));
  }

  [[nodiscard]] bool pop(T &item) {
    const size_t head = consumer_.head.load(std::memory_order_relaxed);
    const size_t tail = producer_.tail.load(std::memory_order_acquire);

    if (head == tail) {
      return false;
    }

    item = buffer_[head & INDEX_MASK];
    consumer_.head.store(head + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool empty() const {
    return size() == 0;
  }

  [[nodiscard]] bool full() const {
    return size() == Capacity;
  }

  [[nodiscard]] size_t size() const {
    const size_t head = consumer_.head.load(std::memory_order_acquire);
    const size_t tail = producer_.tail.load(std::memory_order_acquire);
    return tail - head;
  }

private:
  template <typename U> [[nodiscard]] bool pushImpl(U &&item) {
    const size_t tail = producer_.tail.load(std::memory_order_relaxed);
    const size_t head = consumer_.head.load(std::memory_order_acquire);

    if (tail - head == Capacity) {
      return false;
    }

    buffer_[tail & INDEX_MASK] = std::forward<U>(item);
    producer_.tail.store(tail + 1, std::memory_order_release);
    return true;
  }
};

} // namespace NetDSP
