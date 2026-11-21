#include "avatar/tracking/profile_correspondence.h"

#include <cmath>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace avatar::tracking {

const ProfileLandmark* ProfileLandmarks::find(std::string_view name) const noexcept {
    for (const ProfileLandmark& landmark : landmarks) {
        if (landmark.name == name) {
            return &landmark;
        }
    }
    return nullptr;
}

std::string ProfileLandmarks::to_json() const {
    nlohmann::json document;
    document["format"] = kFormat;
    document["version"] = kVersion;
    document["landmarks"] = nlohmann::json::array();
    for (const ProfileLandmark& landmark : landmarks) {
        document["landmarks"].push_back(
            {{"name", landmark.name}, {"u", landmark.u}, {"v", landmark.v}});
    }
    return document.dump(2);
}

ProfileParseResult profile_landmarks_from_json(std::string_view text) {
    ProfileParseResult result;
    const nlohmann::json document = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        result.error = "profile is not a JSON object";
        return result;
    }
    if (!document.contains("format") || !document["format"].is_string() ||
        document["format"].get<std::string>() != ProfileLandmarks::kFormat) {
        result.error = "profile format is not " + std::string(ProfileLandmarks::kFormat);
        return result;
    }
    if (!document.contains("version") || !document["version"].is_number_integer() ||
        document["version"].get<int>() != ProfileLandmarks::kVersion) {
        result.error = "profile version is not " + std::to_string(ProfileLandmarks::kVersion);
        return result;
    }
    if (!document.contains("landmarks") || !document["landmarks"].is_array()) {
        result.error = "profile landmarks is not an array";
        return result;
    }

    ProfileLandmarks profile;
    profile.landmarks.reserve(document["landmarks"].size());
    for (const nlohmann::json& item : document["landmarks"]) {
        if (!item.is_object() || !item.contains("name") || !item["name"].is_string() ||
            !item.contains("u") || !item["u"].is_number() || !item.contains("v") ||
            !item["v"].is_number()) {
            result.error = "profile landmark entry is malformed";
            return result;
        }
        ProfileLandmark landmark;
        landmark.name = item["name"].get<std::string>();
        landmark.u = item["u"].get<float>();
        landmark.v = item["v"].get<float>();
        if (!std::isfinite(landmark.u) || !std::isfinite(landmark.v)) {
            result.error = "profile landmark values are not finite";
            return result;
        }
        profile.landmarks.push_back(std::move(landmark));
    }

    result.profile = std::move(profile);
    return result;
}

CorrespondenceSet build_profile_correspondences(const ProfileLandmarks& profile,
                                                const LandmarkMap& map,
                                                const LandmarkSet& landmarks) {
    CorrespondenceSet result;
    std::vector<bool> used(map.backend_landmark_count(), false);

    for (const ProfileLandmark& entry : profile.landmarks) {
        const std::optional<LandmarkName> name = parse_landmark_name(entry.name);
        if (!name.has_value()) {
            result.ignored_names.push_back(entry.name);
            continue;
        }

        const std::optional<std::size_t> index = map.resolve(*name);
        if (!index.has_value() || !map.usable(landmarks, *name)) {
            result.missing_names.push_back(entry.name);
            continue;
        }
        if (*index < used.size() && used[*index]) {
            result.duplicate_names.push_back(*name);
            result.duplicate_indices.push_back(*index);
            continue;
        }
        if (*index < used.size()) {
            used[*index] = true;
        }

        const Landmark point = *map.point(landmarks, *name);
        result.correspondences.push_back(
            ProfileCorrespondence{*name, *index, entry.u, entry.v, point.x, point.y});
    }

    result.profile_complete = result.missing_names.empty() && result.duplicate_indices.empty();
    result.sampling_supported = result.correspondences.size() >= kMinimumSamplingCorrespondences;
    if (!result.profile_complete || !result.sampling_supported) {
        result.diagnostic = core::DiagnosticCode::rig_003;
    }
    return result;
}

} // namespace avatar::tracking
