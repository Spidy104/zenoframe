#include "FrameBufferPool.hpp"
#include "SPSCQueue.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

void test_queue_basic_fifo() {
  std::print("Test: SPSCQueue Basic FIFO... ");

  NetDSP::SPSCQueue<uint32_t, 8> queue;
  assert(queue.empty());
  assert(!queue.full());
  assert(queue.capacity() == 8);

  assert(queue.push(10));
  assert(queue.push(20));
  assert(queue.push(30));
  assert(queue.size() == 3);

  uint32_t value = 0;
  assert(queue.pop(value) && value == 10);
  assert(queue.pop(value) && value == 20);
  assert(queue.pop(value) && value == 30);
  assert(!queue.pop(value));
  assert(queue.empty());

  std::println("PASSED");
}

void test_queue_full_and_wraparound() {
  std::print("Test: SPSCQueue Full Capacity + Wraparound... ");

  NetDSP::SPSCQueue<uint32_t, 4> queue;
  uint32_t value = 0;

  assert(queue.push(1));
  assert(queue.push(2));
  assert(queue.push(3));
  assert(queue.push(4));
  assert(queue.full());
  assert(!queue.push(5));

  assert(queue.pop(value) && value == 1);
  assert(queue.pop(value) && value == 2);
  assert(queue.push(5));
  assert(queue.push(6));

  assert(queue.pop(value) && value == 3);
  assert(queue.pop(value) && value == 4);
  assert(queue.pop(value) && value == 5);
  assert(queue.pop(value) && value == 6);
  assert(queue.empty());

  std::println("PASSED");
}

void test_queue_threaded_stress() {
  std::print("Test: SPSCQueue Producer/Consumer Stress... ");

  NetDSP::SPSCQueue<uint32_t, 1024> queue;
  constexpr uint32_t iterations = 200000;
  std::atomic<bool> producer_done{false};
  std::atomic<bool> consumer_done{false};

  std::thread producer([&]() {
    for (uint32_t i = 0; i < iterations; ++i) {
      while (!queue.push(i)) {
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&]() {
    uint32_t expected = 0;
    uint32_t value = 0;

    while (expected < iterations) {
      if (queue.pop(value)) {
        assert(value == expected);
        ++expected;
        continue;
      }

      if (!producer_done.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }

    consumer_done.store(true, std::memory_order_release);
  });

  producer.join();
  consumer.join();

  assert(producer_done.load(std::memory_order_acquire));
  assert(consumer_done.load(std::memory_order_acquire));
  assert(queue.empty());

  std::println("PASSED");
}

NetDSP::PacketHeader make_header(uint32_t sequence, uint16_t frame_id,
                                 uint16_t fragment_index) {
  return NetDSP::PacketHeader{
      .magic = NetDSP::MAGIC_NUMBER,
      .sequence = sequence,
      .frame_id = frame_id,
      .fragment_index = fragment_index,
      .total_fragments = NetDSP::SHADOW_TOTAL_FRAGMENTS,
      .timestamp_us = 456789,
      .type_flags = NetDSP::FLAG_I_FRAME | NetDSP::FLAG_CS_ENABLED,
      .quantization = NetDSP::SHADOW_FRAME_QUANTIZATION,
  };
}

void fill_payload(std::vector<std::byte> &payload, uint8_t value) {
  std::fill(payload.begin(), payload.end(), std::byte{value});
}

void test_out_of_order_fragment_tracking() {
  std::print("Test: Out-of-Order Fragment Tracking... ");

  NetDSP::FrameBufferPool<2> pool;
  const auto leased = pool.tryLeaseFreeSlot();
  assert(leased.has_value());

  std::vector<std::byte> payload(NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES);
  fill_payload(payload, 0x22);

  const auto result1 =
      pool.writeFragment(*leased, make_header(77, 9, 1), payload.data(),
                         payload.size(), 1000, 10000);
  assert(result1.accepted);
  assert(!result1.duplicate);
  assert(!result1.frame_complete);
  assert(pool.hasReceivedFragment(*leased, 1));
  assert(!pool.hasReceivedFragment(*leased, 0));
  assert(pool.assemblyState(*leased).received_fragments == 1);

  const auto duplicate =
      pool.writeFragment(*leased, make_header(77, 9, 1), payload.data(),
                         payload.size(), 1100, 10000);
  assert(!duplicate.accepted);
  assert(duplicate.duplicate);
  assert(!duplicate.frame_complete);
  assert(pool.assemblyState(*leased).received_fragments == 1);

  fill_payload(payload, 0x11);
  const auto result0 =
      pool.writeFragment(*leased, make_header(77, 9, 0), payload.data(),
                         payload.size(), 1200, 10000);
  assert(result0.accepted);
  assert(!result0.frame_complete);
  assert(pool.hasReceivedFragment(*leased, 0));
  assert(pool.assemblyState(*leased).received_fragments == 2);
  assert(pool.state(*leased) == NetDSP::SlotState::Filling);

  std::println("PASSED");
}

void test_shadow_buffer_manager() {
  std::print("Test: Shadow Buffer Commit + Release... ");

  NetDSP::FrameBufferPool<2> pool;
  NetDSP::SPSCQueue<uint16_t, 8> ready_queue;

  static_assert(NetDSP::FrameBufferPool<2>::slotBytes() ==
                NetDSP::SHADOW_BUFFER_BYTES);
  static_assert(NetDSP::FrameBufferPool<2>::pixelCount() ==
                NetDSP::SHADOW_PIXEL_COUNT);
  assert(NetDSP::FrameBufferPool<2>::frameWidth() == 1920);
  assert(NetDSP::FrameBufferPool<2>::frameHeight() == 1080);
  assert(NetDSP::FrameBufferPool<2>::quantization() == 32);
  assert(NetDSP::FrameBufferPool<2>::totalFragments() ==
         NetDSP::SHADOW_TOTAL_FRAGMENTS);

  const auto leased = pool.tryLeaseFreeSlot();
  assert(leased.has_value());
  assert(pool.state(*leased) == NetDSP::SlotState::Filling);

  auto &slot = pool.slot(*leased);
  const auto address = reinterpret_cast<uintptr_t>(slot.data());
  assert(address % NetDSP::SHADOW_BUFFER_ALIGNMENT == 0);

  std::vector<std::byte> payload(NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES);
  std::vector<std::byte> last_payload(
      NetDSP::FrameBufferPool<2>::expectedFragmentPayloadBytes(
          NetDSP::SHADOW_TOTAL_FRAGMENTS - 1));

  for (uint16_t fragment_index = NetDSP::SHADOW_TOTAL_FRAGMENTS; fragment_index-- > 0;) {
    const auto header = make_header(99, 17, fragment_index);

    if (fragment_index == NetDSP::SHADOW_TOTAL_FRAGMENTS - 1) {
      fill_payload(last_payload, 0xEE);
      const auto result = pool.writeFragment(*leased, header,
                                             last_payload.data(),
                                             last_payload.size(), 2000, 10000);
      assert(result.accepted);
      assert(result.frame_complete == (fragment_index == 0));
      continue;
    }

    fill_payload(payload, static_cast<uint8_t>(fragment_index % 251));
    const auto result = pool.writeFragment(*leased, header, payload.data(),
                                           payload.size(), 2000, 10000);
    assert(result.accepted);
    if (fragment_index == 0) {
      assert(result.frame_complete);
    } else {
      assert(!result.frame_complete);
    }
  }

  assert(pool.state(*leased) == NetDSP::SlotState::Ready);
  const NetDSP::FrameDescriptor descriptor = pool.descriptor(*leased);
  assert(descriptor.sequence == 99);
  assert(descriptor.slot_index == *leased);
  assert(descriptor.frame_id == 17);
  assert(descriptor.bytes_used == NetDSP::SHADOW_BUFFER_BYTES);
  assert(descriptor.quantization == 32);
  assert(descriptor.isComplete());
  assert(!descriptor.isPartial());
  assert(descriptor.missing_fragments == 0);

  assert(slot.bytes()[0] == std::byte{0x00});
  assert(slot.bytes()[NetDSP::MAX_FRAGMENT_PAYLOAD_BYTES] == std::byte{0x01});
  assert(slot.bytes()[NetDSP::SHADOW_BUFFER_BYTES - 1] == std::byte{0xEE});

  assert(ready_queue.push(*leased));

  uint16_t ready_index = 0;
  assert(ready_queue.pop(ready_index));
  assert(ready_index == *leased);
  assert(pool.tryAcquireReadySlot(ready_index));
  assert(pool.state(ready_index) == NetDSP::SlotState::Processing);

  pool.releaseProcessedSlot(ready_index);
  assert(pool.state(ready_index) == NetDSP::SlotState::Free);

  const NetDSP::FrameDescriptor cleared = pool.descriptor(ready_index);
  assert(cleared.bytes_used == 0);
  assert(cleared.sequence == 0);
  assert(cleared.total_fragments == 0);
  assert(pool.assemblyState(ready_index).received_fragments == 0);

  std::println("PASSED");
}

int main() {
  std::println("==========================================================");
  std::println("SPSC Queue and Buffer Pool Tests");
  std::println("==========================================================\n");

  test_queue_basic_fifo();
  test_queue_full_and_wraparound();
  test_queue_threaded_stress();
  test_out_of_order_fragment_tracking();
  test_shadow_buffer_manager();

  std::println("\n==========================================================");
  std::println("All SPSC queue tests PASSED!");
  std::println("==========================================================");

  return 0;
}
