#pragma once

#include <cstdint>

#include "avatar/capture/icamera_source.h"

namespace avatar::capture {

struct FrameRect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] bool contains(std::uint32_t px, std::uint32_t py) const noexcept {
        return px >= x && px < x + width && py >= y && py < y + height;
    }
    [[nodiscard]] std::uint32_t centre_x() const noexcept { return x + width / 2u; }
    [[nodiscard]] std::uint32_t centre_y() const noexcept { return y + height / 2u; }
};

struct TestPatternConfig {
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    PixelFormat format = PixelFormat::bgra8;
    bool colour_bars = true;
    bool moving_marker = true;
    bool timestamp_burnin = true;
    bool alpha_checkerboard = true;
};

/// Deterministic capture test card: colour bars, a moving marker, a decimal
/// timestamp burn-in and an alpha checkerboard, driven only by an injected
/// FrameClock.
///
/// The marker position is a pure function of elapsed time, so a test can assert
/// the exact pixel without duplicating the renderer: `marker_rect_at(timestamp)`
/// returns the same rectangle the frame used.
class TestPatternSource final : public ICameraSource {
public:
    explicit TestPatternSource(core::FrameClock& clock, TestPatternConfig config = {});

    [[nodiscard]] core::Status start() override;
    void stop() override;
    [[nodiscard]] FrameRing& ring() noexcept override;
    [[nodiscard]] bool produce() override;
    [[nodiscard]] SourceInfo info() const noexcept override;

    [[nodiscard]] FrameRect marker_rect_at(TimestampNs timestamp_ns) const noexcept;
    [[nodiscard]] FrameRect last_marker_rect() const noexcept { return last_marker_; }
    [[nodiscard]] TimestampNs start_timestamp() const noexcept { return start_ns_; }
    [[nodiscard]] TimestampNs last_timestamp() const noexcept { return last_ns_; }

private:
    core::FrameClock& clock_;
    TestPatternConfig config_;
    FrameRing ring_;
    bool started_ = false;
    bool has_last_ = false;
    TimestampNs start_ns_ = 0;
    TimestampNs last_ns_ = 0;
    FrameRect last_marker_{};
};

} // namespace avatar::capture
