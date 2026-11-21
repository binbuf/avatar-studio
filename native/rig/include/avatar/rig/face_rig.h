#pragma once

#include <array>
#include <memory>

#include "avatar/tracking/profile_correspondence.h"

namespace avatar::rig {

// All public values are plain CPU data. Matrices are row-major, multiply column
// vectors, and sample the unmirrored normalised image. No Eigen/engine ABI.
struct RegionAnchor {
    float u = 0.0f;
    float v = 0.0f;
};

struct FaceRigOutput {
    core::TimestampNs source_timestamp_ns = 0;
    // Inverse of the filtered/predicted camera_from_head tracker pose, metres.
    std::array<float, 16> head_from_camera{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::array<float, 9> canonical_to_image{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<RegionAnchor, 3> regions{}; // eye_l, eye_r, mouth; absolute canonical centres
    float confidence = 0.0f;
    bool features_valid = false;
    std::size_t inlier_count = 0;
    bool profile_subset = false; // caller may surface RIG-003
    bool solve_failed = false;   // caller may surface RIG-002
};

class FaceRig {
public:
    explicit FaceRig(tracking::LandmarkMap map = tracking::LandmarkMap::mediapipe(),
                     float image_aspect = 16.0f / 9.0f);
    ~FaceRig();
    FaceRig(FaceRig&&) noexcept;
    FaceRig& operator=(FaceRig&&) noexcept;
    FaceRig(const FaceRig&) = delete;
    FaceRig& operator=(const FaceRig&) = delete;

    // Empty profile bootstraps neutral canonical space on the next usable frame.
    // Configure/reset also resets the timestamp timeline and all temporal state.
    void configure(const tracking::ProfileLandmarks& profile);
    void reset();
    [[nodiscard]] FaceRigOutput update(const tracking::TrackingResult& result,
                                       core::TimestampNs render_time_ns);
    // Render between tracker updates. Duplicate/older input timestamps do not
    // advance filters or refresh the loss hold. Time is supplied by the caller.
    [[nodiscard]] FaceRigOutput sample(core::TimestampNs render_time_ns) const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace avatar::rig
