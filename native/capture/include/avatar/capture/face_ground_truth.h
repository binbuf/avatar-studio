#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "avatar/core/clock.h"

namespace avatar::capture {

using core::TimestampNs;

/// The canonical correspondences a synthetic or replayed source knows exactly.
///
/// Names match the anatomical set in docs/design/tracking.md. `_l` and `_r` are
/// the subject's left and right, never the viewer's. This is ground truth for
/// tests, not the runtime LandmarkMap (which maps names to backend indices).
enum class GroundTruthLandmark : std::uint8_t {
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
    forehead_center,
    marker_right_cheek,
    count,
};

inline constexpr std::size_t kGroundTruthLandmarkCount =
    static_cast<std::size_t>(GroundTruthLandmark::count);

[[nodiscard]] const char* ground_truth_landmark_name(GroundTruthLandmark landmark) noexcept;
[[nodiscard]] bool parse_ground_truth_landmark(std::string_view name,
                                               GroundTruthLandmark& out) noexcept;

/// One canonical point and where it appears in the rendered frame.
///
/// `u,v` are canonical face-plane coordinates (origin between the eye centres,
/// +u toward the subject's right eye, +v toward the forehead, inter-ocular
/// distance = 1.0).
///
/// `ideal_x,ideal_y` are the pinhole projection in normalised image coordinates
/// (origin top-left, +y down) with no lens distortion. `x,y` are the same point
/// after the source's lens model (identity for an undistorted source), i.e. the
/// pixel the rendered marker actually occupies.
struct CanonicalCorrespondence {
    GroundTruthLandmark id = GroundTruthLandmark::count;
    std::string_view name{};
    float u = 0.0f;
    float v = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    float ideal_x = 0.0f;
    float ideal_y = 0.0f;
};

/// Ground truth emitted alongside a synthetic or replayed frame.
///
/// `camera_from_head` is a row-major 4x4 transform in camera space (right-handed,
/// -Z forward). It is the pose a perfect tracker would report for this frame.
struct FaceGroundTruth {
    TimestampNs timestamp_ns = 0;
    std::uint32_t sequence = 0;
    std::array<float, 16> camera_from_head{1.0f, 0.0f, 0.0f, 0.0f,
                                           0.0f, 1.0f, 0.0f, 0.0f,
                                           0.0f, 0.0f, 1.0f, 0.0f,
                                           0.0f, 0.0f, 0.0f, 1.0f};
    std::array<CanonicalCorrespondence, kGroundTruthLandmarkCount> correspondences{};
    std::size_t correspondence_count = 0;
    bool features_valid = false;
    float barrel_k1 = 0.0f;

    [[nodiscard]] const CanonicalCorrespondence* find(GroundTruthLandmark landmark) const noexcept {
        for (std::size_t index = 0; index < correspondence_count; ++index) {
            if (correspondences[index].id == landmark) {
                return &correspondences[index];
            }
        }
        return nullptr;
    }
};

[[nodiscard]] inline float pose_at(const std::array<float, 16>& matrix, int row, int column) noexcept {
    return matrix[static_cast<std::size_t>(row) * 4u + static_cast<std::size_t>(column)];
}

} // namespace avatar::capture
