#include "avatar/capture/face_ground_truth.h"

namespace avatar::capture {

namespace {

struct LandmarkName {
    GroundTruthLandmark id;
    const char* name;
};

constexpr std::array<LandmarkName, kGroundTruthLandmarkCount> kNames{{
    {GroundTruthLandmark::eye_outer_r, "eye_outer_r"},
    {GroundTruthLandmark::eye_inner_r, "eye_inner_r"},
    {GroundTruthLandmark::iris_center_r, "iris_center_r"},
    {GroundTruthLandmark::eye_outer_l, "eye_outer_l"},
    {GroundTruthLandmark::eye_inner_l, "eye_inner_l"},
    {GroundTruthLandmark::iris_center_l, "iris_center_l"},
    {GroundTruthLandmark::nose_bridge, "nose_bridge"},
    {GroundTruthLandmark::nose_tip, "nose_tip"},
    {GroundTruthLandmark::nose_left, "nose_left"},
    {GroundTruthLandmark::nose_right, "nose_right"},
    {GroundTruthLandmark::mouth_corner_r, "mouth_corner_r"},
    {GroundTruthLandmark::mouth_corner_l, "mouth_corner_l"},
    {GroundTruthLandmark::mouth_upper_center, "mouth_upper_center"},
    {GroundTruthLandmark::mouth_lower_center, "mouth_lower_center"},
    {GroundTruthLandmark::chin, "chin"},
    {GroundTruthLandmark::jaw_left, "jaw_left"},
    {GroundTruthLandmark::jaw_right, "jaw_right"},
    {GroundTruthLandmark::temple_l, "temple_l"},
    {GroundTruthLandmark::temple_r, "temple_r"},
    {GroundTruthLandmark::cheek_l, "cheek_l"},
    {GroundTruthLandmark::cheek_r, "cheek_r"},
    {GroundTruthLandmark::forehead_center, "forehead_center"},
    {GroundTruthLandmark::marker_right_cheek, "marker_right_cheek"},
}};

} // namespace

const char* ground_truth_landmark_name(GroundTruthLandmark landmark) noexcept {
    const auto index = static_cast<std::size_t>(landmark);
    if (index >= kNames.size()) {
        return "unknown";
    }
    return kNames[index].name;
}

bool parse_ground_truth_landmark(std::string_view name, GroundTruthLandmark& out) noexcept {
    for (const LandmarkName& entry : kNames) {
        if (name == entry.name) {
            out = entry.id;
            return true;
        }
    }
    return false;
}

} // namespace avatar::capture
