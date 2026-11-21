#include "avatar/tracking/landmark_map.h"

#include <algorithm>
#include <cmath>

namespace avatar::tracking {

namespace {

constexpr std::array<const char*, kLandmarkNameCount> kLandmarkNames = {
    "eye_outer_r",
    "eye_inner_r",
    "eye_outer_l",
    "eye_inner_l",
    "iris_center_r",
    "iris_center_l",
    "nose_bridge",
    "nose_tip",
    "nose_left",
    "nose_right",
    "mouth_corner_r",
    "mouth_corner_l",
    "mouth_upper_center",
    "mouth_lower_center",
    "chin",
    "jaw_left",
    "jaw_right",
    "temple_l",
    "temple_r",
    "cheek_l",
    "cheek_r",
    "forehead_center",
};

} // namespace

const char* landmark_name(LandmarkName name) noexcept {
    const auto index = static_cast<std::size_t>(name);
    return index < kLandmarkNameCount ? kLandmarkNames[index] : "unknown";
}

std::optional<LandmarkName> parse_landmark_name(std::string_view name) noexcept {
    for (std::size_t index = 0; index < kLandmarkNameCount; ++index) {
        if (name == kLandmarkNames[index]) {
            return static_cast<LandmarkName>(index);
        }
    }
    return std::nullopt;
}

const char* landmark_backend_name(LandmarkBackend backend) noexcept {
    switch (backend) {
        case LandmarkBackend::mediapipe_face_landmarker:
            return "mediapipe_face_landmarker";
        case LandmarkBackend::onnx_face_mesh_v2:
            return "onnx_face_mesh_v2";
        case LandmarkBackend::custom:
            return "custom";
    }
    return "unknown";
}

std::string_view landmark_table_pin(LandmarkBackend backend) noexcept {
    switch (backend) {
        case LandmarkBackend::mediapipe_face_landmarker:
            return kMediaPipeFaceLandmarkerPin;
        case LandmarkBackend::onnx_face_mesh_v2:
            return kOnnxFaceMeshV2Pin;
        case LandmarkBackend::custom:
            return "custom";
    }
    return "unknown";
}

LandmarkMap::LandmarkMap(std::span<const LandmarkBinding> bindings,
                         std::size_t backend_landmark_count)
    : backend_(LandmarkBackend::custom), pin_(landmark_table_pin(LandmarkBackend::custom)),
      backend_landmark_count_(backend_landmark_count) {
    const std::size_t index_slots = backend_landmark_count_ > 0 ? backend_landmark_count_ : 1;
    std::vector<bool> used(index_slots, false);

    for (const LandmarkBinding& binding : bindings) {
        const auto name_index = static_cast<std::size_t>(binding.name);
        if (name_index >= kLandmarkNameCount || known_[name_index]) {
            continue;
        }
        known_[name_index] = true;

        if (binding.index == kNoLandmarkIndex || binding.index >= backend_landmark_count_) {
            continue;
        }
        if (used[binding.index]) {
            duplicate_names_.push_back(binding.name);
            duplicate_indices_.push_back(binding.index);
            continue;
        }
        used[binding.index] = true;
        indices_[name_index] = binding.index;
    }
}

LandmarkMap LandmarkMap::mediapipe() {
    return for_backend(LandmarkBackend::mediapipe_face_landmarker);
}

LandmarkMap LandmarkMap::onnx() {
    return for_backend(LandmarkBackend::onnx_face_mesh_v2);
}

LandmarkMap LandmarkMap::for_backend(LandmarkBackend backend) {
    LandmarkMap map = [&]() {
        switch (backend) {
            case LandmarkBackend::mediapipe_face_landmarker:
                return LandmarkMap(kMediaPipeFaceLandmarkerTable);
            case LandmarkBackend::onnx_face_mesh_v2:
                return LandmarkMap(kOnnxFaceMeshV2Table);
            case LandmarkBackend::custom:
                return LandmarkMap(std::span<const LandmarkBinding>{});
        }
        return LandmarkMap(std::span<const LandmarkBinding>{});
    }();
    map.backend_ = backend;
    map.pin_ = landmark_table_pin(backend);
    return map;
}

bool LandmarkMap::knows(LandmarkName name) const noexcept {
    const auto index = static_cast<std::size_t>(name);
    return index < kLandmarkNameCount && known_[index];
}

std::optional<std::size_t> LandmarkMap::resolve(LandmarkName name) const noexcept {
    const auto index = static_cast<std::size_t>(name);
    if (index >= kLandmarkNameCount || !indices_[index].has_value()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(*indices_[index]);
}

std::optional<std::size_t> LandmarkMap::resolve(std::string_view name) const noexcept {
    const std::optional<LandmarkName> parsed = parse_landmark_name(name);
    return parsed.has_value() ? resolve(*parsed) : std::nullopt;
}

std::optional<Landmark> LandmarkMap::point(const LandmarkSet& landmarks,
                                           LandmarkName name) const noexcept {
    const std::optional<std::size_t> index = resolve(name);
    if (!index.has_value()) {
        return std::nullopt;
    }
    const Landmark* landmark = landmarks.at(*index);
    if (landmark == nullptr) {
        return std::nullopt;
    }
    return *landmark;
}

bool LandmarkMap::usable(const LandmarkSet& landmarks, LandmarkName name) const noexcept {
    const std::optional<Landmark> landmark = point(landmarks, name);
    if (!landmark.has_value()) {
        return false;
    }
    return std::isfinite(landmark->x) && std::isfinite(landmark->y) && std::isfinite(landmark->z) &&
           landmark->x > 0.0f && landmark->x < 1.0f && landmark->y > 0.0f && landmark->y < 1.0f;
}

std::size_t LandmarkMap::resolved_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        indices_.begin(), indices_.end(), [](const auto& index) { return index.has_value(); }));
}

} // namespace avatar::tracking
