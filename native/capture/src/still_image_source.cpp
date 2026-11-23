#include "avatar/capture/still_image_source.h"

#include <array>
#include <cstddef>
#include <utility>

#include "rgb_canvas.h"

namespace avatar::capture {

StillImageSource::StillImageSource(core::FrameClock& clock, StillImageConfig config)
    : clock_(clock), config_(std::move(config)),
      ring_(config_.width, config_.height, config_.format) {}

core::Status StillImageSource::start() {
    if (config_.width == 0 || config_.height == 0) {
        return core::Status::error(core::StatusCode::invalid_argument,
                                   "still image has no dimensions");
    }
    const std::size_t expected =
        static_cast<std::size_t>(config_.width) * static_cast<std::size_t>(config_.height) * 3u;
    if (config_.rgb.size() != expected) {
        return core::Status::error(core::StatusCode::invalid_argument,
                                   "still image RGB size does not match its dimensions");
    }
    std::array<PlaneLayout, kMaxFramePlanes> layout{};
    if (!compute_plane_layout(config_.format, config_.width, config_.height, layout)) {
        return core::Status::error(core::StatusCode::unsupported,
                                   "still image format or dimensions are not supported");
    }
    ring_.reset();
    started_ = true;
    last_ns_ = 0;
    published_ = 0;
    return core::Status::success();
}

void StillImageSource::stop() {
    started_ = false;
}

FrameRing& StillImageSource::ring() noexcept {
    return ring_;
}

bool StillImageSource::produce() {
    if (!started_) {
        return false;
    }
    const TimestampNs now = clock_.now_ns();
    const TimestampNs interval = config_.fps > 0 ? 1'000'000'000ull / config_.fps : 16'666'667ull;
    if (published_ > 0 && now - last_ns_ < interval) {
        return false;
    }
    auto write = ring_.begin_write();
    if (!write) {
        return false;
    }
    detail::RgbCanvas canvas(config_.width, config_.height);
    canvas.set_rgb(config_.rgb.data(), config_.rgb.size());
    if (!canvas.write_to(write->frame())) {
        return false;
    }
    if (!ring_.publish(*write, now, now, false)) {
        return false;
    }
    last_ns_ = now;
    ++published_;
    return true;
}

SourceInfo StillImageSource::info() const noexcept {
    return SourceInfo{"still_image",
                      config_.width,
                      config_.height,
                      config_.format,
                      pixel_format_has_alpha(config_.format),
                      false,
                      false};
}

} // namespace avatar::capture
