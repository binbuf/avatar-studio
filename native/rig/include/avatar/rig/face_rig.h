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

    // Buffers a tracker result without applying it. Results are kept newest-first
    // (at most four) so a result whose source timestamp is ahead of the render
    // clock waits instead of being applied early. Older buffered results are
    // dropped, never queued indefinitely.
    void submit(const tracking::TrackingResult& result);

    // Convenience for callers that have a result and the current render time:
    // submit, then sample. The result is applied only when its source timestamp
    // is at or before `render_time_ns`.
    [[nodiscard]] FaceRigOutput update(const tracking::TrackingResult& result,
                                       core::TimestampNs render_time_ns);

    // Applies the newest buffered result whose source timestamp is at or before
    // `render_time_ns`, then predicts between updates. Duplicate/older input
    // timestamps never advance filters or refresh the loss hold. Time is
    // supplied by the caller. A result from the future remains buffered.
    [[nodiscard]] FaceRigOutput sample(core::TimestampNs render_time_ns);

    [[nodiscard]] std::size_t pending_count() const noexcept;
    [[nodiscard]] std::uint64_t pending_dropped() const noexcept;

private:
    struct State;
    FaceRigOutput apply_result(const tracking::TrackingResult& result,
                               core::TimestampNs render_time_ns);
    FaceRigOutput sample_current(core::TimestampNs render_time_ns) const;
    void apply_pending(core::TimestampNs render_time_ns);
    std::unique_ptr<State> state_;
};

} // namespace avatar::rig
