#pragma once

#include <cstdint>

#include "avatar/core/clock.h"

namespace avatar::core {

/// Identity and time of a frame as it moves through the pipeline.
///
/// timestamp_ns is sampled from FrameClock at the instant the frame becomes
/// available (first byte from the camera, first rendered pixel, ...). Downstream
/// stages pair pose and pixels by this timestamp rather than by arrival order.
struct FrameStamp {
    TimestampNs timestamp_ns = 0;
    std::uint32_t sequence = 0; ///< Per-producer, strictly increasing.
};

[[nodiscard]] constexpr FrameStamp make_frame_stamp(TimestampNs timestamp_ns,
                                                    std::uint32_t sequence) noexcept {
    return FrameStamp{timestamp_ns, sequence};
}

/// Ordering for latest-value slots: newer timestamp wins; equal timestamps fall
/// back to the producer sequence.
[[nodiscard]] constexpr bool is_newer(const FrameStamp& lhs, const FrameStamp& rhs) noexcept {
    if (lhs.timestamp_ns != rhs.timestamp_ns) {
        return lhs.timestamp_ns > rhs.timestamp_ns;
    }
    return lhs.sequence > rhs.sequence;
}

} // namespace avatar::core
