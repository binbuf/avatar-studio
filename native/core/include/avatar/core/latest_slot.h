#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "avatar/core/cache_line.h"

#if defined(_MSC_VER)
// C4324: per-slot values and reader counters are deliberately placed on
// separate cache lines.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace avatar::core {

/// Single-writer, multi-reader latest-value slot.
///
/// Losing an intermediate value is correct for poses and tracking results:
/// queuing them only adds latency. `publish` writes into a slot that is neither
/// the currently published one nor held by a reader, then flips the atomic
/// `latest_` index. A reader therefore copies one complete value; it never
/// observes a half-written one.
///
/// Why not a plain double buffer? With two slots a reader that loaded the old
/// index can still be copying when the writer wraps around and overwrites that
/// slot. Three slots plus a per-slot reader count let the writer skip a slot
/// that is in use. If every candidate is held (a pathological slow reader),
/// `publish` drops the new value and increments `dropped()` rather than
/// blocking the producer.
///
/// Memory ordering:
/// - `read` increments `readers[idx]` with acq_rel, re-checks `latest_` with
///   acquire, and reads the value only if the index is still current. The
///   writer's acquire load of `readers` then cannot miss an active reader.
/// - `publish` writes the value, stores `latest_` with release, and increments
///   `sequence_` with release. A reader's acquire load of `latest_` observes
///   the completed value.
/// - `readers` is decremented with release; the writer's acquire load sees the
///   release once the reader is done with the slot.
///
/// The writer is single-threaded. Readers may be any number of threads.
template <typename T, std::size_t SlotCount = 3>
class LatestSlot {
public:
    static_assert(SlotCount >= 2, "a latest-value slot needs at least two slots");
    static_assert(std::is_default_constructible_v<T>,
                  "LatestSlot default-constructs its fixed slots");

    using value_type = T;

    LatestSlot() = default;

    LatestSlot(const LatestSlot&) = delete;
    LatestSlot& operator=(const LatestSlot&) = delete;
    LatestSlot(LatestSlot&&) = delete;
    LatestSlot& operator=(LatestSlot&&) = delete;

    /// Writer side. Returns false only when every reusable slot is held by a
    /// reader; the new value is then dropped (the previous value stays
    /// readable) and `dropped()` is incremented.
    [[nodiscard]] bool publish(const T& value) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        const std::uint32_t current = latest_.load(std::memory_order_acquire);
        for (std::uint32_t index = 0; index < SlotCount; ++index) {
            if (index == current) {
                continue;
            }
            Slot& slot = slots_[index];
            if (slot.readers.load(std::memory_order_acquire) != 0) {
                continue;
            }
            slot.value = value;
            latest_.store(index, std::memory_order_release);
            sequence_.fetch_add(1, std::memory_order_release);
            return true;
        }
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    [[nodiscard]] bool publish(T&& value) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::uint32_t current = latest_.load(std::memory_order_acquire);
        for (std::uint32_t index = 0; index < SlotCount; ++index) {
            if (index == current) {
                continue;
            }
            Slot& slot = slots_[index];
            if (slot.readers.load(std::memory_order_acquire) != 0) {
                continue;
            }
            slot.value = std::move(value);
            latest_.store(index, std::memory_order_release);
            sequence_.fetch_add(1, std::memory_order_release);
            return true;
        }
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    /// Reader side. Copies the newest complete value, or the previous one if a
    /// publish raced the selection. Returns false before the first publish.
    [[nodiscard]] bool read(T& out) const noexcept(std::is_nothrow_copy_assignable_v<T>) {
        // A fast writer can flip latest_ between the load and the reader
        // count increment. Retry a few times; a reader that keeps losing the
        // race returns false rather than spinning indefinitely.
        for (std::uint32_t attempt = 0; attempt < 8; ++attempt) {
            const std::uint32_t index = latest_.load(std::memory_order_acquire);
            if (sequence_.load(std::memory_order_acquire) == 0) {
                return false;
            }
            Slot& slot = slots_[index];
            slot.readers.fetch_add(1, std::memory_order_acq_rel);
            if (latest_.load(std::memory_order_acquire) != index) {
                slot.readers.fetch_sub(1, std::memory_order_release);
                continue;
            }
            out = slot.value;
            slot.readers.fetch_sub(1, std::memory_order_release);
            return true;
        }
        return false;
    }

    /// Number of successful publishes.
    [[nodiscard]] std::uint64_t sequence() const noexcept {
        return sequence_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool has_value() const noexcept {
        return sequence_.load(std::memory_order_acquire) != 0;
    }

    /// Publishes dropped because every candidate slot was reader-held.
    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

    /// Clears the slot. Only valid while no writer or reader is running.
    void reset() noexcept {
        for (Slot& slot : slots_) {
            slot.readers.store(0, std::memory_order_relaxed);
        }
        latest_.store(0, std::memory_order_relaxed);
        sequence_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
    }

private:
    struct Slot {
        alignas(kCacheLineSize) T value{};
        alignas(kCacheLineSize) std::atomic<std::uint32_t> readers{0};
    };

    // Mutable because `read` must increment/decrement the per-slot reader
    // counters even though it does not modify the published value.
    alignas(kCacheLineSize) mutable std::array<Slot, SlotCount> slots_{};
    alignas(kCacheLineSize) std::atomic<std::uint32_t> latest_{0};
    alignas(kCacheLineSize) std::atomic<std::uint64_t> sequence_{0};
    alignas(kCacheLineSize) std::atomic<std::uint64_t> dropped_{0};
};

} // namespace avatar::core

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
