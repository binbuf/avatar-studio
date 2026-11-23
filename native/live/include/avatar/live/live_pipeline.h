#pragma once

#include <cstdint>

#include "avatar/capture/icamera_source.h"
#include "avatar/core/clock.h"
#include "avatar/core/latency_histogram.h"
#include "avatar/live/live_session.h"
#include "avatar/rig/face_rig.h"
#include "avatar/tracking/itracking_backend.h"

namespace avatar::live {

using core::TimestampNs;

/// One app frame's pipeline result. The adapter uses `output_timestamp_ns` to
/// select the timestamp-paired camera slot and `output` to drive the decal.
struct PipelineTick {
    /// The tracker accepted a frame this tick.
    bool submitted = false;
    /// The tracker returned a result and it was submitted to the rig.
    bool consumed = false;
    /// A new rig result was applied at this render time.
    bool output_fresh = false;
    /// The rig has a usable (possibly held/predicted) pose.
    bool have_output = false;
    TimestampNs output_timestamp_ns = 0;
    rig::FaceRigOutput output{};
};

/// App-side latency histogram: capture callback to transport submit. 100
/// microsecond buckets over a 204.8 ms window, which covers the documented
/// p95 100 ms budget and still resolves a millisecond.
using AppLatencyHistogram = core::LatencyHistogram<2048, 100'000>;

struct PipelineCounters {
    std::uint64_t camera_frames = 0;
    std::uint64_t frames_submitted = 0;
    std::uint64_t results_consumed = 0;
    std::uint64_t output_frames = 0;
    std::uint64_t submit_samples = 0;
};

/// Framework-free orchestrator for the M1 live path (T26):
/// camera source -> tracker -> FaceRig -> paired timestamp.
///
/// It owns no pixels, no GPU state and no platform code. The caller supplies
/// the source, the tracker and the rig, so the same class runs with the real
/// capture/tracker in the app and with the deterministic synthetic sources in
/// tests. Threading: every call runs on the render thread; the camera source
/// produces into its ring on its own capture thread.
class LivePipeline {
public:
    struct Config {
        /// A tracker result older than this is "no face", not a held pose.
        TimestampNs tracker_timeout_ns = 500'000'000;
        /// Below this confidence a valid result is reported as degraded.
        float degraded_confidence = 0.5F;
    };

    LivePipeline(capture::ICameraSource& source, tracking::ITrackingBackend& tracker,
                 rig::FaceRig& rig, Config config = {});
    ~LivePipeline() = default;

    LivePipeline(const LivePipeline&) = delete;
    LivePipeline& operator=(const LivePipeline&) = delete;

    void reset();

    /// Produces, submits at most one frame to the tracker, polls the newest
    /// result, samples the rig and reports the paired output timestamp.
    /// `camera` is the capture state mapped to the HUD vocabulary by the caller.
    PipelineTick tick(TimestampNs render_time_ns, CameraState camera);

    /// Records the capture-callback timestamp of the frame whose pose produced
    /// the pixels currently being submitted.
    void note_paired_frame(TimestampNs source_timestamp_ns, TimestampNs arrival_ns);

    /// Counts a camera frame that reached the GPU ring even when no pose uses it.
    void note_camera_frame() noexcept { ++counters_.camera_frames; }

    /// Clears the latency histogram and counters but keeps the pipeline state
    /// (in-flight tracker work, last pose). The soak calls this after warm-up.
    void reset_metrics() noexcept {
        app_latency_.reset();
        counters_ = {};
    }

    /// Adds one capture-callback-to-transport-submit sample.
    void record_transport_submit(TimestampNs now_ns);

    void set_tracker_available(bool available) noexcept { tracker_available_ = available; }
    [[nodiscard]] bool tracker_available() const noexcept { return tracker_available_; }

    [[nodiscard]] CameraState camera() const noexcept { return camera_; }
    [[nodiscard]] FaceState face_state(TimestampNs now_ns) const noexcept;
    [[nodiscard]] float confidence() const noexcept;
    [[nodiscard]] const rig::FaceRigOutput& output() const noexcept { return output_; }
    [[nodiscard]] TimestampNs paired_timestamp() const noexcept { return paired_timestamp_; }
    [[nodiscard]] TimestampNs paired_arrival() const noexcept { return paired_arrival_; }

    [[nodiscard]] const AppLatencyHistogram& app_latency() const noexcept { return app_latency_; }
    [[nodiscard]] const PipelineCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] const Config& config() const noexcept { return config_; }

private:
    capture::ICameraSource& source_;
    tracking::ITrackingBackend& tracker_;
    rig::FaceRig& rig_;
    Config config_{};

    bool tracker_available_ = true;
    bool in_flight_ = false;
    bool have_result_ = false;
    bool last_valid_ = false;
    bool last_features_valid_ = false;
    float last_confidence_ = 0.0F;
    TimestampNs last_result_ns_ = 0;
    TimestampNs last_output_timestamp_ = 0;
    CameraState camera_ = CameraState::ok;

    rig::FaceRigOutput output_{};
    TimestampNs paired_timestamp_ = 0;
    TimestampNs paired_arrival_ = 0;

    AppLatencyHistogram app_latency_{};
    PipelineCounters counters_{};
};

} // namespace avatar::live
