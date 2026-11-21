#include "test_registry.h"

#include "avatar/capture/video_frame.h"
#include "avatar/tracking/scripted_tracker.h"
#include "avatar/tracking/tracking_result.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using avatar::capture::PixelFormat;
using avatar::capture::VideoFrame;
using avatar::tracking::ScriptedScenario;
using avatar::tracking::ScriptedTracker;
using avatar::tracking::ScriptedTrackerConfig;
using avatar::tracking::TrackingResult;

constexpr avatar::core::TimestampNs kInterval = 16'666'667ull;

VideoFrame make_frame(avatar::core::TimestampNs timestamp, std::uint32_t sequence) {
    VideoFrame frame;
    frame.timestamp_ns = timestamp;
    frame.arrival_ns = timestamp;
    frame.sequence = sequence;
    frame.width = 640;
    frame.height = 480;
    frame.format = PixelFormat::bgra8;
    return frame;
}

std::vector<TrackingResult> run_script(ScriptedTracker& tracker, std::uint32_t count) {
    std::vector<TrackingResult> results;
    results.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        tracker.submit(
            make_frame(static_cast<avatar::core::TimestampNs>(index) * kInterval, index + 1));
        const auto latest = tracker.latest();
        if (latest) {
            results.push_back(*latest);
        }
    }
    return results;
}

float marker_x(const TrackingResult& result) {
    return result.landmarks.points[ScriptedTracker::marker_right_cheek].x;
}

} // namespace

TEST_CASE(scripted_tracker_replays_valid_tracking) {
    ScriptedTracker tracker(ScriptedTrackerConfig{ScriptedScenario::valid_tracking, 80});
    const auto results = run_script(tracker, 80);
    CHECK(results.size() == 80);
    CHECK(tracker.stats().submitted == 80);
    CHECK(tracker.stats().produced == 80);
    CHECK(tracker.stats().dropped == 0);

    for (std::size_t index = 0; index < results.size(); ++index) {
        CHECK(results[index].valid);
        CHECK(results[index].features_valid);
        CHECK(results[index].confidence >= 0.9f);
        CHECK(results[index].source_timestamp_ns ==
              static_cast<avatar::core::TimestampNs>(index) * kInterval);
        CHECK(results[index] == tracker.script()[index]);
    }
}

TEST_CASE(scripted_tracker_produces_dropouts) {
    ScriptedTracker tracker(ScriptedTrackerConfig{ScriptedScenario::dropouts, 80});
    const auto results = run_script(tracker, 80);
    std::uint32_t invalid = 0;
    for (const auto& result : results) {
        if (!result.valid) {
            ++invalid;
            CHECK(!result.features_valid);
            CHECK(result.confidence == 0.0f);
        }
    }
    CHECK(invalid >= 10);
    CHECK(results[0].valid);
    CHECK(!results[30].valid);
    CHECK(!results[39].valid);
    CHECK(results[40].valid);
}

TEST_CASE(scripted_tracker_jitter_is_bounded_and_seeded) {
    ScriptedTracker jitter_a(ScriptedTrackerConfig{ScriptedScenario::jitter, 80, 0x1234u});
    ScriptedTracker jitter_b(ScriptedTrackerConfig{ScriptedScenario::jitter, 80, 0x1234u});
    ScriptedTracker clean(ScriptedTrackerConfig{ScriptedScenario::valid_tracking, 80, 0x1234u});
    const auto results_a = run_script(jitter_a, 80);
    const auto results_b = run_script(jitter_b, 80);
    const auto results_clean = run_script(clean, 80);

    CHECK(jitter_a.script() == jitter_b.script());
    CHECK(jitter_a.script() != clean.script());
    CHECK(results_a.size() == 80);

    bool saw_jitter = false;
    for (std::size_t index = 0; index < results_a.size(); ++index) {
        CHECK(results_a[index].valid);
        const float difference = std::fabs(results_a[index].pose.translation[0] -
                                           results_clean[index].pose.translation[0]);
        CHECK(difference <= 0.0021f);
        if (difference > 1e-5f) {
            saw_jitter = true;
        }
    }
    CHECK(saw_jitter);
}

TEST_CASE(scripted_tracker_emits_outliers) {
    ScriptedTracker tracker(ScriptedTrackerConfig{ScriptedScenario::outliers, 80});
    const auto results = run_script(tracker, 80);
    std::uint32_t outliers = 0;
    for (std::size_t index = 0; index < results.size(); ++index) {
        if (results[index].confidence < 0.5f) {
            ++outliers;
            if (index > 0) {
                CHECK(marker_x(results[index]) > marker_x(results[index - 1]) + 0.1f);
            }
        }
    }
    CHECK(outliers >= 4);
}

TEST_CASE(scripted_tracker_reports_missing_mouth) {
    ScriptedTracker tracker(ScriptedTrackerConfig{ScriptedScenario::missing_mouth, 80});
    const auto results = run_script(tracker, 80);
    std::uint32_t missing = 0;
    for (const auto& result : results) {
        CHECK(result.valid);
        if (!result.features_valid) {
            ++missing;
            for (std::size_t landmark = ScriptedTracker::mouth_corner_r;
                 landmark <= ScriptedTracker::mouth_lower_center; ++landmark) {
                CHECK(result.landmarks.points[landmark].x == 0.0f);
                CHECK(result.landmarks.points[landmark].y == 0.0f);
            }
        }
    }
    CHECK(missing >= 5);
    CHECK(results[0].features_valid);
}

TEST_CASE(scripted_tracker_swaps_left_and_right) {
    ScriptedTracker tracker(ScriptedTrackerConfig{ScriptedScenario::left_right_swap, 80});
    const auto results = run_script(tracker, 80);
    std::uint32_t normal = 0;
    std::uint32_t swapped = 0;
    for (const auto& result : results) {
        if (marker_x(result) < 0.5f) {
            ++normal;
        } else {
            ++swapped;
        }
    }
    CHECK(normal > 0);
    CHECK(swapped > 0);
    CHECK(marker_x(results[0]) < 0.5f);
    CHECK(marker_x(results[25]) > 0.5f);
}

TEST_CASE(scripted_tracker_reset_and_overrun) {
    ScriptedTracker tracker(ScriptedTrackerConfig{ScriptedScenario::valid_tracking, 10});
    const auto first = run_script(tracker, 10);
    tracker.reset();
    CHECK(tracker.cursor() == 0);
    const auto again = run_script(tracker, 10);
    CHECK(first.size() == again.size());
    for (std::size_t index = 0; index < first.size(); ++index) {
        CHECK(first[index] == again[index]);
    }

    tracker.submit(make_frame(999, 99));
    CHECK(tracker.stats().dropped == 1);
    CHECK(tracker.stats().produced == 10);
}

TEST_CASE(scripted_scenario_names_are_stable) {
    CHECK(std::string(avatar::tracking::scripted_scenario_name(ScriptedScenario::valid_tracking)) ==
          std::string("valid_tracking"));
    CHECK(std::string(avatar::tracking::scripted_scenario_name(
              ScriptedScenario::left_right_swap)) == std::string("left_right_swap"));
    CHECK(std::string(avatar::tracking::scripted_scenario_name(ScriptedScenario::missing_mouth)) ==
          std::string("missing_mouth"));
}
