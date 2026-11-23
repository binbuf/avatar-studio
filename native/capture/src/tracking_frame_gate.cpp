#include "avatar/capture/tracking_frame_gate.h"

namespace avatar::capture {

TrackingFrameGate::TrackingFrameGate(TimestampNs max_age_ns) noexcept : max_age_ns_(max_age_ns) {}

bool TrackingFrameGate::offer(std::uint32_t slot, const CameraTextureSlotInfo& info,
                              TimestampNs now_ns) noexcept {
    if (!info.valid || info.timestamp_ns > now_ns || now_ns - info.timestamp_ns > max_age_ns_) {
        ++dropped_stale_;
        return false;
    }
    if (in_flight_) {
        ++dropped_busy_;
        return false;
    }
    if (submitted_ > 0 && info.timestamp_ns <= last_submitted_ns_) {
        ++dropped_duplicate_;
        return false;
    }
    in_flight_ = true;
    ticket_ = Ticket{slot, info.sequence, info.timestamp_ns};
    last_submitted_ns_ = info.timestamp_ns;
    ++submitted_;
    return true;
}

void TrackingFrameGate::complete() noexcept {
    in_flight_ = false;
}

std::optional<TrackingFrameGate::Ticket> TrackingFrameGate::in_flight_ticket() const noexcept {
    return in_flight_ ? std::optional<Ticket>(ticket_) : std::nullopt;
}

void TrackingFrameGate::reset() noexcept {
    in_flight_ = false;
    ticket_ = Ticket{};
    last_submitted_ns_ = 0;
    submitted_ = 0;
    dropped_busy_ = 0;
    dropped_stale_ = 0;
    dropped_duplicate_ = 0;
}

} // namespace avatar::capture
