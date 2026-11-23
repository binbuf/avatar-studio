#pragma once

#include <cstdint>
#include <vector>

#include "avatar/capture/icamera_source.h"

namespace avatar::capture {

/// Development-only source: replays one still RGB image as camera frames.
///
/// The image is tightly packed RGB8. The source converts it to the configured
/// output format (BGRA8 or NV12) at the target rate. It is a diagnostic aid for
/// setup/UI work, not a production capture path: it never opens a device and
/// must not be used as M1 gate evidence.
struct StillImageConfig {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PixelFormat format = PixelFormat::bgra8;
    std::vector<std::uint8_t> rgb;
    std::uint32_t fps = 30;
};

class StillImageSource final : public ICameraSource {
public:
    explicit StillImageSource(core::FrameClock& clock, StillImageConfig config = {});

    [[nodiscard]] core::Status start() override;
    void stop() override;
    [[nodiscard]] FrameRing& ring() noexcept override;
    [[nodiscard]] bool produce() override;
    [[nodiscard]] SourceInfo info() const noexcept override;

    [[nodiscard]] std::uint64_t frames_published() const noexcept { return published_; }

private:
    core::FrameClock& clock_;
    StillImageConfig config_;
    FrameRing ring_;
    bool started_ = false;
    TimestampNs last_ns_ = 0;
    std::uint64_t published_ = 0;
};

} // namespace avatar::capture
