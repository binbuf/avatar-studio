#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "avatar/capture/video_frame.h"

namespace avatar::capture {

class FrameRing;

/// A pinned view of one published frame.
///
/// Move-only and RAII: the pin is released on destruction. While the lease is
/// alive the producer will not overwrite the slot, so `frame().planes` stay
/// valid. Do not retain the VideoFrame value beyond the lease.
class FrameLease {
public:
    FrameLease() noexcept = default;
    FrameLease(const FrameLease&) = delete;
    FrameLease& operator=(const FrameLease&) = delete;
    FrameLease(FrameLease&& other) noexcept;
    FrameLease& operator=(FrameLease&& other) noexcept;
    ~FrameLease();

    [[nodiscard]] bool valid() const noexcept { return ring_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] const VideoFrame& frame() const noexcept { return frame_; }
    [[nodiscard]] const VideoFrame* operator->() const noexcept { return &frame_; }

    /// Releases the pin early. Safe to call more than once.
    void release() noexcept;

private:
    friend class FrameRing;
    FrameRing* ring_ = nullptr;
    std::uint32_t slot_ = 0;
    VideoFrame frame_{};
};

/// A reserved slot the producer may write into.
///
/// Move-only. `publish` (on the ring) commits the write; letting the writer go
/// out of scope without publishing abandons the reservation. The slot is not
/// visible to consumers until publish succeeds.
class FrameWrite {
public:
    FrameWrite() noexcept = default;
    FrameWrite(const FrameWrite&) = delete;
    FrameWrite& operator=(const FrameWrite&) = delete;
    FrameWrite(FrameWrite&& other) noexcept;
    FrameWrite& operator=(FrameWrite&& other) noexcept;
    ~FrameWrite();

    [[nodiscard]] bool active() const noexcept { return ring_ != nullptr && active_; }
    [[nodiscard]] std::uint32_t slot() const noexcept { return slot_; }

    [[nodiscard]] VideoFrame& frame() noexcept { return *frame_; }
    [[nodiscard]] const VideoFrame& frame() const noexcept { return *frame_; }

    [[nodiscard]] std::uint8_t* plane(std::size_t index) noexcept;
    [[nodiscard]] std::uint32_t stride(std::size_t index) const noexcept;

private:
    friend class FrameRing;
    FrameRing* ring_ = nullptr;
    VideoFrame* frame_ = nullptr;
    std::uint32_t slot_ = 0;
    bool active_ = false;
    bool was_published_ = false;
};

/// Fixed-capacity, ring-owned frame slots with explicit pin/release.
///
/// This is the ownership model from camera-capture.md: the producer owns the
/// slots, writes into one that no consumer is reading, and publishes. A
/// consumer pins the newest published slot for the duration of its read. If
/// every reusable slot is pinned, the producer drops the frame and increments
/// `dropped()`; it never blocks and never copies pixels.
///
/// Threading: one producer thread and any number of consumer threads that only
/// call `try_acquire_latest()`. `begin_write`/`publish` must not run
/// concurrently with `reset`. Capacity 1 can publish one frame and then has no
/// reusable slot (the only slot is the latest); a live producer needs capacity
/// 2 or more. The default is 4.
class FrameRing {
public:
    FrameRing(std::uint32_t width, std::uint32_t height, PixelFormat format,
              std::uint32_t capacity = 4);
    ~FrameRing();

    FrameRing(const FrameRing&) = delete;
    FrameRing& operator=(const FrameRing&) = delete;
    FrameRing(FrameRing&&) = delete;
    FrameRing& operator=(FrameRing&&) = delete;

    /// Reserves the best reusable slot: an unpublished slot first, otherwise
    /// the oldest published slot that is neither the latest nor pinned.
    /// Returns nullopt and increments `dropped()` when every slot is pinned.
    [[nodiscard]] std::optional<FrameWrite> begin_write() noexcept;

    /// Commits a reserved write. `write` is consumed (deactivated) on success.
    [[nodiscard]] bool publish(FrameWrite& write, TimestampNs timestamp_ns, TimestampNs arrival_ns,
                               bool mirrored = false) noexcept;

    /// Drops a reservation without publishing. The slot keeps its previous
    /// content (or stays unpublished) and becomes reusable again.
    void abandon(FrameWrite& write) noexcept;

    /// Pins the newest published slot. Returns nullopt when nothing has been
    /// published yet, or when the producer is mid-write on the only candidate.
    [[nodiscard]] std::optional<FrameLease> try_acquire_latest() noexcept;

    /// Drops all published frames and resets the sequence counter. Only call
    /// while no lease is alive and no producer is writing.
    void reset() noexcept;

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] PixelFormat format() const noexcept { return format_; }
    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t published_count() const noexcept {
        return published_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool has_published() const noexcept {
        return latest_index_.load(std::memory_order_acquire) < capacity_;
    }
    [[nodiscard]] TimestampNs latest_timestamp() const noexcept;
    [[nodiscard]] std::uint32_t latest_sequence() const noexcept;

private:
    friend class FrameLease;
    friend class FrameWrite;

    enum class SlotState : std::uint8_t {
        unpublished = 0,
        writing,
        published,
    };

    struct Slot {
        std::vector<std::uint8_t> storage;
        VideoFrame frame{};
        std::atomic<std::uint32_t> pins{0};
        std::atomic<SlotState> state{SlotState::unpublished};
        std::atomic<TimestampNs> published_ns{0};
        std::atomic<std::uint32_t> published_sequence{0};
    };

    [[nodiscard]] bool is_reusable(std::uint32_t index) const noexcept;
    void release_slot(std::uint32_t index) noexcept;

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    PixelFormat format_ = PixelFormat::bgra8;
    std::uint32_t capacity_ = 0;
    std::array<PlaneLayout, kMaxFramePlanes> plane_layouts_{};
    std::vector<Slot> slots_;
    static constexpr std::uint32_t kNoSlot = 0xFFFF'FFFFu;
    std::atomic<std::uint32_t> latest_index_{kNoSlot};
    std::atomic<std::uint32_t> sequence_counter_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> published_count_{0};
};

} // namespace avatar::capture
