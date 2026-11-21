#include "test_registry.h"

#include "avatar/capture/test_pattern_source.h"
#include "avatar/capture/video_frame.h"
#include "avatar/core/clock.h"

#include <cstdint>

namespace {

using avatar::capture::frame_content_hash;
using avatar::capture::PixelFormat;
using avatar::capture::TestPatternConfig;
using avatar::capture::TestPatternSource;
using avatar::capture::VideoFrame;
using avatar::core::FrameClock;
using avatar::core::ManualTimeSource;

TestPatternConfig make_config(std::uint32_t width, std::uint32_t height, PixelFormat format) {
    TestPatternConfig config;
    config.width = width;
    config.height = height;
    config.format = format;
    return config;
}

struct PatternFixture {
    ManualTimeSource time;
    FrameClock clock;
    TestPatternSource source;

    PatternFixture(std::uint32_t width, std::uint32_t height, PixelFormat format)
        : time(0), clock(time), source(clock, make_config(width, height, format)) {}
};

bool has_white_pixel(const VideoFrame& frame, std::uint32_t max_x, std::uint32_t max_y) {
    for (std::uint32_t y = 0; y < max_y && y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < max_x && x < frame.width; ++x) {
            if (frame.format == PixelFormat::bgra8) {
                const std::uint8_t* pixel = frame.plane(0) + y * frame.stride(0) + x * 4u;
                if (pixel[0] > 200 && pixel[1] > 200 && pixel[2] > 200) {
                    return true;
                }
            } else {
                const std::uint8_t* pixel = frame.plane(0) + y * frame.stride(0) + x;
                if (*pixel > 200) {
                    return true;
                }
            }
        }
    }
    return false;
}

void check_pattern(std::uint32_t width, std::uint32_t height, PixelFormat format) {
    PatternFixture fixture(width, height, format);
    CHECK(fixture.source.start().ok());
    CHECK(fixture.source.info().synthetic);
    CHECK(fixture.source.info().width == width);
    CHECK(fixture.source.info().height == height);

    CHECK(fixture.source.produce());
    auto first = fixture.source.ring().try_acquire_latest();
    CHECK(first.has_value());
    if (!first) {
        return;
    }
    const VideoFrame& frame = first->frame();
    CHECK(frame.width == width);
    CHECK(frame.height == height);
    CHECK(frame.format == format);
    CHECK(frame.timestamp_ns == 0);
    CHECK(frame.sequence == 1);
    CHECK(frame.plane(0) != nullptr);

    const auto marker = fixture.source.last_marker_rect();
    CHECK(marker.width > 0);
    CHECK(marker.height > 0);
    CHECK(marker.x + marker.width <= width);
    CHECK(marker.y + marker.height <= height);
    CHECK(marker.contains(marker.centre_x(), marker.centre_y()));

    if (format == PixelFormat::bgra8) {
        const std::uint8_t* pixel =
            frame.plane(0) + marker.centre_y() * frame.stride(0) + marker.centre_x() * 4u;
        CHECK(pixel[0] == 0);
        CHECK(pixel[1] == 128);
        CHECK(pixel[2] == 255);
    } else {
        const std::uint8_t* pixel =
            frame.plane(0) + marker.centre_y() * frame.stride(0) + marker.centre_x();
        CHECK(*pixel >= 135);
        CHECK(*pixel <= 147);
    }

    CHECK(has_white_pixel(frame, width / 4u, height / 8u));

    if (format == PixelFormat::bgra8) {
        std::uint64_t transparent = 0;
        std::uint64_t opaque = 0;
        const std::uint32_t bottom = (height * 7u) / 10u;
        for (std::uint32_t y = bottom; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::uint8_t alpha = frame.plane(0)[y * frame.stride(0) + x * 4u + 3u];
                if (alpha == 0) {
                    ++transparent;
                } else if (alpha == 255) {
                    ++opaque;
                }
            }
        }
        CHECK(transparent > 0);
        CHECK(opaque > 0);
    }

    // The same clock instant must not publish a second frame.
    CHECK(!fixture.source.produce());

    fixture.time.advance(16'666'667);
    CHECK(fixture.source.produce());
    auto second = fixture.source.ring().try_acquire_latest();
    CHECK(second.has_value());
    if (second) {
        CHECK(second->frame().timestamp_ns == 16'666'667);
        CHECK(second->frame().sequence == 2);
        CHECK(frame_content_hash(second->frame()) != frame_content_hash(frame));
    }
}

} // namespace

TEST_CASE(test_pattern_bgra_720p) {
    check_pattern(1280, 720, PixelFormat::bgra8);
}

TEST_CASE(test_pattern_nv12_720p) {
    check_pattern(1280, 720, PixelFormat::nv12);
}

TEST_CASE(test_pattern_bgra_1080p) {
    check_pattern(1920, 1080, PixelFormat::bgra8);
}

TEST_CASE(test_pattern_nv12_1080p) {
    check_pattern(1920, 1080, PixelFormat::nv12);
}

TEST_CASE(test_pattern_is_deterministic) {
    PatternFixture first(1280, 720, PixelFormat::bgra8);
    PatternFixture second(1280, 720, PixelFormat::bgra8);
    CHECK(first.source.start().ok());
    CHECK(second.source.start().ok());
    CHECK(first.source.produce());
    CHECK(second.source.produce());

    auto first_lease = first.source.ring().try_acquire_latest();
    auto second_lease = second.source.ring().try_acquire_latest();
    CHECK(first_lease.has_value());
    CHECK(second_lease.has_value());
    if (first_lease && second_lease) {
        CHECK(frame_content_hash(first_lease->frame()) ==
              frame_content_hash(second_lease->frame()));
    }
}

TEST_CASE(test_pattern_marker_moves_with_time) {
    PatternFixture fixture(1280, 720, PixelFormat::bgra8);
    CHECK(fixture.source.start().ok());
    CHECK(fixture.source.produce());
    const auto first_marker = fixture.source.last_marker_rect();

    fixture.time.advance(500'000'000);
    CHECK(fixture.source.produce());
    const auto second_marker = fixture.source.last_marker_rect();
    CHECK(second_marker.x > first_marker.x);

    fixture.time.advance(500'000'000);
    CHECK(fixture.source.produce());
    const auto third_marker = fixture.source.last_marker_rect();
    CHECK(third_marker.x > second_marker.x);
}

TEST_CASE(test_pattern_timestamps_are_strictly_monotonic) {
    PatternFixture fixture(640, 480, PixelFormat::bgra8);
    CHECK(fixture.source.start().ok());
    avatar::core::TimestampNs previous = 0;
    bool first = true;
    for (int frame = 0; frame < 30; ++frame) {
        fixture.time.advance(16'666'667);
        CHECK(fixture.source.produce());
        auto lease = fixture.source.ring().try_acquire_latest();
        CHECK(lease.has_value());
        if (!lease) {
            return;
        }
        const avatar::core::TimestampNs timestamp = lease->frame().timestamp_ns;
        if (!first) {
            CHECK(timestamp > previous);
        }
        previous = timestamp;
        first = false;
    }
}
