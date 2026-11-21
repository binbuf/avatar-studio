#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "avatar/core/cache_line.h"

#if defined(_MSC_VER)
// C4324: the class is deliberately padded so the producer and consumer
// indices do not share a cache line. The padding is the point, not an
// accident.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace avatar::core {

/// Fixed-capacity single-producer/single-consumer ring.
///
/// The primitive is deliberately small: it never blocks, never allocates, and
/// never evicts on its own. `try_push` reports a full ring and increments
/// `dropped()`; the caller chooses the policy (drop the new item, retry, or use
/// `DropOldestRing`).
///
/// Threading contract:
/// - exactly one thread calls `try_push` (the producer),
/// - exactly one thread calls `try_pop` (the consumer),
/// - `size()`, `empty()`, `full()` and `dropped()` are safe to call from
///   either thread but are only exact while the ring is quiescent.
///
/// Memory ordering:
/// - the producer reads `tail_` with acquire to observe a slot released by the
///   consumer before reusing it, writes the value, then stores `head_` with
///   release so the consumer's acquire load of `head_` sees the value;
/// - the consumer reads `head_` with acquire, copies the value, then stores
///   `tail_` with release so the producer's acquire load of `tail_` sees that
///   the slot is free.
///
/// The indices are monotonically increasing counters; the slot is
/// `index % Capacity`. They wrap with unsigned arithmetic, which is well
/// defined and keeps the difference correct for any capacity far below
/// `SIZE_MAX`.
template <typename T, std::size_t Capacity>
class SpscRing {
public:
    static_assert(Capacity >= 2, "a ring needs at least two slots");
    static_assert(std::is_default_constructible_v<T>,
                  "SpscRing default-constructs its fixed buffer");

    using value_type = T;

    SpscRing() = default;

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&) = delete;
    SpscRing& operator=(SpscRing&&) = delete;

    /// Producer side. Returns false and increments `dropped()` when full.
    [[nodiscard]] bool try_push(const T& value) noexcept(
        std::is_nothrow_copy_assignable_v<T>) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= Capacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        buffer_[head % Capacity] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    /// Producer side. Returns false and increments `dropped()` when full.
    [[nodiscard]] bool try_push(T&& value) noexcept(
        std::is_nothrow_move_assignable_v<T>) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= Capacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        buffer_[head % Capacity] = std::move(value);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    /// Consumer side. Returns false when empty.
    [[nodiscard]] bool try_pop(T& out) noexcept(
        std::is_nothrow_copy_assignable_v<T>) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            return false;
        }
        out = buffer_[tail % Capacity];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Producer-side eviction for a drop-oldest policy wrapper.
    ///
    /// This is *not* hidden inside `try_push`: the policy is explicit and the
    /// caller owns the decision. It is only valid when the consumer is not
    /// concurrently inside `try_pop`, because the consumer owns `tail_`.
    /// `DropOldestRing` documents the same restriction; a concurrent
    /// drop-oldest queue is deferred to the output-manager task.
    [[nodiscard]] bool try_discard_oldest() noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            return false;
        }
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] bool full() const noexcept { return size() >= Capacity; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

    /// Number of `try_push` calls rejected because the ring was full.
    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

    /// Clears the ring. Only valid while no producer or consumer is running.
    void reset() noexcept {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
    }

private:
    // The producer owns head_ and dropped_; the consumer owns tail_. Keeping
    // them on separate cache lines stops a busy producer from invalidating the
    // consumer's line (and vice versa) on every operation. dropped_ is a
    // diagnostic counter with no data dependency, so relaxed ordering is
    // sufficient; its value is approximate while both threads run.
    alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};
    alignas(kCacheLineSize) std::atomic<std::uint64_t> dropped_{0};
    alignas(kCacheLineSize) std::array<T, Capacity> buffer_{};
};

/// Drop-oldest policy wrapper around `SpscRing`.
///
/// The ring primitive never evicts; this wrapper is where the output-queue
/// policy lives. When the ring is full, `push` discards the oldest item and
/// retries once. `dropped()` counts evicted items, while `ring().dropped()`
/// counts the underlying rejected push attempts.
///
/// The wrapper is a policy layer, not a second synchronisation primitive. It
/// inherits the ring's single-producer/single-consumer contract and, like
/// `SpscRing::try_discard_oldest`, requires the consumer not to be inside
/// `pop` while the producer evicts. The concurrent output queue is T46; that
/// task owns the per-slot state needed to make eviction safe with a live
/// consumer.
template <typename T, std::size_t Capacity>
class DropOldestRing {
public:
    static_assert(Capacity >= 2, "a drop-oldest ring needs at least two slots");

    [[nodiscard]] bool push(const T& value) noexcept(
        std::is_nothrow_copy_assignable_v<T>) {
        if (ring_.try_push(value)) {
            return true;
        }
        if (!ring_.try_discard_oldest()) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        dropped_.fetch_add(1, std::memory_order_relaxed);
        if (!ring_.try_push(value)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool push(T&& value) noexcept(
        std::is_nothrow_move_assignable_v<T>) {
        if (ring_.try_push(std::move(value))) {
            return true;
        }
        if (!ring_.try_discard_oldest()) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        dropped_.fetch_add(1, std::memory_order_relaxed);
        if (!ring_.try_push(std::move(value))) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool pop(T& out) noexcept(
        std::is_nothrow_copy_assignable_v<T>) {
        return ring_.try_pop(out);
    }

    [[nodiscard]] std::size_t size() const noexcept { return ring_.size(); }
    [[nodiscard]] bool empty() const noexcept { return ring_.empty(); }
    [[nodiscard]] bool full() const noexcept { return ring_.full(); }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

    /// Items evicted by the drop-oldest policy.
    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] const SpscRing<T, Capacity>& ring() const noexcept { return ring_; }

    void reset() noexcept {
        ring_.reset();
        dropped_.store(0, std::memory_order_relaxed);
    }

private:
    alignas(kCacheLineSize) SpscRing<T, Capacity> ring_{};
    alignas(kCacheLineSize) std::atomic<std::uint64_t> dropped_{0};
};

} // namespace avatar::core

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
