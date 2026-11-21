#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "avatar/capture/video_frame.h"
#include "avatar/tracking/itracking_backend.h"
#include "avatar/tracking/tracking_result.h"

namespace avatar::tracking {

/// Canned TrackingResult sequences used by component and rig tests.
enum class ScriptedScenario {
    valid_tracking = 0,
    dropouts,
    jitter,
    outliers,
    missing_mouth,
    left_right_swap,
};

[[nodiscard]] const char* scripted_scenario_name(ScriptedScenario scenario) noexcept;

struct ScriptedTrackerConfig {
    ScriptedScenario scenario = ScriptedScenario::valid_tracking;
    std::uint32_t frame_count = 60;
    std::uint32_t seed = 0x5EED1234u;
    float base_confidence = 0.92f;
    TimestampNs frame_interval_ns = 16'666'667ull;
};

/// Deterministic replacement for a real tracker backend.
///
/// `submit` consumes one frame and publishes the next scripted result with the
/// frame's own timestamp. No inference, no clock, no unseeded randomness: the
/// jitter and outlier scenarios use a small seeded LCG, so two runs produce
/// byte-identical sequences.
///
/// It implements both the app-side `ITrackingBackend` interface and the C ABI
/// in `avatar/tracker/tracker_c_api.h`, which is what lets the rig and the
/// renderer be built before inference exists.
///
/// Threading: `submit`, `latest`, `stats` and `configure` serialize internally,
/// so a capture thread and a render thread can share one instance. The script
/// inspection accessors (`script`, `config`) are for tests and must not race a
/// `configure` call.
///
/// The first `kScriptedLandmarkCount` landmarks have a documented order so a
/// test can address them without the runtime LandmarkMap (which T12 owns).
class ScriptedTracker final : public ITrackingBackend {
public:
    enum LandmarkIndex : std::size_t {
        eye_outer_r = 0,
        eye_inner_r,
        iris_center_r,
        eye_outer_l,
        eye_inner_l,
        iris_center_l,
        nose_bridge,
        nose_tip,
        nose_left,
        nose_right,
        mouth_corner_r,
        mouth_corner_l,
        mouth_upper_center,
        mouth_lower_center,
        chin,
        jaw_left,
        jaw_right,
        temple_l,
        temple_r,
        cheek_l,
        cheek_r,
        marker_right_cheek,
        kScriptedLandmarkCount,
    };

    explicit ScriptedTracker(ScriptedTrackerConfig config = {});

    /// Rewinds to the first scripted result and clears stats. A scripted
    /// tracker needs no initialization, but `initialize` is part of the
    /// interface and also rewinds.
    bool initialize(const TrackerConfig& config) override;

    /// Rewinds to the first scripted result and clears stats.
    void reset();

    /// Replaces the scripted configuration, rebuilds the sequence and rewinds.
    void configure(ScriptedTrackerConfig config);

    /// Consumes one frame. The result's `source_timestamp_ns` is the frame's
    /// timestamp. Frames beyond the scripted length are counted as dropped and
    /// leave `latest()` unchanged.
    void submit(const capture::VideoFrame& frame) override;

    [[nodiscard]] std::optional<TrackingResult> latest() override;
    [[nodiscard]] std::optional<TrackingResult> latest() const;

    [[nodiscard]] TrackerStats stats() const override;

    /// Stops accepting frames. A later `initialize` resumes the tracker.
    void shutdown() override;

    [[nodiscard]] const std::vector<TrackingResult>& script() const noexcept { return script_; }
    [[nodiscard]] const ScriptedTrackerConfig& config() const noexcept { return config_; }
    [[nodiscard]] std::uint32_t cursor() const;

private:
    void build_script();
    void apply_scenario(std::uint32_t index, TrackingResult& result);
    void reset_locked();

    ScriptedTrackerConfig config_;
    std::vector<TrackingResult> script_;
    std::optional<TrackingResult> latest_{};
    TrackerStats stats_{};
    std::uint32_t cursor_ = 0;
    std::uint32_t random_state_ = 0;
    bool initialized_ = true;
    bool shutdown_ = false;
    mutable std::mutex mutex_;
};

} // namespace avatar::tracking
