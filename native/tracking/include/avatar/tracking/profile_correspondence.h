#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "avatar/core/diagnostics.h"
#include "avatar/tracking/landmark_map.h"
#include "avatar/tracking/tracking_result.h"

namespace avatar::tracking {

/// The homography needs four points, but fewer than eight stable
/// correspondences is a poor profile and is not accepted for sampling. T18's
/// RANSAC inlier gate is a separate, later check.
inline constexpr std::size_t kMinimumSamplingCorrespondences = 8;

struct ProfileLandmark {
    std::string name;
    float u = 0.0f;
    float v = 0.0f;

    [[nodiscard]] bool operator==(const ProfileLandmark&) const = default;
};

/// The named landmark subset of a FaceProfile (docs/design/user-data.md).
///
/// T31 owns the full profile document, capture and storage. T12 owns the names,
/// canonical values and the JSON round-trip of this subset, so a profile can be
/// written by one build and read by a backend with a different index table.
struct ProfileLandmarks {
    static constexpr std::string_view kFormat = "avatar-studio/face-profile";
    static constexpr int kVersion = 1;

    std::vector<ProfileLandmark> landmarks;

    [[nodiscard]] const ProfileLandmark* find(std::string_view name) const noexcept;
    [[nodiscard]] std::string to_json() const;
};

struct ProfileParseResult {
    std::optional<ProfileLandmarks> profile;
    std::string error;
};

/// Parses the `format`, `version` and `landmarks` members of a FaceProfile.
/// Other members are ignored here; T31 extends the document. A malformed
/// document returns `profile == nullopt` and a non-empty error.
[[nodiscard]] ProfileParseResult profile_landmarks_from_json(std::string_view text);

struct ProfileCorrespondence {
    LandmarkName name = LandmarkName::count;
    std::size_t backend_index = 0;
    float u = 0.0f;
    float v = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
};

/// The result of joining a profile to a live tracking result.
///
/// Names the profile does not contain are additive: an older profile simply
/// contributes fewer correspondences. Names the profile contains but this
/// backend cannot provide are reported in `missing_names`; the solver uses the
/// remaining subset and the caller raises `RIG-003`. Unknown names (not in the
/// stable anatomical set) are ignored.
struct CorrespondenceSet {
    std::vector<ProfileCorrespondence> correspondences;
    std::vector<std::string> missing_names;
    std::vector<std::string> ignored_names;
    std::vector<LandmarkName> duplicate_names;
    std::vector<std::size_t> duplicate_indices;
    bool profile_complete = true;
    bool sampling_supported = false;
    std::optional<core::DiagnosticCode> diagnostic;

    [[nodiscard]] std::size_t usable_count() const noexcept { return correspondences.size(); }
};

[[nodiscard]] CorrespondenceSet build_profile_correspondences(const ProfileLandmarks& profile,
                                                              const LandmarkMap& map,
                                                              const LandmarkSet& landmarks);

} // namespace avatar::tracking
