#include "avatar/tracking/scripted_tracker.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace avatar::tracking {

namespace {

constexpr float kPi = 3.14159265358979323846f;

struct Quaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

Quaternion multiply(const Quaternion& a, const Quaternion& b) noexcept {
    return Quaternion{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quaternion from_euler_degrees(float yaw, float pitch, float roll) noexcept {
    const float hy = yaw * (kPi / 180.0f) * 0.5f;
    const float hp = pitch * (kPi / 180.0f) * 0.5f;
    const float hr = roll * (kPi / 180.0f) * 0.5f;
    const Quaternion qx{std::sin(hp), 0.0f, 0.0f, std::cos(hp)};
    const Quaternion qy{0.0f, std::sin(hy), 0.0f, std::cos(hy)};
    const Quaternion qz{0.0f, 0.0f, std::sin(hr), std::cos(hr)};
    return multiply(multiply(qz, qy), qx);
}

void normalise(Quaternion& q) noexcept {
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (length > 1e-6f) {
        q.x /= length;
        q.y /= length;
        q.z /= length;
        q.w /= length;
    }
}

std::uint32_t next_random(std::uint32_t& state) noexcept {
    state = state * 1664525u + 1013904223u;
    return state;
}

float random_signed(std::uint32_t& state, float amplitude) noexcept {
    const float unit = static_cast<float>(next_random(state) >> 8) / 16777216.0f;
    return (unit * 2.0f - 1.0f) * amplitude;
}

// The first kScriptedLandmarkCount landmarks, in the order of the
// ScriptedTracker::LandmarkIndex enum. Subject-right points sit at x < 0.5 in
// an unmirrored image, matching the canonical-face convention.
constexpr std::array<std::array<float, 2>, ScriptedTracker::kScriptedLandmarkCount> kTemplate{{
    {0.42f, 0.45f},  // eye_outer_r
    {0.47f, 0.45f},  // eye_inner_r
    {0.445f, 0.45f}, // iris_center_r
    {0.58f, 0.45f},  // eye_outer_l
    {0.53f, 0.45f},  // eye_inner_l
    {0.555f, 0.45f}, // iris_center_l
    {0.50f, 0.52f},  // nose_bridge
    {0.50f, 0.58f},  // nose_tip
    {0.48f, 0.58f},  // nose_left
    {0.52f, 0.58f},  // nose_right
    {0.45f, 0.66f},  // mouth_corner_r
    {0.55f, 0.66f},  // mouth_corner_l
    {0.50f, 0.64f},  // mouth_upper_center
    {0.50f, 0.68f},  // mouth_lower_center
    {0.50f, 0.76f},  // chin
    {0.40f, 0.64f},  // jaw_left
    {0.60f, 0.64f},  // jaw_right
    {0.38f, 0.40f},  // temple_l
    {0.62f, 0.40f},  // temple_r
    {0.42f, 0.60f},  // cheek_l
    {0.58f, 0.60f},  // cheek_r
    {0.41f, 0.60f},  // marker_right_cheek
}};

} // namespace

const char* scripted_scenario_name(ScriptedScenario scenario) noexcept {
    switch (scenario) {
    case ScriptedScenario::valid_tracking:
        return "valid_tracking";
    case ScriptedScenario::dropouts:
        return "dropouts";
    case ScriptedScenario::jitter:
        return "jitter";
    case ScriptedScenario::outliers:
        return "outliers";
    case ScriptedScenario::missing_mouth:
        return "missing_mouth";
    case ScriptedScenario::left_right_swap:
        return "left_right_swap";
    }
    return "unknown";
}

ScriptedTracker::ScriptedTracker(ScriptedTrackerConfig config) : config_(config) {
    if (config_.frame_count == 0) {
        config_.frame_count = 1;
    }
    random_state_ = config_.seed;
    build_script();
}

void ScriptedTracker::reset() {
    cursor_ = 0;
    latest_.reset();
    stats_ = TrackerStats{};
}

void ScriptedTracker::build_script() {
    script_.clear();
    script_.reserve(config_.frame_count);
    random_state_ = config_.seed;

    for (std::uint32_t index = 0; index < config_.frame_count; ++index) {
        TrackingResult result;
        result.source_timestamp_ns = static_cast<TimestampNs>(index) * config_.frame_interval_ns;
        result.valid = true;
        result.features_valid = true;
        result.confidence = config_.base_confidence;

        const float phase = (2.0f * kPi * static_cast<float>(index)) /
                            static_cast<float>(std::max<std::uint32_t>(config_.frame_count, 1));
        const float yaw = 18.0f * std::sin(phase);
        const float pitch = 9.0f * std::sin(2.0f * phase);
        const float roll = 4.0f * std::sin(0.5f * phase);
        const Quaternion rotation = from_euler_degrees(yaw, pitch, roll);
        result.pose.rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
        result.pose.translation = {0.010f * std::sin(phase), 0.005f * std::sin(2.0f * phase), 0.0f};

        result.landmarks.count = kScriptedLandmarkCount;
        for (std::size_t landmark = 0; landmark < kScriptedLandmarkCount; ++landmark) {
            result.landmarks.points[landmark] =
                Landmark{kTemplate[landmark][0], kTemplate[landmark][1], 0.0f};
        }

        apply_scenario(index, result);
        script_.push_back(result);
    }
}

void ScriptedTracker::apply_scenario(std::uint32_t index, TrackingResult& result) {
    switch (config_.scenario) {
    case ScriptedScenario::valid_tracking:
        break;
    case ScriptedScenario::dropouts:
        if ((index % 40u) >= 30u) {
            result.valid = false;
            result.features_valid = false;
            result.confidence = 0.0f;
        }
        break;
    case ScriptedScenario::jitter:
        if (result.valid) {
            result.pose.translation[0] += random_signed(random_state_, 0.002f);
            result.pose.translation[1] += random_signed(random_state_, 0.002f);
            Quaternion rotation{result.pose.rotation[0],
                                result.pose.rotation[1] + random_signed(random_state_, 0.004f),
                                result.pose.rotation[2],
                                result.pose.rotation[3]};
            normalise(rotation);
            result.pose.rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
            for (std::size_t landmark = 0; landmark < result.landmarks.count; ++landmark) {
                result.landmarks.points[landmark].x += random_signed(random_state_, 0.002f);
                result.landmarks.points[landmark].y += random_signed(random_state_, 0.002f);
            }
        }
        break;
    case ScriptedScenario::outliers:
        if (index > 0 && (index % 17u) == 0u) {
            const Quaternion jump = from_euler_degrees(60.0f, 0.0f, 0.0f);
            const Quaternion current{result.pose.rotation[0],
                                     result.pose.rotation[1],
                                     result.pose.rotation[2],
                                     result.pose.rotation[3]};
            const Quaternion combined = multiply(jump, current);
            result.pose.rotation = {combined.x, combined.y, combined.z, combined.w};
            for (std::size_t landmark = 0; landmark < result.landmarks.count; ++landmark) {
                result.landmarks.points[landmark].x += 0.15f;
            }
            result.confidence = 0.25f;
        }
        break;
    case ScriptedScenario::missing_mouth:
        if ((index % 25u) >= 20u) {
            result.features_valid = false;
            for (std::size_t landmark = mouth_corner_r; landmark <= mouth_lower_center; ++landmark) {
                result.landmarks.points[landmark] = Landmark{0.0f, 0.0f, 0.0f};
            }
        }
        break;
    case ScriptedScenario::left_right_swap:
        if ((index % 40u) >= 20u) {
            for (std::size_t landmark = 0; landmark < result.landmarks.count; ++landmark) {
                result.landmarks.points[landmark].x =
                    1.0f - result.landmarks.points[landmark].x;
            }
            result.pose.rotation[1] = -result.pose.rotation[1];
            Quaternion rotation{result.pose.rotation[0],
                                result.pose.rotation[1],
                                result.pose.rotation[2],
                                result.pose.rotation[3]};
            normalise(rotation);
            result.pose.rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
        }
        break;
    }
}

void ScriptedTracker::submit(const capture::VideoFrame& frame) {
    ++stats_.submitted;
    if (cursor_ >= script_.size()) {
        ++stats_.dropped;
        return;
    }
    TrackingResult result = script_[cursor_];
    result.source_timestamp_ns = frame.timestamp_ns;
    latest_ = result;
    ++stats_.produced;
    ++cursor_;
}

} // namespace avatar::tracking
