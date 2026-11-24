#include "face_rig_fixture.h"
#include "test_registry.h"

#include <algorithm>
#include <limits>

namespace {
using namespace avatar;
constexpr core::TimestampNs kStart = 1'000'000'000;
constexpr core::TimestampNs kStep = 16'666'667;

double point_error(const rig::FaceRigOutput& out, const tracking::TrackingResult& input,
                   const test::RigFixture& f) {
    double maximum = 0;
    for (const auto& p : f.profile.landmarks) {
        const auto xy = test::rig_project(out.canonical_to_image, p.u, p.v);
        const auto expected = input.landmarks.points[*f.map.resolve(p.name)];
        maximum =
            std::max(maximum, std::hypot((xy[0] - expected.x) * 1280, (xy[1] - expected.y) * 720));
    }
    return maximum;
}
} // namespace

TEST_CASE(rig_synthetic_ground_truth_pose_and_reprojection) {
    test::RigFixture f;
    double maximum_px = 0, maximum_deg = 0, maximum_translation = 0;
    for (int i = 0; i < 21; ++i) {
        // Static snapshots isolate estimation accuracy from intentional filter lag.
        rig::FaceRig rig;
        rig.configure(f.profile);
        const auto r =
            f.result(kStart, static_cast<float>(i - 10) * 3, static_cast<float>(i - 10),
                     static_cast<float>(i - 10) * 2, static_cast<float>(i - 10) * 0.002f);
        const auto out = rig.update(r, kStart);
        CHECK(out.features_valid);
        CHECK(!out.solve_failed);
        maximum_px = std::max(maximum_px, point_error(out, r, f));
        maximum_deg = std::max(maximum_deg, test::inverse_pose_error(out, r.pose));
        for (int row = 0; row < 3; ++row) {
            double residual = out.head_from_camera[static_cast<std::size_t>(row * 4 + 3)];
            for (int col = 0; col < 3; ++col) {
                residual += out.head_from_camera[static_cast<std::size_t>(row * 4 + col)] *
                            r.pose.translation[static_cast<std::size_t>(col)];
            }
            maximum_translation = std::max(maximum_translation, std::abs(residual));
        }
    }
    std::printf(
        "T18 static: max_reprojection_px=%.8f max_pose_deg=%.6f inverse_translation_m=%.9f\n",
        maximum_px, maximum_deg, maximum_translation);
    CHECK(maximum_px < 1.0);
    CHECK(maximum_deg < 0.05);
    CHECK(maximum_translation < 1e-6);
}

TEST_CASE(rig_tracks_synthetic_motion_with_bounded_filter_lag) {
    test::RigFixture f;
    rig::FaceRig rig;
    rig.configure(f.profile);
    double max_error = 0, max_px = 0;
    for (int i = 0; i < 240; ++i) {
        const float phase = static_cast<float>(i) / 60;
        const auto r = f.result(kStart + static_cast<core::TimestampNs>(i) * kStep,
                                10 * std::sin(phase), 5 * std::sin(phase), 3 * std::sin(phase));
        const auto out = rig.update(r, r.source_timestamp_ns);
        CHECK(out.features_valid);
        max_error = std::max(max_error, test::inverse_pose_error(out, r.pose));
        max_px = std::max(max_px, point_error(out, r, f));
    }
    std::printf("T18 motion: max_pose_lag_deg=%.6f max_reprojection_px=%.8f\n", max_error, max_px);
    CHECK(max_error < 2.0);
    CHECK(max_px < 1.0);
}

TEST_CASE(rig_bootstraps_canonical_axes_and_filters_region_centroids) {
    test::RigFixture f;
    rig::FaceRig automatic;
    auto r = f.result();
    const auto boot = automatic.update(r, kStart);
    CHECK(boot.features_valid);
    CHECK(boot.regions[0].u < 0);
    CHECK(boot.regions[1].u > 0);
    CHECK(boot.regions[2].v < 0);
    const auto right_eye = test::rig_project(boot.canonical_to_image, 0.5, 0);
    CHECK(std::abs(right_eye[0] - r.landmarks.points[*f.map.resolve("iris_center_r")].x) < 1e-6);

    rig::FaceRig rig;
    rig.configure(f.profile);
    const auto first = rig.update(r, kStart);
    // The adapter subtracts the rest centroid from the live centroid; the first
    // accepted frame defines rest exactly.
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(std::abs(first.rest_regions[i].u - first.regions[i].u) < 1e-6f);
        CHECK(std::abs(first.rest_regions[i].v - first.regions[i].v) < 1e-6f);
    }
    // Move only an expression landmark. It must not enter the rigid H fit.
    r.landmarks.points[*f.map.resolve("iris_center_r")].x -= 0.12f;
    r.source_timestamp_ns += kStep;
    const auto second = rig.update(r, r.source_timestamp_ns);
    CHECK(std::abs(second.rest_regions[1].u - first.rest_regions[1].u) < 1e-6f);
    CHECK(second.regions[1].u != first.regions[1].u);
    const double raw_shift = 0.12 / 3 / std::abs(first.canonical_to_image[0]);
    const double expected = std::min(0.12, raw_shift * (1 - std::exp(-1.0 / 60 / 0.150)));
    CHECK(std::abs(second.regions[1].u - first.regions[1].u - expected) < 1e-5);
    CHECK(second.canonical_to_image == first.canonical_to_image);
    for (int i = 0; i < 120; ++i) {
        r.source_timestamp_ns += kStep;
        const auto output = rig.update(r, r.source_timestamp_ns);
        CHECK(output.regions[1].u <= first.regions[1].u + 0.120001f);
    }
    const auto clamped = rig.sample(r.source_timestamp_ns);
    CHECK(std::abs(clamped.regions[1].u - first.regions[1].u - 0.12) < 1e-5);
}

TEST_CASE(rig_bootstrap_prefers_a_neutral_frame_over_a_turned_first_frame) {
    test::RigFixture f;
    rig::FaceRig rig;
    const auto turned = rig.update(f.result(kStart, 30), kStart);
    CHECK(!turned.features_valid);
    const auto neutral_result = f.result(kStart + kStep);
    const auto neutral = rig.update(neutral_result, neutral_result.source_timestamp_ns);
    CHECK(neutral.features_valid);
    CHECK(!neutral.solve_failed);
    const auto projected = test::rig_project(neutral.canonical_to_image, 0.5, 0);
    const auto outer = neutral_result.landmarks.points[*f.map.resolve("eye_outer_r")];
    const auto inner = neutral_result.landmarks.points[*f.map.resolve("eye_inner_r")];
    const double mid_x = (static_cast<double>(outer.x) + inner.x) * 0.5;
    const double mid_y = (static_cast<double>(outer.y) + inner.y) * 0.5;
    CHECK(std::abs(projected[0] - mid_x) < 1e-6);
    CHECK(std::abs(projected[1] - mid_y) < 1e-6);
}

TEST_CASE(rig_profile_view_holds_a_hidden_eye_without_dropping_the_frame) {
    test::RigFixture f;
    rig::FaceRig rig;
    rig.configure(f.profile);
    auto r = f.result();
    const auto first = rig.update(r, kStart);
    CHECK(first.features_valid);
    // Hide the subject's right eye (viewer's left) and move the left eye. The
    // solve must continue from the visible subset; the hidden region holds.
    for (const char* name : {"eye_outer_r", "eye_inner_r", "iris_center_r"}) {
        r.landmarks.points[*f.map.resolve(name)] = {0.0f, 0.0f, 0.0f};
    }
    r.landmarks.points[*f.map.resolve("eye_outer_l")].x -= 0.03f;
    r.landmarks.points[*f.map.resolve("eye_inner_l")].x -= 0.03f;
    r.source_timestamp_ns += kStep;
    const auto out = rig.update(r, r.source_timestamp_ns);
    CHECK(out.features_valid);
    CHECK(!out.solve_failed);
    CHECK(std::abs(out.regions[1].u - first.regions[1].u) < 1e-5f);
    CHECK(std::abs(out.regions[1].v - first.regions[1].v) < 1e-5f);
    CHECK(std::abs(out.regions[0].u - first.regions[0].u) > 1e-5f);
}

TEST_CASE(rig_one_euro_reduces_rest_jitter_and_translation_noise) {
    test::RigFixture f;
    rig::FaceRig rig;
    rig.configure(f.profile);
    double input_energy = 0, output_energy = 0, translation_energy = 0;
    for (int i = 0; i < 360; ++i) {
        const float noise = 0.6f * std::sin(static_cast<float>(i) * 2.1f);
        const auto r = f.result(kStart + static_cast<core::TimestampNs>(i) * kStep, noise,
                                -noise * 0.5f, noise * 0.3f, noise * 0.002f);
        const auto output = rig.update(r, r.source_timestamp_ns);
        if (i > 60) {
            const double e = test::inverse_pose_error(output, f.result().pose);
            input_energy += noise * noise * 1.34;
            output_energy += e * e;
            translation_energy += output.head_from_camera[3] * output.head_from_camera[3];
        }
    }
    const double ratio = std::sqrt(output_energy / input_energy);
    std::printf("T18 jitter: rotation_rms_ratio=%.6f translation_rms_m=%.9f\n", ratio,
                std::sqrt(translation_energy / 299));
    CHECK(ratio < 0.35);
    CHECK(std::sqrt(translation_energy / 299) < 0.0006);
}

TEST_CASE(rig_rejects_landmark_and_pose_outliers_without_a_jump) {
    test::RigFixture f;
    rig::FaceRig rig;
    rig.configure(f.profile);
    auto r = f.result();
    const auto first = rig.update(r, kStart);
    r.source_timestamp_ns += kStep;
    r.landmarks.points[*f.map.resolve("chin")].x += 0.2f;
    r.landmarks.points[*f.map.resolve("temple_l")].y -= 0.15f;
    const auto robust = rig.update(r, r.source_timestamp_ns);
    CHECK(robust.features_valid);
    CHECK(!robust.solve_failed);
    CHECK(robust.inlier_count == first.inlier_count - 2);
    for (std::size_t i = 0; i < 9; ++i) {
        CHECK(std::abs(robust.canonical_to_image[i] - first.canonical_to_image[i]) < 1e-5);
    }
    const auto jump = f.result(kStart + 2 * kStep, 70);
    const auto rejected = rig.update(jump, jump.source_timestamp_ns);
    CHECK(test::inverse_pose_error(rejected, f.result().pose) < 0.05);
    CHECK_EQ(rejected.source_timestamp_ns, r.source_timestamp_ns);
    r = f.result(kStart + 3 * kStep);
    CHECK(rig.update(r, r.source_timestamp_ns).features_valid);
}

TEST_CASE(rig_loss_hold_expires_without_neutral_decay_or_timestamp_refresh) {
    test::RigFixture f;
    rig::FaceRig rig;
    rig.configure(f.profile);
    auto r = f.result(kStart, 25, 10, -5);
    const auto first = rig.update(r, kStart);
    r.valid = r.features_valid = false;
    r.source_timestamp_ns = kStart + 100'000'000;
    CHECK(rig.update(r, r.source_timestamp_ns).features_valid);
    const auto boundary = rig.sample(kStart + 150'000'000);
    CHECK(boundary.features_valid);
    const auto blank = rig.sample(kStart + 150'000'001);
    CHECK(!blank.features_valid);
    CHECK_EQ(blank.confidence, 0.0f);
    CHECK(blank.head_from_camera == first.head_from_camera);
    CHECK(blank.canonical_to_image == first.canonical_to_image);
    CHECK_EQ(blank.source_timestamp_ns, kStart);
    r = f.result(kStart);
    CHECK(!rig.update(r, kStart + 200'000'000).features_valid);
    r.source_timestamp_ns = kStart - 1;
    CHECK(!rig.update(r, kStart + 200'000'000).features_valid);
    r.source_timestamp_ns = kStart + 210'000'000;
    const auto reacquired = rig.update(r, r.source_timestamp_ns);
    CHECK(reacquired.features_valid);
    CHECK(test::inverse_pose_error(reacquired, f.result(kStart, 25, 10, -5).pose) < 20);
}

TEST_CASE(rig_prediction_is_clamped_and_does_not_warp_source_sampling) {
    test::RigFixture f;
    rig::FaceRig rig;
    rig.configure(f.profile);
    tracking::TrackingResult r;
    for (int i = 0; i < 60; ++i) {
        r = f.result(kStart + static_cast<core::TimestampNs>(i) * kStep,
                     static_cast<float>(i) * 0.2f, 0, 0, static_cast<float>(i) * 0.0001f);
        (void)rig.update(r, r.source_timestamp_ns);
    }
    const auto now = rig.sample(r.source_timestamp_ns);
    const auto predict = rig.sample(r.source_timestamp_ns + 50'000'000);
    const auto capped = rig.sample(r.source_timestamp_ns + 149'000'000);
    CHECK(predict.head_from_camera == capped.head_from_camera);
    CHECK(now.head_from_camera != predict.head_from_camera);
    CHECK(now.canonical_to_image == predict.canonical_to_image);
    CHECK(test::inverse_pose_error(predict, r.pose) < test::inverse_pose_error(now, r.pose));
    CHECK(!rig.sample(r.source_timestamp_ns + 151'000'000).features_valid);

    // The filter step cap is not the elapsed source interval used for velocity.
    // A long gap must not amplify prediction speed on reacquisition.
    rig::FaceRig reacquire;
    reacquire.configure(f.profile);
    (void)reacquire.update(f.result(), kStart);
    const auto returning = f.result(kStart + 1'000'000'000, 15);
    const auto current = reacquire.update(returning, returning.source_timestamp_ns);
    const auto future = reacquire.sample(returning.source_timestamp_ns + 50'000'000);
    const double current_angle = test::inverse_pose_error(current, f.result().pose);
    const double predicted_angle = test::inverse_pose_error(future, f.result().pose);
    CHECK(predicted_angle > current_angle);
    CHECK(predicted_angle < current_angle * 1.051);
}

TEST_CASE(rig_subset_fallback_and_degenerate_failure_hold) {
    test::RigFixture f;
    std::vector<tracking::LandmarkBinding> bindings;
    for (const auto& b : tracking::kMediaPipeFaceLandmarkerTable) {
        if (b.name != tracking::LandmarkName::forehead_center &&
            b.name != tracking::LandmarkName::jaw_left &&
            b.name != tracking::LandmarkName::iris_center_l &&
            b.name != tracking::LandmarkName::iris_center_r) {
            bindings.push_back(b);
        }
    }
    rig::FaceRig rig{tracking::LandmarkMap(bindings)};
    rig.configure(f.profile);
    auto r = f.result();
    const auto first = rig.update(r, kStart);
    CHECK(first.features_valid);
    CHECK(first.profile_subset);
    CHECK(point_error(first, r, f) < 1);
    for (auto& point : r.landmarks.points) {
        point = {0.5f, 0.5f, 0};
    }
    r.source_timestamp_ns += kStep;
    const auto bad = rig.update(r, r.source_timestamp_ns);
    CHECK(bad.features_valid);
    CHECK(bad.solve_failed);
    CHECK(bad.confidence < first.confidence);
    CHECK(bad.canonical_to_image == first.canonical_to_image);
    CHECK(!rig.sample(kStart + 151'000'000).features_valid);
    rig::FaceRig fresh;
    fresh.configure(f.profile);
    CHECK(!fresh.update(r, r.source_timestamp_ns).features_valid);
}

TEST_CASE(rig_nonfinite_inputs_and_inadequate_profiles_do_not_poison_state) {
    test::RigFixture f;
    rig::FaceRig rig;
    rig.configure(f.profile);
    auto r = f.result();
    (void)rig.update(r, kStart);
    r.source_timestamp_ns += kStep;
    r.pose.rotation[0] = std::numeric_limits<float>::quiet_NaN();
    const auto bad = rig.update(r, r.source_timestamp_ns);
    CHECK(bad.features_valid);
    CHECK_EQ(bad.source_timestamp_ns, kStart);
    auto short_profile = f.profile;
    short_profile.landmarks.resize(7);
    rig.configure(short_profile);
    r = f.result();
    CHECK(!rig.update(r, kStart).features_valid);
    rig.configure(f.profile);
    CHECK(rig.update(r, kStart).features_valid);
}

TEST_CASE(rig_noisy_perspective_fit_reprojects_under_one_pixel) {
    test::RigFixture f;
    rig::FaceRig rig;
    rig.configure(f.profile);
    auto r = f.result(kStart, 28, -15, 12);
    const auto exact = r;
    std::uint32_t random = 0x18;
    for (const auto& binding : tracking::kMediaPipeFaceLandmarkerTable) {
        auto& p = r.landmarks.points[binding.index];
        random = random * 1664525u + 1013904223u;
        p.x += (static_cast<float>(random >> 8) / 16777216 - 0.5f) * 0.0006f;
        random = random * 1664525u + 1013904223u;
        p.y += (static_cast<float>(random >> 8) / 16777216 - 0.5f) * 0.0006f;
    }
    const auto out = rig.update(r, kStart);
    CHECK(out.features_valid);
    const double error = point_error(out, exact, f);
    std::printf("T18 noisy perspective: max_reprojection_px=%.6f\n", error);
    CHECK(error < 1);
}

TEST_CASE(rig_three_coherent_pose_rejects_allow_a_smoothed_new_baseline) {
    test::RigFixture f;
    rig::FaceRig rig;
    rig.configure(f.profile);
    (void)rig.update(f.result(), kStart);
    for (int i = 1; i <= 2; ++i) {
        const auto r = f.result(kStart + static_cast<core::TimestampNs>(i) * kStep, 60);
        const auto out = rig.update(r, r.source_timestamp_ns);
        CHECK_EQ(out.source_timestamp_ns, kStart);
    }
    auto invalid = f.result(kStart + 3 * kStep, 60);
    invalid.valid = false;
    (void)rig.update(invalid, invalid.source_timestamp_ns);
    // Invalid input interrupts the consecutive-reject sequence.
    for (int i = 4; i <= 5; ++i) {
        const auto rejected = f.result(kStart + static_cast<core::TimestampNs>(i) * kStep, 60);
        const auto held = rig.update(rejected, rejected.source_timestamp_ns);
        CHECK_EQ(held.source_timestamp_ns, kStart);
    }
    const auto r = f.result(kStart + 6 * kStep, 60);
    const auto out = rig.update(r, r.source_timestamp_ns);
    CHECK_EQ(out.source_timestamp_ns, r.source_timestamp_ns);
    CHECK(test::inverse_pose_error(out, f.result().pose) > 1);
    CHECK(test::inverse_pose_error(out, r.pose) > 1); // filtering, never a snap to the raw jump
}

TEST_CASE(rig_missing_features_and_solver_failures_cannot_extend_the_hold) {
    test::RigFixture f;
    rig::FaceRig rig;
    rig.configure(f.profile);
    (void)rig.update(f.result(), kStart);
    for (int i = 1; i <= 12; ++i) {
        auto r = f.result(kStart + static_cast<core::TimestampNs>(i) * kStep);
        r.features_valid = false;
        const auto out = rig.update(r, r.source_timestamp_ns);
        CHECK_EQ(out.features_valid, i <= 8);
        CHECK_EQ(out.source_timestamp_ns, kStart);
    }
}

TEST_CASE(rig_failed_solve_damped_extrapolation_is_bounded) {
    test::RigFixture f;
    rig::FaceRig rig;
    rig.configure(f.profile);
    (void)rig.update(f.result(), kStart);
    auto r = f.result(kStart + kStep, 0, 0, 0, 0.001f);
    const auto last = rig.update(r, r.source_timestamp_ns);
    for (auto& point : r.landmarks.points) {
        point = {0.5f, 0.5f, 0};
    }
    r.source_timestamp_ns += kStep;
    const auto held = rig.update(r, r.source_timestamp_ns);
    CHECK(held.solve_failed);
    CHECK(held.features_valid);
    CHECK(held.confidence < last.confidence);
    CHECK(held.canonical_to_image != last.canonical_to_image);
    for (double u : {-1.5, 1.5}) {
        for (double v : {-2.0, 1.5}) {
            const auto a = test::rig_project(held.canonical_to_image, u, v);
            const auto b = test::rig_project(last.canonical_to_image, u, v);
            CHECK(std::hypot(a[0] - b[0], a[1] - b[1]) <= 0.008001);
        }
    }
}
