#pragma once

#include <array>
#include <optional>

#include "avatar/tracking/landmark_map.h"

namespace avatar::tracking {

/// Eye centres in normalised image coordinates. The subject's right eye is on
/// the viewer's left in an unmirrored image.
struct EyeCentres {
    std::array<float, 2> right{};
    std::array<float, 2> left{};
    bool from_irises = false;
};

/// Resolves both eye centres through the map. Iris centres are preferred when
/// the backend provides usable ones; otherwise each side falls back to the
/// midpoint of its eye corners.
[[nodiscard]] std::optional<EyeCentres> eye_centres(const LandmarkSet& landmarks,
                                                    const LandmarkMap& map) noexcept;

/// The canonical face plane: origin at the eye midpoint, +u toward the subject's
/// right eye, +v toward the forehead, inter-ocular distance = 1.0.
///
/// The frame is a 2-D similarity, so it follows head roll instead of assuming a
/// level eye line. `aspect` is the camera image width / height; image x is
/// scaled by it before distances are measured.
struct CanonicalFaceFrame {
    float aspect = 1.0f;
    std::array<float, 2> eye_midpoint{}; // aspect-corrected image units
    std::array<float, 2> axis_u{};       // unit vector toward the subject's right eye
    std::array<float, 2> axis_v{};       // unit vector toward the forehead
    float interocular = 1.0f;            // aspect-corrected units

    [[nodiscard]] static std::optional<CanonicalFaceFrame>
    from_landmarks(const LandmarkSet& landmarks, const LandmarkMap& map, float aspect) noexcept;

    /// Projects a normalised image point into canonical `(u,v)`.
    [[nodiscard]] std::array<float, 2> project(float x, float y) const noexcept;
    [[nodiscard]] std::array<float, 2> project(const Landmark& landmark) const noexcept {
        return project(landmark.x, landmark.y);
    }
};

/// The asymmetric-marker check from docs/design/tracking.md.
///
/// In an unmirrored image the subject's right cheek must appear on the viewer's
/// left and have positive canonical u. A mirrored or swapped table produces a
/// plausible but wrong face, so the build fails on `!pass()`.
struct HandednessReport {
    bool eyes_resolved = false;
    bool cheeks_resolved = false;
    float eye_midpoint_x = 0.5f;
    float cheek_right_x = 0.5f;
    float cheek_left_x = 0.5f;
    float cheek_right_u = 0.0f;
    float cheek_left_u = 0.0f;

    [[nodiscard]] bool pass() const noexcept {
        return eyes_resolved && cheeks_resolved && cheek_right_x < eye_midpoint_x &&
               cheek_left_x > eye_midpoint_x && cheek_right_u > 0.0f && cheek_left_u < 0.0f;
    }
};

[[nodiscard]] HandednessReport check_handedness(const LandmarkSet& landmarks,
                                                const LandmarkMap& map, float aspect) noexcept;

} // namespace avatar::tracking
