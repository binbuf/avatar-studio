#include "avatar/capture/still_image_source.h"

#include "avatar/capture/video_frame.h"
#include "avatar/core/clock.h"
#include "still_image_fixture.h"
#include "test_registry.h"

#include <cstdint>

namespace {

using avatar::capture::PixelFormat;
using avatar::capture::StillImageConfig;
using avatar::capture::StillImageSource;
using avatar::core::FrameClock;
using avatar::core::ManualTimeSource;

struct StillFixture {
    ManualTimeSource time{0};
    FrameClock clock{time};
    StillImageSource source;

    explicit StillFixture(std::uint32_t width, std::uint32_t height, PixelFormat format)
        : source(clock, make_config(width, height, format)) {}

    static StillImageConfig make_config(std::uint32_t width, std::uint32_t height,
                                        PixelFormat format) {
        StillImageConfig config;
        config.width = width;
        config.height = height;
        config.format = format;
        config.fps = 30;
        config.rgb = avatar::test::make_still_rgb(width, height);
        return config;
    }
};

} // namespace

TEST_CASE(still_image_source_publishes_the_fixture_as_bgra_frames) {
    StillFixture fixture(96, 48, PixelFormat::bgra8);
    CHECK(fixture.source.start().ok());
    CHECK(fixture.source.info().name == "still_image");
    CHECK(fixture.source.info().width == 96);
    CHECK(fixture.source.info().height == 48);
    CHECK(fixture.source.produce());

    auto lease = fixture.source.ring().try_acquire_latest();
    CHECK(lease.has_value());
    if (!lease) {
        return;
    }
    const auto& frame = lease->frame();
    CHECK(frame.width == 96);
    CHECK(frame.height == 48);
    CHECK(frame.format == PixelFormat::bgra8);

    // BGRA byte order: the fixture's left third is red.
    const std::uint8_t* left = frame.plane(0) + frame.stride(0) + 4;
    CHECK(left[0] < 20);   // B
    CHECK(left[1] < 20);   // G
    CHECK(left[2] > 200);  // R
    CHECK(left[3] == 255); // A
    // The middle third is green.
    const std::uint8_t* middle = frame.plane(0) + frame.stride(0) + 48u * 4u;
    CHECK(middle[0] < 20);
    CHECK(middle[1] > 200);
    CHECK(middle[2] < 20);
}

TEST_CASE(still_image_source_repeats_at_the_configured_rate) {
    StillFixture fixture(64, 32, PixelFormat::bgra8);
    CHECK(fixture.source.start().ok());
    CHECK(fixture.source.produce());
    CHECK_EQ(fixture.source.frames_published(), 1u);
    // The same clock value is inside the 30 fps interval: no second frame.
    CHECK(!fixture.source.produce());
    CHECK_EQ(fixture.source.frames_published(), 1u);
    fixture.time.advance(33'333'334);
    CHECK(fixture.source.produce());
    CHECK_EQ(fixture.source.frames_published(), 2u);
}

TEST_CASE(still_image_source_rejects_mismatched_rgb_bytes) {
    StillImageConfig config;
    config.width = 32;
    config.height = 16;
    config.format = PixelFormat::bgra8;
    config.rgb.assign(10, 0);
    ManualTimeSource time{0};
    FrameClock clock{time};
    StillImageSource source(clock, config);
    const auto status = source.start();
    CHECK(!status.ok());
}

TEST_CASE(still_image_source_supports_nv12) {
    StillFixture fixture(64, 32, PixelFormat::nv12);
    CHECK(fixture.source.start().ok());
    CHECK(fixture.source.produce());
    auto lease = fixture.source.ring().try_acquire_latest();
    CHECK(lease.has_value());
    if (!lease) {
        return;
    }
    CHECK(lease->frame().format == PixelFormat::nv12);
    CHECK(lease->frame().plane(1) != nullptr);
    CHECK(lease->frame().width == 64);
    // The bright top row produces a high luma value.
    CHECK(lease->frame().plane(0)[0] > 180);
}
