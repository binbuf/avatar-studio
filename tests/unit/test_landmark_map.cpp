#include "test_registry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "avatar/capture/synthetic_face_source.h"
#include "avatar/tracking/canonical_face_frame.h"
#include "avatar/tracking/landmark_map.h"
#include "avatar/tracking/profile_correspondence.h"

namespace {

using avatar::capture::SyntheticFaceConfig;
using avatar::capture::SyntheticFaceSource;
using avatar::tracking::build_profile_correspondences;
using avatar::tracking::CanonicalFaceFrame;
using avatar::tracking::check_handedness;
using avatar::tracking::CorrespondenceSet;
using avatar::tracking::kLandmarkNameCount;
using avatar::tracking::kMediaPipeFaceLandmarkerTable;
using avatar::tracking::kMinimumSamplingCorrespondences;
using avatar::tracking::kNoLandmarkIndex;
using avatar::tracking::kOnnxFaceMeshV2Table;
using avatar::tracking::Landmark;
using avatar::tracking::landmark_name;
using avatar::tracking::LandmarkBinding;
using avatar::tracking::LandmarkMap;
using avatar::tracking::LandmarkName;
using avatar::tracking::LandmarkSet;
using avatar::tracking::parse_landmark_name;
using avatar::tracking::profile_landmarks_from_json;
using avatar::tracking::ProfileLandmark;
using avatar::tracking::ProfileLandmarks;

struct CanonicalPoint {
    LandmarkName name;
    float u;
    float v;
};

constexpr std::array<CanonicalPoint, kLandmarkNameCount> kCanonicalPoints = {{
    {LandmarkName::eye_outer_r, 0.70f, 0.00f},
    {LandmarkName::eye_inner_r, 0.30f, 0.00f},
    {LandmarkName::eye_outer_l, -0.70f, 0.00f},
    {LandmarkName::eye_inner_l, -0.30f, 0.00f},
    {LandmarkName::iris_center_r, 0.50f, 0.00f},
    {LandmarkName::iris_center_l, -0.50f, 0.00f},
    {LandmarkName::nose_bridge, 0.00f, 0.05f},
    {LandmarkName::nose_tip, 0.00f, -0.32f},
    {LandmarkName::nose_left, -0.10f, -0.32f},
    {LandmarkName::nose_right, 0.10f, -0.32f},
    {LandmarkName::mouth_corner_r, 0.30f, -0.78f},
    {LandmarkName::mouth_corner_l, -0.30f, -0.78f},
    {LandmarkName::mouth_upper_center, 0.00f, -0.70f},
    {LandmarkName::mouth_lower_center, 0.00f, -0.86f},
    {LandmarkName::chin, 0.00f, -1.05f},
    {LandmarkName::jaw_left, -0.75f, -0.75f},
    {LandmarkName::jaw_right, 0.75f, -0.75f},
    {LandmarkName::temple_l, -0.80f, 0.55f},
    {LandmarkName::temple_r, 0.80f, 0.55f},
    {LandmarkName::cheek_l, -0.65f, -0.15f},
    {LandmarkName::cheek_r, 0.65f, -0.15f},
    {LandmarkName::forehead_center, 0.00f, 1.05f},
}};

constexpr float kAspect = 1280.0f / 720.0f;

SyntheticFaceConfig synthetic_config() {
    SyntheticFaceConfig config;
    config.width = 1280;
    config.height = 720;
    config.path = avatar::capture::default_face_motion_path();
    return config;
}

avatar::capture::FacePoseSample neutral_pose() {
    return SyntheticFaceSource::evaluate_pose(synthetic_config(), 0);
}

LandmarkSet synthetic_landmarks(const LandmarkMap& map, bool marker_on_right_cheek) {
    LandmarkSet set;
    set.count = LandmarkSet::kCapacity;
    const auto pose = neutral_pose();
    for (const CanonicalPoint& point : kCanonicalPoints) {
        const std::optional<std::size_t> index = map.resolve(point.name);
        if (!index.has_value()) {
            continue;
        }
        const float u =
            marker_on_right_cheek && point.name == LandmarkName::cheek_r ? 0.62f : point.u;
        const float v =
            marker_on_right_cheek && point.name == LandmarkName::cheek_r ? -0.25f : point.v;
        const auto image = SyntheticFaceSource::project_canonical(pose, u, v);
        set.points[*index] = Landmark{image[0], image[1], 0.0f};
    }
    return set;
}

ProfileLandmarks make_profile(std::initializer_list<LandmarkName> names) {
    ProfileLandmarks profile;
    for (const LandmarkName name : names) {
        const auto found =
            std::find_if(kCanonicalPoints.begin(), kCanonicalPoints.end(),
                         [name](const CanonicalPoint& point) { return point.name == name; });
        profile.landmarks.push_back(ProfileLandmark{landmark_name(name), found->u, found->v});
    }
    return profile;
}

std::array<LandmarkBinding, kLandmarkNameCount> swapped_table() {
    std::array<LandmarkBinding, kLandmarkNameCount> table = kMediaPipeFaceLandmarkerTable;
    const auto index_of = [&table](LandmarkName name) {
        for (std::size_t index = 0; index < table.size(); ++index) {
            if (table[index].name == name) {
                return index;
            }
        }
        return table.size();
    };
    const std::array<std::pair<LandmarkName, LandmarkName>, 9> pairs = {{
        {LandmarkName::eye_outer_r, LandmarkName::eye_outer_l},
        {LandmarkName::eye_inner_r, LandmarkName::eye_inner_l},
        {LandmarkName::iris_center_r, LandmarkName::iris_center_l},
        {LandmarkName::nose_right, LandmarkName::nose_left},
        {LandmarkName::mouth_corner_r, LandmarkName::mouth_corner_l},
        {LandmarkName::jaw_right, LandmarkName::jaw_left},
        {LandmarkName::temple_r, LandmarkName::temple_l},
        {LandmarkName::cheek_r, LandmarkName::cheek_l},
    }};
    for (const auto& pair : pairs) {
        const std::size_t right = index_of(pair.first);
        const std::size_t left = index_of(pair.second);
        std::swap(table[right].index, table[left].index);
    }
    return table;
}

} // namespace

TEST_CASE(landmark_map_resolves_every_documented_name) {
    const LandmarkMap mediapipe = LandmarkMap::mediapipe();
    const LandmarkMap onnx = LandmarkMap::onnx();
    for (std::size_t index = 0; index < kLandmarkNameCount; ++index) {
        const auto name = static_cast<LandmarkName>(index);
        const auto parsed = parse_landmark_name(landmark_name(name));
        CHECK(parsed.has_value());
        if (parsed.has_value()) {
            CHECK(*parsed == name);
        }
        for (const LandmarkMap* map : {&mediapipe, &onnx}) {
            CHECK(map->knows(name));
            CHECK(map->resolve(name).has_value());
            CHECK(map->resolve(name).value() < LandmarkSet::kCapacity);
        }
    }
    CHECK_EQ(mediapipe.resolved_count(), kLandmarkNameCount);
    CHECK_EQ(onnx.resolved_count(), kLandmarkNameCount);
    CHECK(!mediapipe.has_duplicate_indices());
    CHECK(!onnx.has_duplicate_indices());
    CHECK(mediapipe.table_pin().find("478") != std::string_view::npos);
    CHECK(onnx.table_pin().find("478") != std::string_view::npos);
}

TEST_CASE(landmark_map_pins_the_asymmetric_names) {
    const LandmarkMap map = LandmarkMap::mediapipe();
    CHECK_EQ(*map.resolve(LandmarkName::eye_outer_r), 33u);
    CHECK_EQ(*map.resolve(LandmarkName::eye_outer_l), 263u);
    CHECK_EQ(*map.resolve(LandmarkName::iris_center_r), 468u);
    CHECK_EQ(*map.resolve(LandmarkName::iris_center_l), 473u);
    CHECK_EQ(*map.resolve(LandmarkName::cheek_r), 50u);
    CHECK_EQ(*map.resolve(LandmarkName::cheek_l), 280u);
    CHECK_EQ(*map.resolve(LandmarkName::jaw_right), 234u);
    CHECK_EQ(*map.resolve(LandmarkName::jaw_left), 454u);
    CHECK_EQ(*map.resolve(LandmarkName::temple_r), 127u);
    CHECK_EQ(*map.resolve(LandmarkName::temple_l), 356u);
    CHECK_EQ(*map.resolve(LandmarkName::nose_right), 129u);
    CHECK_EQ(*map.resolve(LandmarkName::nose_left), 358u);
    CHECK_EQ(*map.resolve(LandmarkName::mouth_corner_r), 61u);
    CHECK_EQ(*map.resolve(LandmarkName::mouth_corner_l), 291u);
    CHECK_EQ(*map.resolve(LandmarkName::forehead_center), 10u);
}

TEST_CASE(landmark_map_unknown_names_are_ignored) {
    const LandmarkMap map = LandmarkMap::mediapipe();
    CHECK(!map.resolve("marker_right_cheek").has_value());
    CHECK(!map.resolve("future_landmark").has_value());
    CHECK(!map.resolve("").has_value());
    CHECK(!parse_landmark_name("marker_right_cheek").has_value());
}

TEST_CASE(landmark_map_duplicate_indices_are_reported) {
    const std::array<LandmarkBinding, 3> bindings = {{
        {LandmarkName::cheek_r, 50},
        {LandmarkName::cheek_l, 50},
        {LandmarkName::eye_outer_r, 33},
    }};
    const LandmarkMap map(bindings, LandmarkSet::kCapacity);
    CHECK(map.resolve(LandmarkName::cheek_r).has_value());
    CHECK(!map.resolve(LandmarkName::cheek_l).has_value());
    CHECK_EQ(map.duplicate_indices().size(), 1u);
    CHECK_EQ(map.duplicate_indices()[0], 50u);
    CHECK_EQ(map.duplicate_names().size(), 1u);
    CHECK(map.duplicate_names()[0] == LandmarkName::cheek_l);
}

TEST_CASE(asymmetric_marker_subject_right_maps_to_viewer_left) {
    for (const LandmarkMap& map : {LandmarkMap::mediapipe(), LandmarkMap::onnx()}) {
        const LandmarkSet set = synthetic_landmarks(map, true);
        const auto report = check_handedness(set, map, kAspect);
        CHECK(report.eyes_resolved);
        CHECK(report.cheeks_resolved);
        CHECK(report.cheek_right_x < report.eye_midpoint_x);
        CHECK(report.cheek_left_x > report.eye_midpoint_x);
        CHECK(report.cheek_right_u > 0.0f);
        CHECK(report.cheek_left_u < 0.0f);
        CHECK(report.pass());

        const auto pose = neutral_pose();
        const auto marker = SyntheticFaceSource::project_canonical(pose, 0.62f, -0.25f);
        const auto point = map.point(set, LandmarkName::cheek_r);
        CHECK(point.has_value());
        CHECK(std::fabs(point->x - marker[0]) < 1e-5f);
        CHECK(std::fabs(point->y - marker[1]) < 1e-5f);
    }
}

TEST_CASE(asymmetric_marker_swapped_table_fails) {
    const LandmarkSet set = synthetic_landmarks(LandmarkMap::mediapipe(), true);
    const auto table = swapped_table();
    const LandmarkMap swapped(table, LandmarkSet::kCapacity);
    const auto report = check_handedness(set, swapped, kAspect);
    CHECK(report.eyes_resolved);
    CHECK(report.cheeks_resolved);
    CHECK(report.cheek_right_x > report.eye_midpoint_x);
    CHECK(!report.pass());
}

TEST_CASE(canonical_axes_point_to_subject_right_and_forehead) {
    const LandmarkMap map = LandmarkMap::mediapipe();
    const LandmarkSet set = synthetic_landmarks(map, false);
    const auto pose = neutral_pose();

    const auto right_eye = SyntheticFaceSource::project_canonical(pose, 1.0f, 0.0f);
    const auto left_eye = SyntheticFaceSource::project_canonical(pose, -1.0f, 0.0f);
    CHECK(right_eye[0] < left_eye[0]);
    const auto forehead = SyntheticFaceSource::project_canonical(pose, 0.0f, 1.0f);
    const auto chin = SyntheticFaceSource::project_canonical(pose, 0.0f, -1.0f);
    CHECK(forehead[1] < chin[1]);

    const auto frame = CanonicalFaceFrame::from_landmarks(set, map, kAspect);
    CHECK(frame.has_value());
    const auto right = frame->project(*map.point(set, LandmarkName::iris_center_r));
    const auto left = frame->project(*map.point(set, LandmarkName::iris_center_l));
    const auto forehead_uv = frame->project(*map.point(set, LandmarkName::forehead_center));
    const auto chin_uv = frame->project(*map.point(set, LandmarkName::chin));
    CHECK(right[0] > 0.45f && right[0] < 0.55f);
    CHECK(left[0] < -0.45f && left[0] > -0.55f);
    CHECK(forehead_uv[1] > 0.9f);
    CHECK(chin_uv[1] < -0.9f);
}

TEST_CASE(profile_correspondences_require_eight_inliers) {
    const LandmarkMap map = LandmarkMap::mediapipe();
    const LandmarkSet set = synthetic_landmarks(map, false);
    const ProfileLandmarks eight = make_profile(
        {LandmarkName::eye_outer_r, LandmarkName::eye_inner_r, LandmarkName::eye_outer_l,
         LandmarkName::eye_inner_l, LandmarkName::nose_tip, LandmarkName::mouth_corner_r,
         LandmarkName::mouth_corner_l, LandmarkName::chin});
    const CorrespondenceSet enough = build_profile_correspondences(eight, map, set);
    CHECK_EQ(enough.usable_count(), kMinimumSamplingCorrespondences);
    CHECK(enough.sampling_supported);
    CHECK(enough.profile_complete);
    CHECK(!enough.diagnostic.has_value());

    ProfileLandmarks seven = eight;
    seven.landmarks.pop_back();
    const CorrespondenceSet too_few = build_profile_correspondences(seven, map, set);
    CHECK_EQ(too_few.usable_count(), 7u);
    CHECK(!too_few.sampling_supported);
    CHECK(too_few.diagnostic.has_value());
    CHECK(*too_few.diagnostic == avatar::core::DiagnosticCode::rig_003);
}

TEST_CASE(profile_correspondences_fall_back_and_raise_rig_003) {
    std::array<LandmarkBinding, kLandmarkNameCount> table = kMediaPipeFaceLandmarkerTable;
    for (LandmarkBinding& binding : table) {
        if (binding.name == LandmarkName::iris_center_r ||
            binding.name == LandmarkName::iris_center_l) {
            binding.index = kNoLandmarkIndex;
        }
    }
    const LandmarkMap map(table, LandmarkSet::kCapacity);
    const LandmarkSet set = synthetic_landmarks(LandmarkMap::mediapipe(), false);
    const ProfileLandmarks profile = make_profile(
        {LandmarkName::eye_outer_r, LandmarkName::eye_inner_r, LandmarkName::eye_outer_l,
         LandmarkName::eye_inner_l, LandmarkName::iris_center_r, LandmarkName::iris_center_l,
         LandmarkName::nose_tip, LandmarkName::mouth_corner_r, LandmarkName::mouth_corner_l,
         LandmarkName::chin});
    const CorrespondenceSet result = build_profile_correspondences(profile, map, set);
    CHECK_EQ(result.usable_count(), 8u);
    CHECK(result.sampling_supported);
    CHECK(!result.profile_complete);
    CHECK_EQ(result.missing_names.size(), 2u);
    CHECK(result.diagnostic.has_value());
    CHECK(*result.diagnostic == avatar::core::DiagnosticCode::rig_003);
}

TEST_CASE(profile_correspondences_report_duplicate_indices) {
    const LandmarkMap map = LandmarkMap::mediapipe();
    const LandmarkSet set = synthetic_landmarks(map, false);
    ProfileLandmarks profile = make_profile(
        {LandmarkName::eye_outer_r, LandmarkName::eye_inner_r, LandmarkName::eye_outer_l,
         LandmarkName::eye_inner_l, LandmarkName::nose_tip, LandmarkName::mouth_corner_r,
         LandmarkName::mouth_corner_l, LandmarkName::chin, LandmarkName::cheek_r});
    // A profile that names the same point twice cannot contribute two
    // independent correspondences; the duplicate backend index is reported.
    profile.landmarks.push_back(profile.landmarks.back());
    const CorrespondenceSet result = build_profile_correspondences(profile, map, set);
    CHECK_EQ(result.usable_count(), 9u);
    CHECK_EQ(result.duplicate_indices.size(), 1u);
    CHECK_EQ(result.duplicate_indices[0], 50u);
    CHECK_EQ(result.duplicate_names.size(), 1u);
    CHECK(result.duplicate_names[0] == LandmarkName::cheek_r);
    CHECK(!result.profile_complete);
    CHECK(result.diagnostic.has_value());
    CHECK(*result.diagnostic == avatar::core::DiagnosticCode::rig_003);
}

TEST_CASE(profile_round_trips_through_json_without_losing_names) {
    ProfileLandmarks profile = make_profile(
        {LandmarkName::eye_outer_r, LandmarkName::eye_inner_r, LandmarkName::eye_outer_l,
         LandmarkName::eye_inner_l, LandmarkName::nose_tip, LandmarkName::mouth_corner_r,
         LandmarkName::mouth_corner_l, LandmarkName::chin, LandmarkName::cheek_r});
    profile.landmarks.push_back(ProfileLandmark{"future_landmark", 0.25f, -0.5f});

    const std::string json = profile.to_json();
    const auto parsed = profile_landmarks_from_json(json);
    CHECK(parsed.profile.has_value());
    CHECK(parsed.error.empty());
    CHECK(parsed.profile->landmarks == profile.landmarks);
    CHECK(parsed.profile->find("future_landmark") != nullptr);

    const std::string full = R"({
        "format": "avatar-studio/face-profile",
        "version": 1,
        "id": "6a1e2f4c-8b3d-4a71-9e55-2c7f0d1a6b84",
        "name": "Default",
        "camera": {"width": 1280, "height": 720},
        "landmarks": [{"name": "eye_outer_r", "u": 0.52, "v": 0.02}],
        "quality": {"inlier_ratio": 0.98}
    })";
    const auto full_parsed = profile_landmarks_from_json(full);
    CHECK(full_parsed.profile.has_value());
    CHECK_EQ(full_parsed.profile->landmarks.size(), 1u);
    CHECK(full_parsed.profile->landmarks[0].name == "eye_outer_r");
}

TEST_CASE(profile_json_rejects_malformed_documents) {
    CHECK(!profile_landmarks_from_json("not json").profile.has_value());
    CHECK(!profile_landmarks_from_json("{}").profile.has_value());
    CHECK(!profile_landmarks_from_json(R"({"format":"other","version":1,"landmarks":[]})")
               .profile.has_value());
    CHECK(!profile_landmarks_from_json(
               R"({"format":"avatar-studio/face-profile","version":2,"landmarks":[]})")
               .profile.has_value());
    CHECK(
        !profile_landmarks_from_json(
             R"({"format":"avatar-studio/face-profile","version":1,"landmarks":[{"name":"chin"}]})")
             .profile.has_value());
}
