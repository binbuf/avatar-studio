#include "avatar/capture/test_pattern_source.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "rgb_canvas.h"

namespace avatar::capture {

namespace {

constexpr TimestampNs kMarkerPeriodNs = 2'000'000'000ull;
constexpr std::uint32_t kBarCount = 8;

// 5x7 bitmapped digits, one byte per row, five low bits used (bit 4 = left).
constexpr std::uint8_t kDigits[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
};

constexpr detail::Rgb8 kBarColours[kBarCount] = {
    {235, 235, 235},
    {235, 235, 16},
    {16, 235, 235},
    {16, 235, 16},
    {235, 16, 235},
    {235, 16, 16},
    {16, 16, 235},
    {16, 16, 16},
};

void draw_text(detail::RgbCanvas& canvas,
               std::uint32_t x,
               std::uint32_t y,
               const std::string& text,
               std::uint32_t scale,
               detail::Rgb8 colour) noexcept {
    const std::uint32_t advance = 6u * scale;
    for (std::size_t character = 0; character < text.size(); ++character) {
        const char value = text[character];
        if (value < '0' || value > '9') {
            continue;
        }
        const std::uint8_t* rows = kDigits[static_cast<std::size_t>(value - '0')];
        const std::uint32_t origin_x = x + static_cast<std::uint32_t>(character) * advance;
        for (std::uint32_t row = 0; row < 7; ++row) {
            for (std::uint32_t column = 0; column < 5; ++column) {
                const std::uint8_t mask = static_cast<std::uint8_t>(0x10u >> column);
                if ((rows[row] & mask) == 0) {
                    continue;
                }
                for (std::uint32_t sy = 0; sy < scale; ++sy) {
                    for (std::uint32_t sx = 0; sx < scale; ++sx) {
                        canvas.set(origin_x + column * scale + sx,
                                   y + row * scale + sy,
                                   colour);
                    }
                }
            }
        }
    }
}

void render_pattern(const TestPatternConfig& config,
                    detail::RgbCanvas& canvas,
                    TimestampNs timestamp_ns,
                    FrameRect marker) noexcept {
    const std::uint32_t width = config.width;
    const std::uint32_t height = config.height;
    const std::uint32_t bars_bottom = config.alpha_checkerboard ? (height * 7u) / 10u : height;

    if (config.colour_bars) {
        for (std::uint32_t bar = 0; bar < kBarCount; ++bar) {
            const std::uint32_t x0 = (width * bar) / kBarCount;
            const std::uint32_t x1 = (width * (bar + 1u)) / kBarCount;
            for (std::uint32_t y = 0; y < bars_bottom; ++y) {
                for (std::uint32_t x = x0; x < x1; ++x) {
                    canvas.set(x, y, kBarColours[bar]);
                }
            }
        }
    } else {
        for (std::uint32_t y = 0; y < bars_bottom; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                canvas.set(x, y, detail::Rgb8{40, 40, 40});
            }
        }
    }

    if (config.alpha_checkerboard) {
        const std::uint32_t checker = std::max<std::uint32_t>(8u, width / 80u);
        for (std::uint32_t y = bars_bottom; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const bool dark = (((x / checker) + (y / checker)) % 2u) == 0;
                canvas.set(x, y, dark ? detail::Rgb8{16, 16, 16} : detail::Rgb8{235, 235, 235});
            }
        }
    }

    if (config.timestamp_burnin) {
        const std::uint32_t scale = std::max<std::uint32_t>(2u, width / 640u);
        const std::string text = std::to_string(timestamp_ns);
        const std::uint32_t text_width =
            static_cast<std::uint32_t>(text.size()) * 6u * scale;
        const std::uint32_t text_height = 7u * scale;
        const std::uint32_t pad = 2u * scale;
        const std::uint32_t box_x = std::max<std::uint32_t>(4u, width / 32u);
        const std::uint32_t box_y = std::max<std::uint32_t>(4u, height / 32u);
        for (std::uint32_t y = 0; y < text_height + 2u * pad; ++y) {
            for (std::uint32_t x = 0; x < text_width + 2u * pad; ++x) {
                canvas.set(box_x + x, box_y + y, detail::Rgb8{0, 0, 0});
            }
        }
        draw_text(canvas, box_x + pad, box_y + pad, text, scale, detail::Rgb8{255, 255, 255});
    }

    if (config.moving_marker) {
        for (std::uint32_t y = marker.y; y < marker.y + marker.height; ++y) {
            for (std::uint32_t x = marker.x; x < marker.x + marker.width; ++x) {
                canvas.set(x, y, detail::Rgb8{255, 128, 0});
            }
        }
    }
}

} // namespace

TestPatternSource::TestPatternSource(core::FrameClock& clock, TestPatternConfig config)
    : clock_(clock),
      config_(config),
      ring_(config.width, config.height, config.format) {}

core::Status TestPatternSource::start() {
    std::array<PlaneLayout, kMaxFramePlanes> layout{};
    if (!compute_plane_layout(config_.format, config_.width, config_.height, layout)) {
        return core::Status::error(core::StatusCode::unsupported,
                                   "test pattern format or dimensions are not supported");
    }
    ring_.reset();
    started_ = true;
    has_last_ = false;
    last_ns_ = 0;
    start_ns_ = clock_.now_ns();
    last_marker_ = marker_rect_at(start_ns_);
    return core::Status::success();
}

void TestPatternSource::stop() {
    started_ = false;
}

FrameRing& TestPatternSource::ring() noexcept {
    return ring_;
}

SourceInfo TestPatternSource::info() const noexcept {
    return SourceInfo{"test_pattern",
                      config_.width,
                      config_.height,
                      config_.format,
                      pixel_format_has_alpha(config_.format),
                      true,
                      false};
}

FrameRect TestPatternSource::marker_rect_at(TimestampNs timestamp_ns) const noexcept {
    const TimestampNs elapsed = timestamp_ns > start_ns_ ? timestamp_ns - start_ns_ : 0;
    const double phase =
        static_cast<double>(elapsed % kMarkerPeriodNs) / static_cast<double>(kMarkerPeriodNs);
    std::uint32_t size = std::max<std::uint32_t>(16u, config_.width / 24u);
    size -= size % 2u;
    const std::uint32_t margin = std::max<std::uint32_t>(8u, config_.width / 32u);
    const std::uint32_t available =
        config_.width > (2u * margin + size) ? config_.width - 2u * margin - size : 0u;
    FrameRect rect;
    rect.width = size;
    rect.height = size;
    rect.x = margin + static_cast<std::uint32_t>(phase * static_cast<double>(available));
    rect.y = config_.height / 2u > size / 2u ? config_.height / 2u - size / 2u : 0u;
    return rect;
}

bool TestPatternSource::produce() {
    if (!started_) {
        return false;
    }
    const TimestampNs now = clock_.now_ns();
    if (has_last_ && now <= last_ns_) {
        return false;
    }
    if (!has_last_) {
        start_ns_ = now;
    }

    const FrameRect marker = marker_rect_at(now);
    auto write = ring_.begin_write();
    if (!write) {
        return false;
    }

    detail::RgbCanvas canvas(config_.width, config_.height);
    render_pattern(config_, canvas, now, marker);
    if (!canvas.write_to(write->frame())) {
        return false;
    }

    if (config_.alpha_checkerboard && pixel_format_has_alpha(config_.format)) {
        VideoFrame& frame = write->frame();
        std::uint8_t* base = const_cast<std::uint8_t*>(frame.planes[0]);
        const std::uint32_t stride = frame.strides[0];
        const std::uint32_t bars_bottom = (config_.height * 7u) / 10u;
        const std::uint32_t checker = std::max<std::uint32_t>(8u, config_.width / 80u);
        for (std::uint32_t y = bars_bottom; y < config_.height; ++y) {
            std::uint8_t* row = base + static_cast<std::size_t>(y) * stride;
            for (std::uint32_t x = 0; x < config_.width; ++x) {
                const bool dark = (((x / checker) + (y / checker)) % 2u) == 0;
                row[static_cast<std::size_t>(x) * 4u + 3u] = dark ? 0u : 255u;
            }
        }
    }

    if (!ring_.publish(*write, now, now, false)) {
        return false;
    }
    last_marker_ = marker;
    last_ns_ = now;
    has_last_ = true;
    return true;
}

} // namespace avatar::capture
