#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "avatar/tracking/tracking_result.h"

namespace avatar::tracking {

/// Stable anatomical names for the correspondence set in docs/design/tracking.md.
///
/// `_l` and `_r` are always the subject's left and right, never the viewer's. The
/// rig and the FaceProfile speak these names; a backend index never leaves the
/// adapter that owns it.
enum class LandmarkName : std::uint8_t {
    eye_outer_r = 0,
    eye_inner_r,
    eye_outer_l,
    eye_inner_l,
    iris_center_r,
    iris_center_l,
    nose_bridge,
    nose_tip,
    nose_left,
    nose_right,
    mouth_corner_r,
    mouth_corner_l,
    mouth_upper_center,
    mouth_lower_center,
    chin,
    jaw_left,
    jaw_right,
    temple_l,
    temple_r,
    cheek_l,
    cheek_r,
    forehead_center,
    count,
};

inline constexpr std::size_t kLandmarkNameCount = static_cast<std::size_t>(LandmarkName::count);

[[nodiscard]] const char* landmark_name(LandmarkName name) noexcept;
[[nodiscard]] std::optional<LandmarkName> parse_landmark_name(std::string_view name) noexcept;

/// A backend whose landmark topology is pinned. `custom` exists for tests and
/// for a future adapter that has not been added to this enum yet.
enum class LandmarkBackend : std::uint8_t {
    mediapipe_face_landmarker = 0,
    onnx_face_mesh_v2,
    custom,
};

[[nodiscard]] const char* landmark_backend_name(LandmarkBackend backend) noexcept;

/// The exact model/topology the table was pinned against. Recorded in progress
/// notes and tests so a model update cannot silently change the mapping.
[[nodiscard]] std::string_view landmark_table_pin(LandmarkBackend backend) noexcept;

/// Sentinel for a name the backend knows but cannot provide (for example an
/// iris name on a model without iris output). It is not an index and must never
/// be used to read a landmark.
inline constexpr std::uint16_t kNoLandmarkIndex = 0xFFFF;

struct LandmarkBinding {
    LandmarkName name = LandmarkName::count;
    std::uint16_t index = kNoLandmarkIndex;

    [[nodiscard]] bool operator==(const LandmarkBinding&) const = default;
};

/// Exact pins the two tables were reviewed against. A model or conversion
/// change must update the pin and rerun the asymmetric-marker test.
inline constexpr std::string_view kMediaPipeFaceLandmarkerPin =
    "mediapipe-0.10.21/face_landmarker/float16/1/478";
inline constexpr std::string_view kOnnxFaceMeshV2Pin =
    "senty-au/face_landmarks_detector-ONNX@337d58218b5b1cc597ca3c67360880b920f6ce7b/478";

/// Pinned MediaPipe Face Mesh V2 / Face Landmarker 478-point topology.
///
/// The real Windows backend is MediaPipe 0.10.21 with the
/// `face_landmarker/face_landmarker/float16/1` bundle (T11). The feature gate in
/// `native/tracking/mediapipe/preprocessing.cpp` must agree with these indices.
inline constexpr std::array<LandmarkBinding, kLandmarkNameCount> kMediaPipeFaceLandmarkerTable = {{
    {LandmarkName::eye_outer_r, 33},
    {LandmarkName::eye_inner_r, 133},
    {LandmarkName::eye_outer_l, 263},
    {LandmarkName::eye_inner_l, 362},
    {LandmarkName::iris_center_r, 468},
    {LandmarkName::iris_center_l, 473},
    {LandmarkName::nose_bridge, 168},
    {LandmarkName::nose_tip, 1},
    {LandmarkName::nose_left, 358},
    {LandmarkName::nose_right, 129},
    {LandmarkName::mouth_corner_r, 61},
    {LandmarkName::mouth_corner_l, 291},
    {LandmarkName::mouth_upper_center, 13},
    {LandmarkName::mouth_lower_center, 14},
    {LandmarkName::chin, 152},
    {LandmarkName::jaw_left, 454},
    {LandmarkName::jaw_right, 234},
    {LandmarkName::temple_l, 356},
    {LandmarkName::temple_r, 127},
    {LandmarkName::cheek_l, 280},
    {LandmarkName::cheek_r, 50},
    {LandmarkName::forehead_center, 10},
}};

/// Pinned table for the evaluated ONNX Face Mesh V2 conversion
/// (`senty-au/face_landmarks_detector-ONNX` at commit
/// `337d58218b5b1cc597ca3c67360880b920f6ce7b`, 478 points). The conversion
/// preserves MediaPipe's point order, but it is a separate table on purpose: an
/// ONNX adapter must opt into the topology it actually runs, not inherit the
/// MediaPipe table by accident.
inline constexpr std::array<LandmarkBinding, kLandmarkNameCount> kOnnxFaceMeshV2Table = {{
    {LandmarkName::eye_outer_r, 33},
    {LandmarkName::eye_inner_r, 133},
    {LandmarkName::eye_outer_l, 263},
    {LandmarkName::eye_inner_l, 362},
    {LandmarkName::iris_center_r, 468},
    {LandmarkName::iris_center_l, 473},
    {LandmarkName::nose_bridge, 168},
    {LandmarkName::nose_tip, 1},
    {LandmarkName::nose_left, 358},
    {LandmarkName::nose_right, 129},
    {LandmarkName::mouth_corner_r, 61},
    {LandmarkName::mouth_corner_l, 291},
    {LandmarkName::mouth_upper_center, 13},
    {LandmarkName::mouth_lower_center, 14},
    {LandmarkName::chin, 152},
    {LandmarkName::jaw_left, 454},
    {LandmarkName::jaw_right, 234},
    {LandmarkName::temple_l, 356},
    {LandmarkName::temple_r, 127},
    {LandmarkName::cheek_l, 280},
    {LandmarkName::cheek_r, 50},
    {LandmarkName::forehead_center, 10},
}};

/// The named correspondence set. It resolves anatomical names to the active
/// backend's indices and never invents an index for a name it cannot resolve.
///
/// Construction is intentionally small and allocation-light: a backend adapter
/// builds one map and keeps it for the session.
class LandmarkMap {
public:
    static constexpr std::size_t kDefaultLandmarkCount = LandmarkSet::kCapacity;

    /// The production tables. `for_backend` is what a future adapter calls.
    [[nodiscard]] static LandmarkMap mediapipe();
    [[nodiscard]] static LandmarkMap onnx();
    [[nodiscard]] static LandmarkMap for_backend(LandmarkBackend backend);

    /// Builds a map from an explicit table. A binding with `kNoLandmarkIndex`
    /// records that the backend knows the name but cannot provide it. The first
    /// binding for a name wins; a second use of the same index is reported as a
    /// duplicate and ignored.
    explicit LandmarkMap(std::span<const LandmarkBinding> bindings,
                         std::size_t backend_landmark_count = kDefaultLandmarkCount);

    [[nodiscard]] LandmarkBackend backend() const noexcept { return backend_; }
    [[nodiscard]] std::string_view table_pin() const noexcept { return pin_; }
    [[nodiscard]] std::size_t backend_landmark_count() const noexcept {
        return backend_landmark_count_;
    }

    /// True when the table contains this name, even if the backend cannot
    /// provide a point for it. `resolve` is the availability check.
    [[nodiscard]] bool knows(LandmarkName name) const noexcept;

    [[nodiscard]] std::optional<std::size_t> resolve(LandmarkName name) const noexcept;

    /// Unknown names return nullopt and are ignored by callers. This is the
    /// forward-compatible path for a profile written by a newer build.
    [[nodiscard]] std::optional<std::size_t> resolve(std::string_view name) const noexcept;

    /// Returns the backend point when the index is inside `landmarks.count`.
    /// It does not test whether the point is finite or inside the image.
    [[nodiscard]] std::optional<Landmark> point(const LandmarkSet& landmarks,
                                                LandmarkName name) const noexcept;

    /// True when the point exists, is finite and lies strictly inside the
    /// normalised image. A zeroed/missing backend entry is not usable.
    [[nodiscard]] bool usable(const LandmarkSet& landmarks, LandmarkName name) const noexcept;

    [[nodiscard]] std::size_t resolved_count() const noexcept;
    [[nodiscard]] bool has_duplicate_indices() const noexcept {
        return !duplicate_indices_.empty();
    }
    [[nodiscard]] std::span<const LandmarkName> duplicate_names() const noexcept {
        return duplicate_names_;
    }
    [[nodiscard]] std::span<const std::uint16_t> duplicate_indices() const noexcept {
        return duplicate_indices_;
    }

private:
    LandmarkBackend backend_ = LandmarkBackend::custom;
    std::string_view pin_{};
    std::size_t backend_landmark_count_ = kDefaultLandmarkCount;
    std::array<bool, kLandmarkNameCount> known_{};
    std::array<std::optional<std::uint16_t>, kLandmarkNameCount> indices_{};
    std::vector<LandmarkName> duplicate_names_;
    std::vector<std::uint16_t> duplicate_indices_;
};

} // namespace avatar::tracking
