#pragma once

#include "avatar/core/clock.h"

#include <cstdint>

namespace avatar::core {

/// How the render cadence is anchored to the display.
enum class PacingMode {
    /// The display refresh is an integer multiple of the target rate, so vsync
    /// can present every target frame without beating.
    vsync,
    /// The display refresh is not a clean multiple (or is unknown), so the
    /// runtime paces itself against an absolute monotonic deadline grid.
    deadline,
};

/// Nominal 60 Hz frame interval in nanoseconds.
inline constexpr TimestampNs kDefaultFrameIntervalNs = 16'666'667;

/// Pacing decision for one main-loop iteration.
///
/// `begin_frame()` is called exactly once per iteration. It advances an
/// absolute deadline grid and reports how many whole intervals were skipped.
/// The caller renders the current frame once; it must never render extra
/// frames to catch up.
struct PacingDecision {
    /// The deadline this frame belongs to. For an early call this is the
    /// deadline the caller should wait for before rendering.
    TimestampNs deadline_ns = 0;
    /// How late the frame is against its deadline. Zero when on time or early.
    TimestampNs lateness_ns = 0;
    /// How long the caller should sleep to reach `deadline_ns` (0 when late).
    TimestampNs sleep_ns = 0;
    /// Whole frame slots skipped since the previous call. These are dropped,
    /// not replayed.
    std::uint32_t dropped = 0;
};

/// Fixed-rate frame pacer shared by the runtime and its tests.
///
/// The pacer owns no clock and performs no sleeping: it is a pure accounting
/// state machine over an injected monotonic timeline. The runtime reads the
/// clock, calls `begin_frame()`, and sleeps only the remainder it reports.
/// This keeps the render path allocation-free and makes the drop-not-double
/// rule testable with `ManualTimeSource`.
class FramePacer {
public:
    explicit FramePacer(TimestampNs interval_ns = kDefaultFrameIntervalNs) noexcept;

    /// Chooses vsync when `display_refresh_hz` is a positive integer multiple
    /// of `target_hz` within 1% (60/120/240, and 59.94), otherwise deadline.
    /// Unknown or non-positive refresh rates use the deadline scheduler.
    [[nodiscard]] static PacingMode select_mode(double display_refresh_hz,
                                                double target_hz) noexcept;

    /// Interval for a target rate, clamped to at least 1 ns.
    [[nodiscard]] static TimestampNs interval_for_fps(double target_hz) noexcept;

    void set_interval(TimestampNs interval_ns) noexcept;
    [[nodiscard]] TimestampNs interval_ns() const noexcept { return interval_ns_; }
    [[nodiscard]] double interval_ms() const noexcept;

    /// Starts a new deadline grid at `now_ns`.
    void reset(TimestampNs now_ns) noexcept;
    [[nodiscard]] bool started() const noexcept { return started_; }

    /// Advances the grid to the frame that contains `now_ns`.
    ///
    /// If `now_ns` is one or more whole intervals past the next deadline, the
    /// skipped slots are counted in `dropped` and the grid advances by whole
    /// intervals only. The next deadline is therefore always in the future
    /// (or now), never in the past, so a caller cannot be tempted to render a
    /// burst to catch up.
    [[nodiscard]] PacingDecision begin_frame(TimestampNs now_ns) noexcept;

    [[nodiscard]] TimestampNs next_deadline_ns() const noexcept { return next_deadline_ns_; }
    [[nodiscard]] std::uint64_t dropped_total() const noexcept { return dropped_total_; }
    [[nodiscard]] std::uint64_t late_frames() const noexcept { return late_frames_; }
    [[nodiscard]] std::uint64_t early_frames() const noexcept { return early_frames_; }

private:
    TimestampNs interval_ns_;
    TimestampNs next_deadline_ns_ = 0;
    std::uint64_t dropped_total_ = 0;
    std::uint64_t late_frames_ = 0;
    std::uint64_t early_frames_ = 0;
    bool started_ = false;
};

} // namespace avatar::core
