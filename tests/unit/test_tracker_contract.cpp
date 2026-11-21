#include "test_registry.h"

#include "avatar/tracker/tracker_c_api.h"
#include "avatar/tracking/itracking_backend.h"
#include "avatar/tracking/scripted_tracker.h"
#include "avatar/tracking/tracking_result.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

using avatar::capture::PixelFormat;
using avatar::capture::VideoFrame;
using avatar::tracking::ITrackingBackend;
using avatar::tracking::ScriptedScenario;
using avatar::tracking::ScriptedTracker;
using avatar::tracking::ScriptedTrackerConfig;
using avatar::tracking::TrackerConfig;
using avatar::tracking::TrackerStats;

constexpr avatar::core::TimestampNs kInterval = 16'666'667ull;

VideoFrame make_frame(avatar::core::TimestampNs timestamp, std::uint32_t sequence = 1,
                      std::uint32_t width = 640, std::uint32_t height = 480) {
    VideoFrame frame;
    frame.timestamp_ns = timestamp;
    frame.arrival_ns = timestamp;
    frame.sequence = sequence;
    frame.width = width;
    frame.height = height;
    frame.format = PixelFormat::bgra8;
    return frame;
}

avatar_tracker_config make_config(std::uint32_t scenario) {
    avatar_tracker_config config = AVATAR_TRACKER_CONFIG_INIT;
    config.scenario = scenario;
    config.frame_count = 60;
    return config;
}

avatar_tracker_frame make_c_frame(std::uint64_t timestamp, std::uint32_t sequence = 1) {
    avatar_tracker_frame frame = {};
    frame.timestamp_ns = timestamp;
    frame.arrival_ns = timestamp;
    frame.sequence = sequence;
    frame.width = 640;
    frame.height = 480;
    frame.format = AVATAR_TRACKER_PIXEL_FORMAT_BGRA8;
    frame.plane_count = 0;
    return frame;
}

} // namespace

TEST_CASE(tracker_c_abi_layout_matches_documented_table) {
    CHECK(AVATAR_TRACKER_ABI_VERSION == 1u);
    CHECK(sizeof(avatar_tracker_head_pose) == 28);
    CHECK(sizeof(avatar_tracker_landmark) == 12);
    CHECK(sizeof(avatar_tracker_config) == 48);
    CHECK(sizeof(avatar_tracker_result) == 5992);
    CHECK(offsetof(avatar_tracker_result, source_timestamp_ns) == 0);
    CHECK(offsetof(avatar_tracker_result, confidence) == 12);
    CHECK(offsetof(avatar_tracker_result, pose) == 16);
    CHECK(offsetof(avatar_tracker_result, landmark_count) == 44);
    CHECK(offsetof(avatar_tracker_result, landmarks) == 48);
    CHECK(offsetof(avatar_tracker_result, blendshapes) == 5784);
}

TEST_CASE(tracker_c_abi_rejects_version_and_config_mismatches) {
    avatar_tracker_config config = make_config(AVATAR_TRACKER_SCENARIO_VALID_TRACKING);
    avatar_tracker_error error = AVATAR_TRACKER_OK;

    config.abi_version = 2;
    CHECK(avatar_tracker_create(&config, &error) == nullptr);
    CHECK(error == AVATAR_TRACKER_ERROR_ABI_MISMATCH);

    config = make_config(AVATAR_TRACKER_SCENARIO_VALID_TRACKING);
    config.frame_count = 0;
    CHECK(avatar_tracker_create(&config, &error) == nullptr);
    CHECK(error == AVATAR_TRACKER_ERROR_INVALID_CONFIG);

    config = make_config(99);
    CHECK(avatar_tracker_create(&config, &error) == nullptr);
    CHECK(error == AVATAR_TRACKER_ERROR_INVALID_CONFIG);

    config = make_config(AVATAR_TRACKER_SCENARIO_VALID_TRACKING);
    config.base_confidence = 2.0f;
    CHECK(avatar_tracker_create(&config, &error) == nullptr);
    CHECK(error == AVATAR_TRACKER_ERROR_INVALID_CONFIG);

    config = make_config(AVATAR_TRACKER_SCENARIO_VALID_TRACKING);
    config.input_width = 640;
    config.input_height = 0;
    CHECK(avatar_tracker_create(&config, &error) == nullptr);
    CHECK(error == AVATAR_TRACKER_ERROR_INVALID_CONFIG);

    config = make_config(AVATAR_TRACKER_SCENARIO_VALID_TRACKING);
    config.reserved = 1;
    CHECK(avatar_tracker_create(&config, &error) == nullptr);
    CHECK(error == AVATAR_TRACKER_ERROR_INVALID_CONFIG);

    config = make_config(AVATAR_TRACKER_SCENARIO_VALID_TRACKING);
    config.simulate_fault = 99;
    CHECK(avatar_tracker_create(&config, &error) == nullptr);
    CHECK(error == AVATAR_TRACKER_ERROR_INVALID_CONFIG);

    // A null config uses the documented defaults.
    avatar_tracker_handle* handle = avatar_tracker_create(nullptr, &error);
    CHECK(handle != nullptr);
    CHECK(error == AVATAR_TRACKER_OK);
    avatar_tracker_destroy(handle);
}

TEST_CASE(tracker_c_abi_returns_only_the_newest_result) {
    avatar_tracker_error error = AVATAR_TRACKER_OK;
    avatar_tracker_handle* handle = avatar_tracker_create(nullptr, &error);
    CHECK(handle != nullptr);

    avatar_tracker_result result = {};
    CHECK(avatar_tracker_poll_result(handle, &result) == AVATAR_TRACKER_NO_RESULT);
    CHECK(result.source_timestamp_ns == 0);

    avatar_tracker_frame first = make_c_frame(100);
    CHECK(avatar_tracker_push_frame(handle, &first) == AVATAR_TRACKER_OK);
    CHECK(avatar_tracker_poll_result(handle, &result) == AVATAR_TRACKER_OK);
    CHECK(result.source_timestamp_ns == 100);
    CHECK(result.valid == 1);
    CHECK(result.features_valid == 1);
    CHECK(result.confidence > 0.9f);
    CHECK(result.landmark_count == ScriptedTracker::kScriptedLandmarkCount);

    // The result was consumed; a second poll without a push returns nothing.
    CHECK(avatar_tracker_poll_result(handle, &result) == AVATAR_TRACKER_NO_RESULT);

    // Two pushes before a poll: only the newest is returned.
    avatar_tracker_frame second = make_c_frame(200, 2);
    avatar_tracker_frame third = make_c_frame(300, 3);
    CHECK(avatar_tracker_push_frame(handle, &second) == AVATAR_TRACKER_OK);
    CHECK(avatar_tracker_push_frame(handle, &third) == AVATAR_TRACKER_OK);
    CHECK(avatar_tracker_poll_result(handle, &result) == AVATAR_TRACKER_OK);
    CHECK(result.source_timestamp_ns == 300);
    CHECK(avatar_tracker_poll_result(handle, &result) == AVATAR_TRACKER_NO_RESULT);

    avatar_tracker_destroy(handle);
}

TEST_CASE(tracker_c_abi_preserves_duplicate_and_non_monotonic_timestamps) {
    avatar_tracker_error error = AVATAR_TRACKER_OK;
    avatar_tracker_handle* handle = avatar_tracker_create(nullptr, &error);
    CHECK(handle != nullptr);
    avatar_tracker_result result = {};

    avatar_tracker_frame frame = make_c_frame(500);
    CHECK(avatar_tracker_push_frame(handle, &frame) == AVATAR_TRACKER_OK);
    CHECK(avatar_tracker_poll_result(handle, &result) == AVATAR_TRACKER_OK);
    CHECK(result.source_timestamp_ns == 500);

    // A camera reset can reuse a timestamp. The contract preserves it rather
    // than inventing a different one; the MediaPipe adapter owns the +1 ms
    // workaround inside the backend.
    CHECK(avatar_tracker_push_frame(handle, &frame) == AVATAR_TRACKER_OK);
    CHECK(avatar_tracker_poll_result(handle, &result) == AVATAR_TRACKER_OK);
    CHECK(result.source_timestamp_ns == 500);

    frame.timestamp_ns = 400;
    CHECK(avatar_tracker_push_frame(handle, &frame) == AVATAR_TRACKER_OK);
    CHECK(avatar_tracker_poll_result(handle, &result) == AVATAR_TRACKER_OK);
    CHECK(result.source_timestamp_ns == 400);

    avatar_tracker_destroy(handle);
}

TEST_CASE(tracker_c_abi_distinguishes_invalid_from_features_invalid) {
    avatar_tracker_error error = AVATAR_TRACKER_OK;
    avatar_tracker_result result = {};

    avatar_tracker_config dropouts = make_config(AVATAR_TRACKER_SCENARIO_DROPOUTS);
    avatar_tracker_handle* handle = avatar_tracker_create(&dropouts, &error);
    CHECK(handle != nullptr);
    std::uint32_t invalid_count = 0;
    for (std::uint32_t index = 0; index < 60; ++index) {
        avatar_tracker_frame frame = make_c_frame(index * kInterval, index + 1);
        CHECK(avatar_tracker_push_frame(handle, &frame) == AVATAR_TRACKER_OK);
        CHECK(avatar_tracker_poll_result(handle, &result) == AVATAR_TRACKER_OK);
        if (result.valid == 0) {
            ++invalid_count;
            CHECK(result.features_valid == 0);
        }
    }
    CHECK(invalid_count > 0);
    avatar_tracker_destroy(handle);

    avatar_tracker_config missing_mouth = make_config(AVATAR_TRACKER_SCENARIO_MISSING_MOUTH);
    handle = avatar_tracker_create(&missing_mouth, &error);
    CHECK(handle != nullptr);
    std::uint32_t features_invalid = 0;
    for (std::uint32_t index = 0; index < 60; ++index) {
        avatar_tracker_frame frame = make_c_frame(index * kInterval, index + 1);
        CHECK(avatar_tracker_push_frame(handle, &frame) == AVATAR_TRACKER_OK);
        CHECK(avatar_tracker_poll_result(handle, &result) == AVATAR_TRACKER_OK);
        CHECK(result.valid == 1);
        if (result.features_valid == 0) {
            ++features_invalid;
            CHECK(result.landmarks[ScriptedTracker::mouth_corner_r].x == 0.0f);
            CHECK(result.landmarks[ScriptedTracker::mouth_lower_center].y == 0.0f);
        }
    }
    CHECK(features_invalid > 0);
    avatar_tracker_destroy(handle);
}

TEST_CASE(tracker_c_abi_destroy_and_null_arguments_are_safe) {
    avatar_tracker_destroy(nullptr);

    avatar_tracker_error error = AVATAR_TRACKER_OK;
    avatar_tracker_config config = make_config(AVATAR_TRACKER_SCENARIO_VALID_TRACKING);
    CHECK(avatar_tracker_configure(nullptr, &config) == AVATAR_TRACKER_ERROR_INVALID_ARGUMENT);
    CHECK(avatar_tracker_push_frame(nullptr, nullptr) == AVATAR_TRACKER_ERROR_INVALID_ARGUMENT);
    CHECK(avatar_tracker_poll_result(nullptr, nullptr) == AVATAR_TRACKER_ERROR_INVALID_ARGUMENT);

    avatar_tracker_handle* handle = avatar_tracker_create(&config, &error);
    CHECK(handle != nullptr);
    CHECK(avatar_tracker_push_frame(handle, nullptr) == AVATAR_TRACKER_ERROR_INVALID_ARGUMENT);
    CHECK(avatar_tracker_poll_result(handle, nullptr) == AVATAR_TRACKER_ERROR_INVALID_ARGUMENT);
    CHECK(avatar_tracker_configure(handle, nullptr) == AVATAR_TRACKER_ERROR_INVALID_ARGUMENT);

    avatar_tracker_destroy(handle);
    avatar_tracker_destroy(nullptr);
}

TEST_CASE(tracker_c_abi_contains_backend_exceptions) {
    avatar_tracker_error error = AVATAR_TRACKER_OK;
    avatar_tracker_result result = {};

    avatar_tracker_config push_fault = make_config(AVATAR_TRACKER_SCENARIO_VALID_TRACKING);
    push_fault.simulate_fault = AVATAR_TRACKER_FAULT_PUSH;
    avatar_tracker_handle* handle = avatar_tracker_create(&push_fault, &error);
    CHECK(handle != nullptr);
    avatar_tracker_frame frame = make_c_frame(1000);
    CHECK(avatar_tracker_push_frame(handle, &frame) == AVATAR_TRACKER_ERROR_INTERNAL);
    avatar_tracker_destroy(handle);

    avatar_tracker_config configure_fault = make_config(AVATAR_TRACKER_SCENARIO_VALID_TRACKING);
    configure_fault.simulate_fault = AVATAR_TRACKER_FAULT_CONFIGURE;
    handle = avatar_tracker_create(&configure_fault, &error);
    CHECK(handle != nullptr);
    CHECK(avatar_tracker_configure(handle, &configure_fault) == AVATAR_TRACKER_ERROR_INTERNAL);
    avatar_tracker_destroy(handle);

    avatar_tracker_config poll_fault = make_config(AVATAR_TRACKER_SCENARIO_VALID_TRACKING);
    poll_fault.simulate_fault = AVATAR_TRACKER_FAULT_POLL;
    handle = avatar_tracker_create(&poll_fault, &error);
    CHECK(handle != nullptr);
    CHECK(avatar_tracker_push_frame(handle, &frame) == AVATAR_TRACKER_OK);
    CHECK(avatar_tracker_poll_result(handle, &result) == AVATAR_TRACKER_ERROR_INTERNAL);
    CHECK(result.source_timestamp_ns == 0);
    avatar_tracker_destroy(handle);
}

TEST_CASE(tracker_c_abi_rejects_invalid_frames) {
    avatar_tracker_error error = AVATAR_TRACKER_OK;
    avatar_tracker_handle* handle = avatar_tracker_create(nullptr, &error);
    CHECK(handle != nullptr);

    avatar_tracker_frame frame = make_c_frame(100);
    frame.plane_count = 4;
    CHECK(avatar_tracker_push_frame(handle, &frame) == AVATAR_TRACKER_ERROR_INVALID_ARGUMENT);

    frame = make_c_frame(100);
    frame.plane_count = 1;
    frame.plane0 = nullptr;
    CHECK(avatar_tracker_push_frame(handle, &frame) == AVATAR_TRACKER_ERROR_INVALID_ARGUMENT);

    frame = make_c_frame(100);
    frame.format = 99;
    CHECK(avatar_tracker_push_frame(handle, &frame) == AVATAR_TRACKER_ERROR_INVALID_ARGUMENT);

    avatar_tracker_destroy(handle);
}

TEST_CASE(tracker_c_abi_configure_rewinds_and_keeps_contract) {
    avatar_tracker_error error = AVATAR_TRACKER_OK;
    avatar_tracker_config config = make_config(AVATAR_TRACKER_SCENARIO_VALID_TRACKING);
    avatar_tracker_handle* handle = avatar_tracker_create(&config, &error);
    CHECK(handle != nullptr);

    avatar_tracker_frame frame = make_c_frame(100);
    CHECK(avatar_tracker_push_frame(handle, &frame) == AVATAR_TRACKER_OK);
    avatar_tracker_result result = {};
    CHECK(avatar_tracker_poll_result(handle, &result) == AVATAR_TRACKER_OK);

    avatar_tracker_config reconfigured = make_config(AVATAR_TRACKER_SCENARIO_DROPOUTS);
    CHECK(avatar_tracker_configure(handle, &reconfigured) == AVATAR_TRACKER_OK);
    // The unconsumed result was discarded; the next frame starts the new script.
    CHECK(avatar_tracker_poll_result(handle, &result) == AVATAR_TRACKER_NO_RESULT);
    frame = make_c_frame(777, 2);
    CHECK(avatar_tracker_push_frame(handle, &frame) == AVATAR_TRACKER_OK);
    CHECK(avatar_tracker_poll_result(handle, &result) == AVATAR_TRACKER_OK);
    CHECK(result.source_timestamp_ns == 777);
    CHECK(result.valid == 1);

    avatar_tracker_destroy(handle);
}

TEST_CASE(scripted_tracker_implements_the_cpp_interface) {
    ScriptedTracker tracker(ScriptedTrackerConfig{ScriptedScenario::valid_tracking, 10});
    ITrackingBackend& backend = tracker;

    TrackerConfig config;
    config.input_width = 1280;
    config.input_height = 720;
    CHECK(backend.initialize(config));
    CHECK(backend.stats().input_width == 1280);
    CHECK(backend.stats().input_height == 720);

    backend.submit(make_frame(1000, 1, 1280, 720));
    const auto result = backend.latest();
    CHECK(result.has_value());
    CHECK(result->source_timestamp_ns == 1000);
    CHECK(result->valid);
    CHECK(result->features_valid);

    TrackerStats stats = backend.stats();
    CHECK(stats.submitted == 1);
    CHECK(stats.produced == 1);
    CHECK(stats.dropped == 0);
    CHECK(stats.input_width == 1280);
    CHECK(stats.input_height == 720);

    backend.shutdown();
    CHECK(!backend.latest().has_value());
    backend.submit(make_frame(2000, 2));
    stats = backend.stats();
    CHECK(stats.dropped_shutdown == 1);
    CHECK(stats.last_drop_reason == avatar::tracking::DropReason::shutdown);

    CHECK(backend.initialize(config));
    backend.submit(make_frame(3000, 3));
    const auto resumed = backend.latest();
    CHECK(resumed.has_value());
    CHECK(resumed->source_timestamp_ns == 3000);
}

TEST_CASE(scripted_tracker_latest_is_only_the_newest_result) {
    ScriptedTracker tracker(ScriptedTrackerConfig{ScriptedScenario::valid_tracking, 10});
    tracker.submit(make_frame(100, 1));
    tracker.submit(make_frame(200, 2));
    const auto result = tracker.latest();
    CHECK(result.has_value());
    CHECK(result->source_timestamp_ns == 200);
}

TEST_CASE(scripted_tracker_stats_record_drops_confidence_and_resolution) {
    ScriptedTracker tracker(ScriptedTrackerConfig{ScriptedScenario::valid_tracking, 5});
    for (std::uint32_t index = 0; index < 7; ++index) {
        tracker.submit(make_frame(index * kInterval, index + 1, 1280, 720));
    }

    const TrackerStats stats = tracker.stats();
    CHECK(stats.submitted == 7);
    CHECK(stats.produced == 5);
    CHECK(stats.dropped == 2);
    CHECK(stats.dropped_script_exhausted == 2);
    CHECK(stats.last_drop_reason == avatar::tracking::DropReason::script_exhausted);
    CHECK(stats.confidence.samples == 5);
    std::uint64_t confidence_bins = 0;
    for (std::size_t index = 0; index < avatar::tracking::ConfidenceDistribution::kBucketCount;
         ++index) {
        confidence_bins += stats.confidence.bucket(index);
    }
    CHECK(confidence_bins == 5);
    CHECK(std::fabs(stats.confidence.mean() - 0.92) < 1e-4);
    CHECK(stats.inference_ns.empty());
    CHECK(stats.inference_ns.p50_ns() == 0);
    CHECK(stats.inference_ns.p95_ns() == 0);
    CHECK(stats.inference_p50_ns() == 0);
    CHECK(stats.inference_p95_ns() == 0);
    CHECK(stats.input_width == 1280);
    CHECK(stats.input_height == 720);
}

TEST_CASE(tracker_stats_records_inference_percentiles) {
    TrackerStats stats;
    for (std::uint64_t milliseconds = 1; milliseconds <= 100; ++milliseconds) {
        stats.record_inference(milliseconds * 1'000'000);
    }
    CHECK(stats.inference_ns.samples() == 100);
    CHECK(stats.inference_p50_ns() >= 49'900'000);
    CHECK(stats.inference_p50_ns() <= 50'100'000);
    CHECK(stats.inference_p95_ns() >= 94'900'000);
    CHECK(stats.inference_p95_ns() <= 95'100'000);
}

TEST_CASE(tracker_submit_never_waits_for_inference) {
    constexpr std::uint32_t kFrames = 10'000;
    ScriptedTracker tracker(ScriptedTrackerConfig{ScriptedScenario::valid_tracking, kFrames});

    const auto start = std::chrono::steady_clock::now();
    for (std::uint32_t index = 0; index < kFrames; ++index) {
        tracker.submit(make_frame(static_cast<avatar::core::TimestampNs>(index) * 1000, index));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    CHECK(elapsed < 5000);
    CHECK(tracker.stats().produced == kFrames);
}
