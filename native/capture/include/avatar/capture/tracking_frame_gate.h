#pragma once

#include <cstdint>
#include <optional>

#include "avatar/capture/camera_texture_ring.h"
#include "avatar/core/clock.h"

namespace avatar::capture {

/// Latest-only admission control for tracker inference.
///
/// The camera ring may publish faster than inference runs. This gate never
/// queues: while an inference is in flight every offered frame is dropped and
/// counted, and the next frame offered after completion is the newest one. A
/// frame older than `max_age_ns` (or newer than the render clock) is stale and
/// is not submitted at all. The real tracker has its own busy drop; this is the
/// app-side policy that keeps frames from accumulating before inference.
class TrackingFrameGate {
public:
    struct Ticket {
        std::uint32_t slot = 0;
        std::uint32_t sequence = 0;
        TimestampNs timestamp_ns = 0;

        [[nodiscard]] bool operator==(const Ticket&) const = default;
    };

    explicit TrackingFrameGate(TimestampNs max_age_ns = 150'000'000) noexcept;

    /// Offers a selected camera slot. Returns true when the caller should
    /// submit it to inference and marks an inference in flight.
    [[nodiscard]] bool offer(std::uint32_t slot, const CameraTextureSlotInfo& info,
                             TimestampNs now_ns) noexcept;

    /// Marks the in-flight inference complete. Safe when nothing is in flight.
    void complete() noexcept;

    [[nodiscard]] bool in_flight() const noexcept { return in_flight_; }
    [[nodiscard]] std::optional<Ticket> in_flight_ticket() const noexcept;
    [[nodiscard]] std::uint64_t submitted() const noexcept { return submitted_; }
    [[nodiscard]] std::uint64_t dropped_busy() const noexcept { return dropped_busy_; }
    [[nodiscard]] std::uint64_t dropped_stale() const noexcept { return dropped_stale_; }
    [[nodiscard]] std::uint64_t dropped_duplicate() const noexcept { return dropped_duplicate_; }
    [[nodiscard]] TimestampNs last_submitted_ns() const noexcept { return last_submitted_ns_; }
    void reset() noexcept;

private:
    TimestampNs max_age_ns_ = 150'000'000;
    bool in_flight_ = false;
    Ticket ticket_{};
    TimestampNs last_submitted_ns_ = 0;
    std::uint64_t submitted_ = 0;
    std::uint64_t dropped_busy_ = 0;
    std::uint64_t dropped_stale_ = 0;
    std::uint64_t dropped_duplicate_ = 0;
};

} // namespace avatar::capture
