#include "avatar/live/live_pipeline.h"

namespace avatar::live {

namespace {

FaceState map_result(bool valid, bool features_valid, float confidence, bool solve_failed,
                     float degraded_confidence) noexcept {
    if (!valid) {
        return FaceState::not_visible;
    }
    if (!features_valid) {
        return FaceState::features_hidden;
    }
    if (solve_failed || confidence < degraded_confidence) {
        return FaceState::degraded;
    }
    return FaceState::tracked;
}

} // namespace

LivePipeline::LivePipeline(capture::ICameraSource& source, tracking::ITrackingBackend& tracker,
                           rig::FaceRig& rig, Config config)
    : source_(source), tracker_(tracker), rig_(rig), config_(config) {}

void LivePipeline::reset() {
    in_flight_ = false;
    have_result_ = false;
    last_valid_ = false;
    last_features_valid_ = false;
    last_confidence_ = 0.0F;
    last_result_ns_ = 0;
    last_output_timestamp_ = 0;
    camera_ = CameraState::ok;
    output_ = {};
    paired_timestamp_ = 0;
    paired_arrival_ = 0;
    app_latency_.reset();
    counters_ = {};
}

PipelineTick LivePipeline::tick(TimestampNs render_time_ns, CameraState camera) {
    PipelineTick tick;
    camera_ = camera;

    (void)source_.produce();

    if (!in_flight_) {
        const auto lease = source_.ring().try_acquire_latest();
        if (lease) {
            tracker_.submit(lease->frame());
            in_flight_ = true;
            ++counters_.frames_submitted;
            tick.submitted = true;
        }
    }

    if (in_flight_) {
        const auto result = tracker_.latest();
        if (result.has_value()) {
            in_flight_ = false;
            ++counters_.results_consumed;
            tick.consumed = true;
            have_result_ = true;
            last_valid_ = result->valid;
            last_features_valid_ = result->features_valid;
            last_confidence_ = result->confidence;
            last_result_ns_ =
                result->source_timestamp_ns != 0 ? result->source_timestamp_ns : render_time_ns;
            rig_.submit(*result);
        }
    }

    output_ = rig_.sample(render_time_ns);
    tick.have_output = have_result_ && output_.source_timestamp_ns != 0;
    if (tick.have_output && output_.source_timestamp_ns != last_output_timestamp_) {
        last_output_timestamp_ = output_.source_timestamp_ns;
        tick.output_fresh = true;
        ++counters_.output_frames;
    }
    tick.output_timestamp_ns = output_.source_timestamp_ns;
    tick.output = output_;
    return tick;
}

void LivePipeline::note_paired_frame(TimestampNs source_timestamp_ns, TimestampNs arrival_ns) {
    paired_timestamp_ = source_timestamp_ns;
    paired_arrival_ = arrival_ns;
}

void LivePipeline::record_transport_submit(TimestampNs now_ns) {
    if (paired_arrival_ == 0 || now_ns < paired_arrival_) {
        return;
    }
    app_latency_.add(now_ns - paired_arrival_);
    ++counters_.submit_samples;
}

FaceState LivePipeline::face_state(TimestampNs now_ns) const noexcept {
    if (!tracker_available_) {
        return FaceState::unavailable;
    }
    if (!have_result_) {
        return FaceState::not_visible;
    }
    if (last_result_ns_ != 0 && now_ns > last_result_ns_ &&
        now_ns - last_result_ns_ > config_.tracker_timeout_ns) {
        return FaceState::not_visible;
    }
    return map_result(last_valid_, last_features_valid_, last_confidence_, output_.solve_failed,
                      config_.degraded_confidence);
}

float LivePipeline::confidence() const noexcept {
    if (!tracker_available_ || !have_result_ || !last_valid_ || !last_features_valid_) {
        return 0.0F;
    }
    return last_confidence_;
}

} // namespace avatar::live
