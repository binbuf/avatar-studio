#include "test_registry.h"

#include "avatar/capture/synthetic_face_source.h"
#include "avatar/capture/video_frame.h"
#include "avatar/core/clock.h"

#include <cmath>
#include <cstdint>

namespace {

using avatar::capture::FaceGroundTruth;
using avatar::capture::frame_content_hash;
using avatar::capture::GroundTruthLandmark;
using avatar::capture::PixelFormat;
using avatar::capture::pose_at;
using avatar::capture::SyntheticFaceConfig;
using avatar::capture::SyntheticFaceSource;
using avatar::capture::VideoFrame;
using avatar::core::FrameClock;
using avatar::core::ManualTimeSource;

SyntheticFaceConfig make_config(std::uint32_t width, std::uint32_t height, PixelFormat format,
                                float barrel_k1 = 0.0f) {
    SyntheticFaceConfig config;
    config.width = width;
    config.height = height;
    config.format = format;
    config.barrel_k1 = barrel_k1;
    return config;
}

struct FaceFixture {
    ManualTimeSource time;
    FrameClock clock;
    SyntheticFaceSource source;

    FaceFixture(std::uint32_t width, std::uint32_t height, PixelFormat format,
                float barrel_k1 = 0.0f)
        : time(0), clock(time), source(clock, make_config(width, height, format, barrel_k1)) {}
};

bool window_has_green(const VideoFrame& frame, int centre_x, int centre_y, int radius) {
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int x = centre_x + dx;
            const int y = centre_y + dy;
            if (x < 0 || y < 0 || x >= static_cast<int>(frame.width) ||
                y >= static_cast<int>(frame.height)) {
                continue;
            }
            const std::uint8_t* pixel = frame.plane(0) +
                                        static_cast<std::size_t>(y) * frame.stride(0) +
                                        static_cast<std::size_t>(x) * 4u;
            if (pixel[1] > 200 && pixel[2] < 80 && pixel[0] < 80) {
                return true;
            }
        }
    }
    return false;
}

void check_ground_truth_matches_pose(const FaceGroundTruth& truth,
                                     const avatar::capture::FacePoseSample& pose) {
    for (std::size_t index = 0; index < truth.correspondence_count; ++index) {
        const auto& correspondence = truth.correspondences[index];
        const auto ideal =
            SyntheticFaceSource::project_canonical(pose, correspondence.u, correspondence.v);
        CHECK(std::fabs(ideal[0] - correspondence.ideal_x) < 1e-5f);
        CHECK(std::fabs(ideal[1] - correspondence.ideal_y) < 1e-5f);
    }
}

} // namespace

TEST_CASE(synthetic_face_emits_precise_ground_truth) {
    FaceFixture fixture(640, 480, PixelFormat::bgra8);
    CHECK(fixture.source.start().ok());
    CHECK(fixture.source.info().ground_truth);
    CHECK(fixture.source.produce());

    const auto truth = fixture.source.latest_ground_truth();
    CHECK(truth.has_value());
    if (!truth) {
        return;
    }
    CHECK(truth->timestamp_ns == 0);
    CHECK(truth->sequence == 1);
    CHECK(truth->features_valid);
    CHECK(truth->correspondence_count == avatar::capture::kGroundTruthLandmarkCount);
    CHECK(std::fabs(truth->barrel_k1) < 1e-6f);

    // Neutral path keyframe: identity rotation, 0.6 m in front of the camera.
    CHECK(std::fabs(pose_at(truth->camera_from_head, 0, 0) - 1.0f) < 1e-5f);
    CHECK(std::fabs(pose_at(truth->camera_from_head, 1, 1) - 1.0f) < 1e-5f);
    CHECK(std::fabs(pose_at(truth->camera_from_head, 2, 2) - 1.0f) < 1e-5f);
    CHECK(std::fabs(pose_at(truth->camera_from_head, 2, 3) + 0.6f) < 1e-5f);

    const auto& pose = fixture.source.last_pose();
    check_ground_truth_matches_pose(*truth, pose);

    const auto centre = SyntheticFaceSource::project_canonical(pose, 0.0f, 0.0f);
    CHECK(std::fabs(centre[0] - 0.5f) < 1e-5f);
    CHECK(std::fabs(centre[1] - 0.5f) < 1e-5f);

    const auto* marker = truth->find(GroundTruthLandmark::marker_right_cheek);
    CHECK(marker != nullptr);
    if (marker == nullptr) {
        return;
    }
    // Subject's right cheek appears on the viewer's left in an unmirrored image.
    CHECK(marker->x < 0.5f);
    CHECK(std::fabs(marker->x - marker->ideal_x) < 1e-6f);
    CHECK(std::fabs(marker->y - marker->ideal_y) < 1e-6f);

    auto lease = fixture.source.ring().try_acquire_latest();
    CHECK(lease.has_value());
    if (lease) {
        const int pixel_x = static_cast<int>(marker->x * 640.0f);
        const int pixel_y = static_cast<int>(marker->y * 480.0f);
        CHECK(window_has_green(lease->frame(), pixel_x, pixel_y, 3));
    }
}

TEST_CASE(synthetic_face_moves_on_the_scripted_path) {
    FaceFixture fixture(640, 480, PixelFormat::bgra8);
    CHECK(fixture.source.start().ok());
    CHECK(fixture.source.produce());
    const auto first_truth = fixture.source.latest_ground_truth();
    const auto first_homography = fixture.source.last_pose().canonical_to_image;
    CHECK(first_truth.has_value());

    fixture.time.advance(1'000'000'000);
    CHECK(fixture.source.produce());
    const auto second_truth = fixture.source.latest_ground_truth();
    CHECK(second_truth.has_value());
    if (first_truth && second_truth) {
        CHECK(!(first_truth->camera_from_head == second_truth->camera_from_head));
        check_ground_truth_matches_pose(*second_truth, fixture.source.last_pose());
    }
    CHECK(fixture.source.last_pose().canonical_to_image != first_homography);
}

TEST_CASE(synthetic_face_barrel_variant_distorts_ground_truth) {
    FaceFixture fixture(640, 480, PixelFormat::bgra8, 0.25f);
    CHECK(fixture.source.start().ok());
    CHECK(fixture.source.produce());

    const auto truth = fixture.source.latest_ground_truth();
    CHECK(truth.has_value());
    if (!truth) {
        return;
    }
    CHECK(std::fabs(truth->barrel_k1 - 0.25f) < 1e-6f);
    const auto* marker = truth->find(GroundTruthLandmark::marker_right_cheek);
    CHECK(marker != nullptr);
    if (marker == nullptr) {
        return;
    }
    // Away from the centre the barrel model moves the point measurably.
    CHECK(std::fabs(marker->x - marker->ideal_x) > 1e-4f);
    CHECK(std::fabs(marker->y - marker->ideal_y) > 1e-4f);

    auto lease = fixture.source.ring().try_acquire_latest();
    CHECK(lease.has_value());
    if (lease) {
        const int pixel_x = static_cast<int>(marker->x * 640.0f);
        const int pixel_y = static_cast<int>(marker->y * 480.0f);
        CHECK(window_has_green(lease->frame(), pixel_x, pixel_y, 4));
    }
}

TEST_CASE(synthetic_face_is_deterministic) {
    FaceFixture first(640, 480, PixelFormat::bgra8);
    FaceFixture second(640, 480, PixelFormat::bgra8);
    CHECK(first.source.start().ok());
    CHECK(second.source.start().ok());
    CHECK(first.source.produce());
    CHECK(second.source.produce());

    auto first_lease = first.source.ring().try_acquire_latest();
    auto second_lease = second.source.ring().try_acquire_latest();
    CHECK(first_lease.has_value());
    CHECK(second_lease.has_value());
    if (first_lease && second_lease) {
        CHECK(frame_content_hash(first_lease->frame()) ==
              frame_content_hash(second_lease->frame()));
    }
}

TEST_CASE(synthetic_face_supports_nv12) {
    FaceFixture fixture(1280, 720, PixelFormat::nv12);
    CHECK(fixture.source.start().ok());
    CHECK(fixture.source.produce());
    auto lease = fixture.source.ring().try_acquire_latest();
    CHECK(lease.has_value());
    if (lease) {
        CHECK(lease->frame().format == PixelFormat::nv12);
        CHECK(lease->frame().plane(1) != nullptr);
        CHECK(lease->frame().width == 1280);
    }
    CHECK(fixture.source.latest_ground_truth().has_value());
}

TEST_CASE(synthetic_face_pose_interpolates_between_keyframes) {
    SyntheticFaceConfig config = make_config(640, 480, PixelFormat::bgra8);
    config.path = avatar::capture::default_face_motion_path();
    const auto neutral = SyntheticFaceSource::evaluate_pose(config, 0);
    const auto halfway = SyntheticFaceSource::evaluate_pose(config, 500'000'000);
    const auto first_key = SyntheticFaceSource::evaluate_pose(config, 1'000'000'000);

    // Neutral has an identity rotation; halfway is a smoothstep blend of the
    // 0 -> 1 s segment, so the yaw rotation term (m02) is positive but smaller
    // than the first keyframe's.
    CHECK(std::fabs(neutral.camera_from_head[1]) < 1e-6f);
    CHECK(halfway.camera_from_head[2] > 0.0f);
    CHECK(halfway.camera_from_head[2] < first_key.camera_from_head[2]);
}

TEST_CASE(synthetic_face_timestamps_are_strictly_monotonic) {
    FaceFixture fixture(320, 240, PixelFormat::bgra8);
    CHECK(fixture.source.start().ok());
    avatar::core::TimestampNs previous = 0;
    bool first = true;
    for (int frame = 0; frame < 12; ++frame) {
        fixture.time.advance(33'333'334);
        CHECK(fixture.source.produce());
        auto lease = fixture.source.ring().try_acquire_latest();
        CHECK(lease.has_value());
        if (!lease) {
            return;
        }
        const avatar::core::TimestampNs timestamp = lease->frame().timestamp_ns;
        if (!first) {
            CHECK(timestamp > previous);
        }
        previous = timestamp;
        first = false;
    }
}

TEST_CASE(synthetic_face_distortion_is_identity_at_the_centre) {
    const auto centre = SyntheticFaceSource::distort({0.5f, 0.5f}, 0.08f, 16.0f / 9.0f);
    CHECK(std::fabs(centre[0] - 0.5f) < 1e-6f);
    CHECK(std::fabs(centre[1] - 0.5f) < 1e-6f);
    const auto unchanged = SyntheticFaceSource::distort({0.3f, 0.7f}, 0.0f, 16.0f / 9.0f);
    CHECK(std::fabs(unchanged[0] - 0.3f) < 1e-6f);
    CHECK(std::fabs(unchanged[1] - 0.7f) < 1e-6f);
}
