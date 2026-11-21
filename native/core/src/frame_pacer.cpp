#include "avatar/core/frame_pacer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace avatar::core {

FramePacer::FramePacer(TimestampNs interval_ns) noexcept
    : interval_ns_(interval_ns == 0 ? kDefaultFrameIntervalNs : interval_ns) {}

PacingMode FramePacer::select_mode(double display_refresh_hz, double target_hz) noexcept {
    if (!(display_refresh_hz > 0.0) || !(target_hz > 0.0)) {
        return PacingMode::deadline;
    }
    const double ratio = display_refresh_hz / target_hz;
    const double nearest = std::round(ratio);
    if (nearest < 1.0) {
        return PacingMode::deadline;
    }
    // A 1% tolerance accepts 59.94/60 and 119.88/120 while rejecting 90/60
    // and 144/60, which would beat against a fixed 60 Hz output.
    const double tolerance = 0.01 * nearest;
    return std::abs(ratio - nearest) <= tolerance ? PacingMode::vsync : PacingMode::deadline;
}

TimestampNs FramePacer::interval_for_fps(double target_hz) noexcept {
    if (!(target_hz > 0.0)) {
        return kDefaultFrameIntervalNs;
    }
    const double interval = 1'000'000'000.0 / target_hz;
    if (interval <= 1.0) {
        return 1;
    }
    if (interval >= static_cast<double>(std::numeric_limits<TimestampNs>::max())) {
        return std::numeric_limits<TimestampNs>::max();
    }
    return static_cast<TimestampNs>(std::llround(interval));
}

void FramePacer::set_interval(TimestampNs interval_ns) noexcept {
    interval_ns_ = interval_ns == 0 ? kDefaultFrameIntervalNs : interval_ns;
}

double FramePacer::interval_ms() const noexcept {
    return static_cast<double>(interval_ns_) / 1'000'000.0;
}

void FramePacer::reset(TimestampNs now_ns) noexcept {
    started_ = true;
    next_deadline_ns_ = now_ns;
    dropped_total_ = 0;
    late_frames_ = 0;
    early_frames_ = 0;
}

PacingDecision FramePacer::begin_frame(TimestampNs now_ns) noexcept {
    PacingDecision decision;
    if (!started_) {
        reset(now_ns);
    }

    if (now_ns < next_deadline_ns_) {
        // The main loop arrived early. Report the wait; the caller renders the
        // same frame after sleeping, so no extra frame is produced.
        ++early_frames_;
        decision.deadline_ns = next_deadline_ns_;
        decision.sleep_ns = next_deadline_ns_ - now_ns;
        return decision;
    }

    if (now_ns > next_deadline_ns_) {
        ++late_frames_;
    }

    const TimestampNs delta = now_ns - next_deadline_ns_;
    const std::uint64_t missed = delta / interval_ns_;
    if (missed > 0) {
        dropped_total_ += missed;
        decision.dropped = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(missed, std::numeric_limits<std::uint32_t>::max()));
        next_deadline_ns_ += missed * interval_ns_;
    }

    decision.deadline_ns = next_deadline_ns_;
    decision.lateness_ns = now_ns - next_deadline_ns_;
    next_deadline_ns_ += interval_ns_;
    return decision;
}

} // namespace avatar::core
