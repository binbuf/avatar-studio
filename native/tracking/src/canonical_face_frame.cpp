#include "avatar/tracking/canonical_face_frame.h"

#include <cmath>

namespace avatar::tracking {

namespace {

[[nodiscard]] std::optional<std::array<float, 2>> average(const Landmark& first,
                                                          const Landmark& second) noexcept {
    return std::array<float, 2>{(first.x + second.x) * 0.5f, (first.y + second.y) * 0.5f};
}

[[nodiscard]] std::optional<std::array<float, 2>> eye_centre(const LandmarkSet& landmarks,
                                                             const LandmarkMap& map,
                                                             LandmarkName iris, LandmarkName outer,
                                                             LandmarkName inner) noexcept {
    if (map.usable(landmarks, iris)) {
        const Landmark point = *map.point(landmarks, iris);
        return std::array<float, 2>{point.x, point.y};
    }
    if (map.usable(landmarks, outer) && map.usable(landmarks, inner)) {
        return average(*map.point(landmarks, outer), *map.point(landmarks, inner));
    }
    return std::nullopt;
}

} // namespace

std::optional<EyeCentres> eye_centres(const LandmarkSet& landmarks,
                                      const LandmarkMap& map) noexcept {
    const std::optional<std::array<float, 2>> right =
        eye_centre(landmarks, map, LandmarkName::iris_center_r, LandmarkName::eye_outer_r,
                   LandmarkName::eye_inner_r);
    const std::optional<std::array<float, 2>> left =
        eye_centre(landmarks, map, LandmarkName::iris_center_l, LandmarkName::eye_outer_l,
                   LandmarkName::eye_inner_l);
    if (!right.has_value() || !left.has_value()) {
        return std::nullopt;
    }

    EyeCentres centres;
    centres.right = *right;
    centres.left = *left;
    centres.from_irises = map.usable(landmarks, LandmarkName::iris_center_r) &&
                          map.usable(landmarks, LandmarkName::iris_center_l);
    return centres;
}

std::optional<CanonicalFaceFrame> CanonicalFaceFrame::from_landmarks(const LandmarkSet& landmarks,
                                                                     const LandmarkMap& map,
                                                                     float aspect) noexcept {
    if (!std::isfinite(aspect) || aspect <= 0.0f) {
        return std::nullopt;
    }
    const std::optional<EyeCentres> centres = eye_centres(landmarks, map);
    if (!centres.has_value()) {
        return std::nullopt;
    }

    const std::array<float, 2> right{centres->right[0] * aspect, centres->right[1]};
    const std::array<float, 2> left{centres->left[0] * aspect, centres->left[1]};
    const float dx = right[0] - left[0];
    const float dy = right[1] - left[1];
    const float interocular = std::hypot(dx, dy);
    if (!(interocular > 1e-6f)) {
        return std::nullopt;
    }

    CanonicalFaceFrame frame;
    frame.aspect = aspect;
    frame.interocular = interocular;
    frame.eye_midpoint = {(right[0] + left[0]) * 0.5f, (right[1] + left[1]) * 0.5f};
    frame.axis_u = {dx / interocular, dy / interocular};
    // Image y points down. Rotating +u by +90 degrees in image coordinates gives
    // a unit vector toward the forehead, not toward the chin.
    frame.axis_v = {-frame.axis_u[1], frame.axis_u[0]};
    return frame;
}

std::array<float, 2> CanonicalFaceFrame::project(float x, float y) const noexcept {
    const float dx = x * aspect - eye_midpoint[0];
    const float dy = y - eye_midpoint[1];
    return {(dx * axis_u[0] + dy * axis_u[1]) / interocular,
            (dx * axis_v[0] + dy * axis_v[1]) / interocular};
}

HandednessReport check_handedness(const LandmarkSet& landmarks, const LandmarkMap& map,
                                  float aspect) noexcept {
    HandednessReport report;
    const std::optional<EyeCentres> centres = eye_centres(landmarks, map);
    if (!centres.has_value()) {
        return report;
    }
    report.eyes_resolved = true;
    report.eye_midpoint_x = (centres->right[0] + centres->left[0]) * 0.5f;

    if (!map.usable(landmarks, LandmarkName::cheek_r) ||
        !map.usable(landmarks, LandmarkName::cheek_l)) {
        return report;
    }
    report.cheeks_resolved = true;
    const Landmark right = *map.point(landmarks, LandmarkName::cheek_r);
    const Landmark left = *map.point(landmarks, LandmarkName::cheek_l);
    report.cheek_right_x = right.x;
    report.cheek_left_x = left.x;

    const std::optional<CanonicalFaceFrame> frame =
        CanonicalFaceFrame::from_landmarks(landmarks, map, aspect);
    if (!frame.has_value()) {
        return report;
    }
    report.cheek_right_u = frame->project(right)[0];
    report.cheek_left_u = frame->project(left)[0];
    return report;
}

} // namespace avatar::tracking
