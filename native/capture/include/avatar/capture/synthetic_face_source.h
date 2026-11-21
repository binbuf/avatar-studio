#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "avatar/capture/icamera_source.h"

namespace avatar::capture {

/// One keyframe of the scripted 6-DOF face path.
///
/// `time_ns` is relative to the source's start timestamp. Rotation is applied
/// as Rz(roll) * Ry(yaw) * Rx(pitch) in degrees. Translation is metres relative
/// to the base distance, so a neutral keyframe is all zeros.
struct FaceMotionKeyframe {
    TimestampNs time_ns = 0;
    std::array<float, 3> translation_m{0.0f, 0.0f, 0.0f};
    std::array<float, 3> euler_deg{0.0f, 0.0f, 0.0f}; // yaw, pitch, roll
    float mouth_open = 0.0f;
    float blink = 0.0f;
};

/// A default scripted path: a slow yaw sweep with a nod, a roll and a small
/// lateral translation. Deterministic and short enough for a component test.
[[nodiscard]] std::vector<FaceMotionKeyframe> default_face_motion_path();

struct SyntheticFaceConfig {
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    PixelFormat format = PixelFormat::bgra8;
    float vfov_deg = 35.0f;
    float interocular_m = 0.065f;
    float base_distance_m = 0.60f;
    /// Radial barrel coefficient. Zero is a pinhole camera; 0.06-0.10 is a
    /// plausible wide webcam. Negative (pincushion) values are rejected.
    float barrel_k1 = 0.0f;
    bool draw_marker = true;
    std::vector<FaceMotionKeyframe> path;
};

/// Pose and homography for one instant of the scripted path.
struct FacePoseSample {
    std::array<float, 16> camera_from_head{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                           0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    /// Canonical (u,v) -> normalised image (x,y), row-major 3x3, pinhole only.
    std::array<float, 9> canonical_to_image{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    float mouth_open = 0.0f;
    float blink = 0.0f;
};

/// Deterministic CPU-rendered face on a scripted 6-DOF path.
///
/// The source renders a canonical face plane through the exact plane-induced
/// homography for the pose, so the emitted ground truth is not an approximation:
/// `correspondences[i].ideal_x/ideal_y` are the pinhole projection of the
/// canonical point, and `x/y` apply the same barrel model the renderer uses.
/// A test can therefore solve a homography from the correspondences and compare
/// it numerically, or assert that a marker lands on the expected pixel.
///
/// Canonical space: origin between the eye centres, +u toward the subject's
/// right eye, +v toward the forehead, inter-ocular distance 1.0. In an
/// unmirrored image the subject's right side appears on the viewer's left, so
/// +u maps to a smaller image x.
class SyntheticFaceSource final : public ICameraSource {
public:
    explicit SyntheticFaceSource(core::FrameClock& clock, SyntheticFaceConfig config = {});

    [[nodiscard]] core::Status start() override;
    void stop() override;
    [[nodiscard]] FrameRing& ring() noexcept override;
    [[nodiscard]] bool produce() override;
    [[nodiscard]] SourceInfo info() const noexcept override;
    [[nodiscard]] std::optional<FaceGroundTruth> latest_ground_truth() const override;

    [[nodiscard]] const FacePoseSample& last_pose() const noexcept { return last_pose_; }
    [[nodiscard]] TimestampNs start_timestamp() const noexcept { return start_ns_; }
    [[nodiscard]] TimestampNs last_timestamp() const noexcept { return last_ns_; }

    /// Evaluates the scripted path without rendering. `elapsed_ns` is relative
    /// to the source start. Exposed so later rig tests can derive the expected
    /// homography independently of the renderer.
    [[nodiscard]] static FacePoseSample evaluate_pose(const SyntheticFaceConfig& config,
                                                      TimestampNs elapsed_ns);

    /// Applies the pinhole homography to a canonical point. Returns normalised
    /// image coordinates.
    [[nodiscard]] static std::array<float, 2> project_canonical(const FacePoseSample& pose, float u,
                                                                float v) noexcept;

    /// Forward barrel distortion in normalised image coordinates.
    [[nodiscard]] static std::array<float, 2> distort(std::array<float, 2> ideal, float k1,
                                                      float aspect) noexcept;

private:
    core::FrameClock& clock_;
    SyntheticFaceConfig config_;
    FrameRing ring_;
    bool started_ = false;
    bool has_last_ = false;
    TimestampNs start_ns_ = 0;
    TimestampNs last_ns_ = 0;
    FacePoseSample last_pose_{};
    std::optional<FaceGroundTruth> ground_truth_{};
};

} // namespace avatar::capture
