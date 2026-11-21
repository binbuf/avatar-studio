#include "test_registry.h"

#include "avatar/core/spsc_ring.h"

#include <atomic>
#include <cstdint>
#include <thread>

namespace {

using avatar::core::DropOldestRing;
using avatar::core::SpscRing;

constexpr std::uint32_t kStressItems = 1'000'000;

} // namespace

TEST_CASE(spsc_ring_preserves_fifo_order_across_wrap) {
    SpscRing<std::uint32_t, 4> ring;
    CHECK(ring.empty());
    CHECK_EQ(ring.capacity(), std::size_t{4});

    for (std::uint32_t value = 0; value < 4; ++value) {
        CHECK(ring.try_push(value));
    }
    CHECK(ring.full());
    CHECK_EQ(ring.size(), std::size_t{4});

    std::uint32_t value = 0;
    for (std::uint32_t expected = 0; expected < 4; ++expected) {
        CHECK(ring.try_pop(value));
        CHECK_EQ(value, expected);
    }
    CHECK(ring.empty());

    // Push again after the read index wrapped; the ring must reuse the slots
    // in order, not overwrite unread items.
    for (std::uint32_t next = 10; next < 14; ++next) {
        CHECK(ring.try_push(next));
    }
    for (std::uint32_t expected = 10; expected < 14; ++expected) {
        CHECK(ring.try_pop(value));
        CHECK_EQ(value, expected);
    }
    CHECK_EQ(ring.dropped(), std::uint64_t{0});
}

TEST_CASE(spsc_ring_rejects_when_full_and_counts_drops) {
    SpscRing<std::uint32_t, 2> ring;
    CHECK(ring.try_push(1));
    CHECK(ring.try_push(2));
    CHECK(!ring.try_push(3));
    CHECK_EQ(ring.dropped(), std::uint64_t{1});

    std::uint32_t value = 0;
    CHECK(ring.try_pop(value));
    CHECK_EQ(value, std::uint32_t{1});
    CHECK(ring.try_push(3));

    CHECK(ring.try_pop(value));
    CHECK_EQ(value, std::uint32_t{2});
    CHECK(ring.try_pop(value));
    CHECK_EQ(value, std::uint32_t{3});
    CHECK(!ring.try_pop(value));
}

TEST_CASE(drop_oldest_ring_evicts_the_oldest_item) {
    DropOldestRing<std::uint32_t, 3> ring;
    CHECK(ring.push(1));
    CHECK(ring.push(2));
    CHECK(ring.push(3));
    CHECK(ring.full());

    CHECK(ring.push(4));
    CHECK_EQ(ring.size(), std::size_t{3});
    CHECK_EQ(ring.dropped(), std::uint64_t{1});

    std::uint32_t value = 0;
    CHECK(ring.pop(value));
    CHECK_EQ(value, std::uint32_t{2});
    CHECK(ring.pop(value));
    CHECK_EQ(value, std::uint32_t{3});
    CHECK(ring.pop(value));
    CHECK_EQ(value, std::uint32_t{4});
    CHECK(!ring.pop(value));
}

TEST_CASE(spsc_ring_stress_two_threads_one_million_items) {
    SpscRing<std::uint32_t, 64> ring;

    std::atomic<bool> producer_done{false};
    std::atomic<std::uint32_t> mismatches{0};
    std::uint32_t consumed = 0;

    std::thread producer([&ring, &producer_done] {
        for (std::uint32_t value = 0; value < kStressItems; ++value) {
            while (!ring.try_push(value)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&ring, &producer_done, &mismatches, &consumed] {
        std::uint32_t expected = 0;
        std::uint32_t value = 0;
        while (expected < kStressItems) {
            if (ring.try_pop(value)) {
                if (value != expected) {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                }
                ++expected;
            } else if (producer_done.load(std::memory_order_acquire)) {
                // The producer is finished; if the ring is still empty the
                // item count is wrong and the checks below will report it.
                if (ring.empty()) {
                    break;
                }
            } else {
                std::this_thread::yield();
            }
        }
        consumed = expected;
    });

    producer.join();
    consumer.join();

    CHECK_EQ(consumed, kStressItems);
    CHECK_EQ(mismatches.load(std::memory_order_relaxed), std::uint32_t{0});
    // The producer retries when the ring is full, so transient rejected pushes
    // are expected; the counter is a policy signal, not a data-loss signal.
    CHECK(ring.dropped() < kStressItems);
    CHECK(ring.empty());
}
