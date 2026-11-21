#include "test_registry.h"

#include "avatar/capture/replay_source.h"
#include "avatar/capture/video_frame.h"
#include "avatar/core/clock.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef AVATAR_FIXTURE_DIR
#define AVATAR_FIXTURE_DIR "tests/fixtures"
#endif

namespace {

using avatar::capture::frame_content_hash;
using avatar::capture::PixelFormat;
using avatar::capture::ReplaySource;
using avatar::capture::ReplaySourceConfig;
using avatar::core::FrameClock;
using avatar::core::ManualTimeSource;
using avatar::core::TimestampNs;

std::filesystem::path fixture_path(const std::string& relative) {
    return std::filesystem::path(AVATAR_FIXTURE_DIR) / relative;
}

ReplaySourceConfig make_config(PixelFormat format) {
    ReplaySourceConfig config;
    config.y4m_path = fixture_path("replay/tiny.y4m");
    config.format = format;
    return config;
}

constexpr std::array<TimestampNs, 3> kExpectedTimestamps{0ull, 16'666'667ull, 33'333'334ull};

} // namespace

TEST_CASE(replay_source_replays_the_fixture) {
    ManualTimeSource time(0);
    FrameClock clock(time);
    ReplaySource source(clock, make_config(PixelFormat::bgra8));

    const auto status = source.start();
    CHECK(status.ok());
    if (!status.ok()) {
        return;
    }
    CHECK(source.frame_count() == 3);

    std::vector<std::uint64_t> hashes;
    for (std::size_t index = 0; index < kExpectedTimestamps.size(); ++index) {
        CHECK(source.produce());
        auto lease = source.ring().try_acquire_latest();
        CHECK(lease.has_value());
        if (!lease) {
            return;
        }
        CHECK(lease->frame().width == 64);
        CHECK(lease->frame().height == 48);
        CHECK(lease->frame().format == PixelFormat::bgra8);
        CHECK(lease->frame().timestamp_ns == kExpectedTimestamps[index]);
        CHECK(lease->frame().sequence == index + 1);
        hashes.push_back(frame_content_hash(lease->frame()));

        const auto truth = source.latest_ground_truth();
        CHECK(truth.has_value());
        if (truth) {
            CHECK(truth->timestamp_ns == kExpectedTimestamps[index]);
            CHECK(truth->sequence == index + 1);
            CHECK(truth->features_valid);
            CHECK(truth->correspondence_count > 0);
        }
    }
    CHECK(!source.produce());
    CHECK(hashes.size() == 3);
    CHECK(hashes[0] != hashes[1]);
    CHECK(hashes[1] != hashes[2]);
}

TEST_CASE(replay_source_is_deterministic) {
    ManualTimeSource first_time(0);
    FrameClock first_clock(first_time);
    ReplaySource first(first_clock, make_config(PixelFormat::bgra8));

    ManualTimeSource second_time(0);
    FrameClock second_clock(second_time);
    ReplaySource second(second_clock, make_config(PixelFormat::bgra8));

    CHECK(first.start().ok());
    CHECK(second.start().ok());
    for (int frame = 0; frame < 3; ++frame) {
        CHECK(first.produce());
        CHECK(second.produce());
        auto first_lease = first.ring().try_acquire_latest();
        auto second_lease = second.ring().try_acquire_latest();
        CHECK(first_lease.has_value());
        CHECK(second_lease.has_value());
        if (first_lease && second_lease) {
            CHECK(frame_content_hash(first_lease->frame()) ==
                  frame_content_hash(second_lease->frame()));
        }
    }
}

TEST_CASE(replay_source_supports_nv12) {
    ManualTimeSource time(0);
    FrameClock clock(time);
    ReplaySource source(clock, make_config(PixelFormat::nv12));
    CHECK(source.start().ok());
    CHECK(source.produce());
    auto lease = source.ring().try_acquire_latest();
    CHECK(lease.has_value());
    if (lease) {
        CHECK(lease->frame().format == PixelFormat::nv12);
        CHECK(lease->frame().plane(1) != nullptr);
    }
}

TEST_CASE(replay_source_rejects_a_bad_sidecar) {
    const std::filesystem::path bad_sidecar =
        std::filesystem::temp_directory_path() / "avatar_studio_replay_bad.json";
    {
        std::ofstream output(bad_sidecar, std::ios::binary);
        output << "{\"version\": 2, \"width\": 64, \"height\": 48, \"frames\": []}";
    }

    ManualTimeSource time(0);
    FrameClock clock(time);
    ReplaySourceConfig config = make_config(PixelFormat::bgra8);
    config.sidecar_path = bad_sidecar;
    ReplaySource source(clock, config);
    const auto status = source.start();
    CHECK(!status.ok());
    std::error_code code;
    std::filesystem::remove(bad_sidecar, code);
}
