#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "avatar/core/clock.h"

namespace avatar::tracking {

using core::TimestampNs;

/// Head pose in camera space: right-handed, -Z forward, metres.
/// `rotation` is a unit quaternion in [x, y, z, w] order, matching the package
/// format convention.
struct HeadPose {
    std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 3> translation{0.0f, 0.0f, 0.0f};

    [[nodiscard]] bool operator==(const HeadPose&) const = default;
};

/// One landmark in normalised image coordinates plus relative depth.
struct Landmark {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    [[nodiscard]] bool operator==(const Landmark&) const = default;
};

/// The dense landmark set a real backend fills. A scripted backend may use
/// fewer points; `count` is the number of valid entries.
struct LandmarkSet {
    static constexpr std::size_t kCapacity = 478;

    std::array<Landmark, kCapacity> points{};
    std::size_t count = 0;

    [[nodiscard]] Landmark* at(std::size_t index) noexcept {
        return index < count && index < kCapacity ? &points[index] : nullptr;
    }
    [[nodiscard]] const Landmark* at(std::size_t index) const noexcept {
        return index < count && index < kCapacity ? &points[index] : nullptr;
    }

    [[nodiscard]] bool operator==(const LandmarkSet&) const = default;
};

struct BlendshapeSet {
    static constexpr std::size_t kCount = 52;
    std::array<float, kCount> coefficients{};

    [[nodiscard]] bool operator==(const BlendshapeSet&) const = default;
};

/// The contract every tracker backend must satisfy (docs/design/tracking.md).
struct TrackingResult {
    TimestampNs source_timestamp_ns = 0;
    bool valid = false;
    bool features_valid = false;
    float confidence = 0.0f;
    HeadPose pose;
    LandmarkSet landmarks;
    BlendshapeSet blendshapes;

    [[nodiscard]] bool operator==(const TrackingResult&) const = default;
};

struct TrackerStats {
    std::uint64_t submitted = 0;
    std::uint64_t produced = 0;
    std::uint64_t dropped = 0;
};

} // namespace avatar::tracking
