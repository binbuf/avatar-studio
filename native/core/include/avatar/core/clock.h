#pragma once

#include <atomic>
#include <cstdint>

namespace avatar::core {

/// Nanoseconds on a monotonic timeline. The epoch is arbitrary; only
/// differences are meaningful.
using TimestampNs = std::uint64_t;

/// Injectable time source.
///
/// The core never reads a clock directly except through SteadyTimeSource, so
/// tests can drive time deterministically and no subsystem reads wall-clock
/// time.
class TimeSource {
public:
    virtual ~TimeSource() = default;

    [[nodiscard]] virtual TimestampNs now_ns() const = 0;
};

/// Monotonic system clock (std::chrono::steady_clock). This is the only place
/// in the core that reads the OS clock.
class SteadyTimeSource final : public TimeSource {
public:
    [[nodiscard]] TimestampNs now_ns() const override;
};

/// Deterministic time source for tests and scripted harnesses.
///
/// It is part of the core, not a test-only header, so a component test can
/// inject it without duplicating a fake in every test file.
class ManualTimeSource final : public TimeSource {
public:
    explicit ManualTimeSource(TimestampNs start_ns = 0) noexcept;

    [[nodiscard]] TimestampNs now_ns() const override;

    /// Moves the timeline to an absolute value. It may move backwards; that is
    /// exactly the case FrameClock is required to clamp.
    void set(TimestampNs now_ns) noexcept;

    /// Advances by a non-negative delta.
    void advance(TimestampNs delta_ns) noexcept;

private:
    std::atomic<TimestampNs> now_ns_;
};

/// Monotonic view of an injected TimeSource.
///
/// A source that steps backwards (clock adjustment, a buggy test fake) cannot
/// make the frame timeline go backwards: now_ns() clamps to the highest value
/// observed so far.
class FrameClock {
public:
    explicit FrameClock(const TimeSource& source) noexcept;

    /// Current frame time in nanoseconds, never less than a previous call.
    [[nodiscard]] TimestampNs now_ns() noexcept;

    /// Last value returned by now_ns(), or 0 before the first call.
    [[nodiscard]] TimestampNs last_ns() const noexcept { return last_ns_; }

private:
    const TimeSource* source_;
    TimestampNs last_ns_ = 0;
    bool started_ = false;
};

} // namespace avatar::core
