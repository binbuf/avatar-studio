#include "avatar/live/live_pipeline.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "face_rig_fixture.h"
#include "test_registry.h"

namespace {

using avatar::capture::VideoFrame;
using avatar::core::TimestampNs;
using avatar::live::FaceState;
using avatar::live::LivePipeline;
using avatar::rig::FaceRig;
using avatar::test::RigFixture;
using avatar::tracking::ITrackingBackend;
using avatar::tracking::TrackerConfig;
using avatar::tracking::TrackerStats;
using avatar::tracking::TrackingResult;

constexpr TimestampNs kFrameNs = 16'666'667;

/// Test tracker: the fixture generates a real face result stamped with the
/// submitted frame's own timestamp, which is the ABI's source-timestamp rule.
class FixtureTracker final : public ITrackingBackend {
public:
    explicit FixtureTracker(RigFixture& fixture) : fixture_(fixture) {}

    bool initialize(const TrackerConfig&) override { return true; }

    void submit(const VideoFrame& frame) override {
        ++submitted_;
        if (drop_all_) {
            return;
        }
        pending_ = fixture_.result(frame.timestamp_ns);
        if (!valid_) {
            pending_->valid = false;
            pending_->features_valid = false;
            pending_->confidence = 0.0F;
        } else if (!features_valid_) {
            pending_->features_valid = false;
        }
        if (low_confidence_) {
            pending_->confidence = 0.2F;
        }
        has_pending_ = true;
    }

    std::optional<TrackingResult> latest() override {
        if (!has_pending_) {
            return std::nullopt;
        }
        has_pending_ = false;
        ++produced_;
        return std::exchange(pending_, std::nullopt);
    }

    TrackerStats stats() const override { return {}; }
    void shutdown() override {}

    void set_valid(bool value) { valid_ = value; }
    void set_features_valid(bool value) { features_valid_ = value; }
    void set_drop_all(bool value) { drop_all_ = value; }
    void set_low_confidence(bool value) { low_confidence_ = value; }
    std::uint64_t submitted() const { return submitted_; }
    std::uint64_t produced() const { return produced_; }

private:
    RigFixture& fixture_;
    std::optional<TrackingResult> pending_{};
    bool has_pending_ = false;
    bool valid_ = true;
    bool features_valid_ = true;
    bool drop_all_ = false;
    bool low_confidence_ = false;
    std::uint64_t submitted_ = 0;
    std::uint64_t produced_ = 0;
};

struct Harness {
    RigFixture rig_fixture;
    FixtureTracker tracker{rig_fixture};
    FaceRig rig{avatar::tracking::LandmarkMap::mediapipe(), 320.0F / 180.0F};
    LivePipeline pipeline{rig_fixture.source, tracker, rig};
    TimestampNs now = 1'000'000'000;

    Harness() {
        rig.configure(rig_fixture.profile);
        pipeline.reset();
    }

    avatar::live::PipelineTick step() {
        now += kFrameNs;
        rig_fixture.time.set(now);
        return pipeline.tick(now, avatar::live::CameraState::ok);
    }
};

} // namespace

TEST_CASE(pipeline_reports_a_tracked_face_and_the_paired_timestamp) {
    Harness h;
    avatar::live::PipelineTick tick;
    for (int i = 0; i < 12; ++i) {
        tick = h.step();
    }
    CHECK(tick.submitted);
    CHECK(tick.consumed);
    CHECK(tick.have_output);
    CHECK(tick.output_timestamp_ns != 0);
    CHECK(tick.output_timestamp_ns == h.pipeline.output().source_timestamp_ns);
    CHECK(h.pipeline.face_state(h.now) == FaceState::tracked);
    CHECK(h.pipeline.confidence() > 0.9F);
    CHECK(h.pipeline.counters().frames_submitted >= 10);
    CHECK(h.pipeline.counters().results_consumed >= 10);
    CHECK(h.tracker.submitted() == h.pipeline.counters().frames_submitted);
}

TEST_CASE(pipeline_reports_a_dropout_as_not_visible) {
    Harness h;
    for (int i = 0; i < 6; ++i) {
        (void)h.step();
    }
    CHECK(h.pipeline.face_state(h.now) == FaceState::tracked);
    h.tracker.set_valid(false);
    for (int i = 0; i < 6; ++i) {
        (void)h.step();
    }
    CHECK(h.pipeline.face_state(h.now) == FaceState::not_visible);
    CHECK_EQ(h.pipeline.confidence(), 0.0F);
}

TEST_CASE(pipeline_reports_hidden_features_distinctly) {
    Harness h;
    h.tracker.set_features_valid(false);
    for (int i = 0; i < 6; ++i) {
        (void)h.step();
    }
    CHECK(h.pipeline.face_state(h.now) == FaceState::features_hidden);
    CHECK_EQ(h.pipeline.confidence(), 0.0F);
}

TEST_CASE(pipeline_reports_a_low_confidence_result_as_degraded) {
    Harness h;
    h.tracker.set_low_confidence(true);
    for (int i = 0; i < 6; ++i) {
        (void)h.step();
    }
    CHECK(h.pipeline.face_state(h.now) == FaceState::degraded);
}

TEST_CASE(pipeline_reports_an_unavailable_tracker) {
    Harness h;
    for (int i = 0; i < 4; ++i) {
        (void)h.step();
    }
    h.pipeline.set_tracker_available(false);
    CHECK(h.pipeline.face_state(h.now) == FaceState::unavailable);
    CHECK_EQ(h.pipeline.confidence(), 0.0F);
}

TEST_CASE(pipeline_times_out_a_stale_result) {
    Harness h;
    for (int i = 0; i < 6; ++i) {
        (void)h.step();
    }
    CHECK(h.pipeline.face_state(h.now) == FaceState::tracked);
    h.tracker.set_drop_all(true);
    for (int i = 0; i < 40; ++i) {
        (void)h.step();
    }
    CHECK(h.pipeline.face_state(h.now) == FaceState::not_visible);
}

TEST_CASE(pipeline_holds_the_setup_decal_while_the_face_state_reports_loss) {
    Harness h;
    avatar::live::PipelineTick tick;
    for (int i = 0; i < 12; ++i) {
        tick = h.step();
    }
    CHECK(tick.output.features_valid);
    CHECK(tick.have_output);
    const auto held_timestamp = tick.output.source_timestamp_ns;
    h.tracker.set_valid(false);
    tick = h.step();
    // The OBS gate reads face_state and blanks immediately; the Setup decal
    // reads output.features_valid and keeps the last FaceRig pose for 150 ms.
    CHECK(h.pipeline.face_state(h.now) == FaceState::not_visible);
    CHECK(tick.output.features_valid);
    CHECK(tick.have_output);
    CHECK_EQ(tick.output.source_timestamp_ns, held_timestamp);
    for (int i = 0; i < 9; ++i) {
        tick = h.step();
    }
    CHECK(!tick.output.features_valid);
    CHECK(h.pipeline.face_state(h.now) == FaceState::not_visible);
}

TEST_CASE(pipeline_records_capture_to_submit_latency) {
    Harness h;
    h.pipeline.note_paired_frame(1'000, 5'000'000);
    h.pipeline.record_transport_submit(25'000'000); // 20 ms
    h.pipeline.record_transport_submit(45'000'000); // 40 ms
    CHECK_EQ(h.pipeline.counters().submit_samples, 2u);
    CHECK_EQ(h.pipeline.app_latency().samples(), 2u);
    const auto p50 = h.pipeline.app_latency().p50_ns();
    CHECK(p50 >= 19'900'000u && p50 <= 20'100'000u);
    CHECK_EQ(h.pipeline.app_latency().max_ns(), 40'000'000u);
    // A submit before the capture callback is ignored, not recorded as negative.
    h.pipeline.note_paired_frame(1'000, 50'000'000);
    h.pipeline.record_transport_submit(49'000'000);
    CHECK_EQ(h.pipeline.app_latency().samples(), 2u);
}
