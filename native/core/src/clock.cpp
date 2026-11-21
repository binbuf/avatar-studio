#include "avatar/core/clock.h"

#include <chrono>

namespace avatar::core {
namespace {

TimestampNs steady_now_ns() {
    const auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<TimestampNs>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count());
}

} // namespace

TimestampNs SteadyTimeSource::now_ns() const {
    return steady_now_ns();
}

FrameClock::FrameClock(const TimeSource& source) noexcept : source_(&source) {}

TimestampNs FrameClock::now_ns() noexcept {
    const TimestampNs candidate = source_->now_ns();
    if (!started_ || candidate > last_ns_) {
        last_ns_ = candidate;
        started_ = true;
    }
    return last_ns_;
}

} // namespace avatar::core
