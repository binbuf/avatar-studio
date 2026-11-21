#pragma once

#include <algorithm>
#include <cmath>

#include "avatar/capture/synthetic_face_source.h"
#include "avatar/rig/face_rig.h"

namespace avatar::test {

// Ground truth comes from the source renderer, not from the solver. The fast
// motion/benchmark path projects the renderer's neutral points through its
// independently evaluated analytic pose.
struct RigFixture {
    core::ManualTimeSource time{1'000'000'000};
    core::FrameClock clock{time};
    capture::SyntheticFaceConfig config;
    capture::SyntheticFaceSource source;
    tracking::LandmarkMap map = tracking::LandmarkMap::mediapipe();
    capture::FaceGroundTruth neutral;
    tracking::ProfileLandmarks profile;

    static capture::SyntheticFaceConfig make_config() {
        capture::SyntheticFaceConfig c;
        c.width = 320;
        c.height = 180;
        c.path = {{0, {0, 0, 0}, {0, 0, 0}}};
        return c;
    }

    RigFixture() : config(make_config()), source(clock, config) {
        (void)source.start();
        (void)source.produce();
        neutral = *source.latest_ground_truth();
        for (std::size_t i = 0; i < neutral.correspondence_count; ++i) {
            const auto& p = neutral.correspondences[i];
            if (tracking::parse_landmark_name(p.name)) {
                profile.landmarks.push_back({std::string(p.name), p.u, p.v});
            }
        }
    }

    static tracking::HeadPose head_pose(const std::array<float, 16>& m) {
        tracking::HeadPose p;
        // Test paths stay inside +/-80 degrees, so the trace branch is stable.
        const float scale = std::sqrt(1 + m[0] + m[5] + m[10]) * 2;
        p.rotation = {(m[9] - m[6]) / scale, (m[2] - m[8]) / scale, (m[4] - m[1]) / scale,
                      scale / 4};
        p.translation = {m[3], m[7], m[11]};
        return p;
    }

    tracking::TrackingResult result(core::TimestampNs timestamp = 1'000'000'000, float yaw = 0,
                                    float pitch = 0, float roll = 0, float tx = 0) const {
        auto c = config;
        c.path = {{0, {tx, 0, 0}, {yaw, pitch, roll}}};
        const auto pose = capture::SyntheticFaceSource::evaluate_pose(c, 0);
        tracking::TrackingResult r;
        r.source_timestamp_ns = timestamp;
        r.valid = r.features_valid = true;
        r.confidence = 1;
        r.pose = head_pose(pose.camera_from_head);
        r.landmarks.count = tracking::LandmarkSet::kCapacity;
        for (std::size_t i = 0; i < neutral.correspondence_count; ++i) {
            const auto& p = neutral.correspondences[i];
            const auto index = map.resolve(p.name);
            if (index) {
                const auto xy = capture::SyntheticFaceSource::project_canonical(pose, p.u, p.v);
                r.landmarks.points[*index] = {xy[0], xy[1], 0};
            }
        }
        return r;
    }
};

inline std::array<double, 2> rig_project(const std::array<float, 9>& h, double u, double v) {
    const double w = h[6] * u + h[7] * v + h[8];
    return {(h[0] * u + h[1] * v + h[2]) / w, (h[3] * u + h[4] * v + h[5]) / w};
}

inline double inverse_pose_error(const rig::FaceRigOutput& out,
                                 const tracking::HeadPose& expected) {
    // Compare trace(R_output * R_expected) with identity. R_output is inverse.
    const auto& q = expected.rotation;
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const std::array<double, 9> r{
        1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
        2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
        2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
    double trace = 0;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            trace += out.head_from_camera[static_cast<std::size_t>(4 * i + j)] *
                     r[static_cast<std::size_t>(3 * j + i)];
        }
    }
    return std::acos(std::clamp((trace - 1) / 2, -1.0, 1.0)) * 180 / 3.141592653589793;
}
} // namespace avatar::test
