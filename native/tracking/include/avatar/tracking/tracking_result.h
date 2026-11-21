#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "avatar/core/clock.h"
#include "avatar/core/latency_histogram.h"

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

/// Why a submitted frame did not produce a result.
///
/// The counters are separate fields rather than a map so a backend can update
/// one in the hot path without allocating, and so a test can assert the exact
/// reason. `backend_busy` is the real backend's normal drop path: the inference
/// thread already has a frame, so the new one is discarded rather than queued.
enum class DropReason : std::uint8_t {
    none = 0,
    script_exhausted,
    backend_busy,
    invalid_frame,
    not_initialized,
    shutdown,
};

[[nodiscard]] inline const char* drop_reason_name(DropReason reason) noexcept {
    switch (reason) {
        case DropReason::none:
            return "none";
        case DropReason::script_exhausted:
            return "script_exhausted";
        case DropReason::backend_busy:
            return "backend_busy";
        case DropReason::invalid_frame:
            return "invalid_frame";
        case DropReason::not_initialized:
            return "not_initialized";
        case DropReason::shutdown:
            return "shutdown";
    }
    return "unknown";
}

/// Ten equal-width bins over the confidence range [0, 1].
struct ConfidenceDistribution {
    static constexpr std::size_t kBucketCount = 10;

    std::array<std::uint64_t, kBucketCount> counts{};
    std::uint64_t samples = 0;
    double total = 0.0;

    void add(float confidence) noexcept {
        float clamped = confidence;
        if (!(clamped >= 0.0f)) {
            clamped = 0.0f;
        } else if (clamped > 1.0f) {
            clamped = 1.0f;
        }
        std::size_t index = static_cast<std::size_t>(clamped * 10.0f);
        if (index >= kBucketCount) {
            index = kBucketCount - 1;
        }
        ++counts[index];
        ++samples;
        total += static_cast<double>(clamped);
    }

    [[nodiscard]] std::uint64_t bucket(std::size_t index) const noexcept {
        return index < kBucketCount ? counts[index] : 0;
    }

    [[nodiscard]] double mean() const noexcept {
        return samples == 0 ? 0.0 : total / static_cast<double>(samples);
    }
};

/// Per-session tracker instrumentation (docs/design/tracking.md#instrumentation).
///
/// `inference_ns` is empty for a backend that does no inference (the scripted
/// tracker); a real backend records one sample per produced result. The
/// histogram is in the core so the performance harness measures the same shape.
struct TrackerStats {
    std::uint64_t submitted = 0;
    std::uint64_t produced = 0;
    std::uint64_t dropped = 0;
    std::uint64_t dropped_script_exhausted = 0;
    std::uint64_t dropped_backend_busy = 0;
    std::uint64_t dropped_invalid_frame = 0;
    std::uint64_t dropped_not_initialized = 0;
    std::uint64_t dropped_shutdown = 0;
    std::uint32_t input_width = 0;
    std::uint32_t input_height = 0;
    /// Inference latency. Read `p50_ns()` / `p95_ns()` / `p99_ns()`; empty for
    /// a backend that does no inference (the scripted tracker).
    core::TrackerLatencyHistogram inference_ns;
    ConfidenceDistribution confidence;
    DropReason last_drop_reason = DropReason::none;

    void record_result(float result_confidence) noexcept {
        ++produced;
        confidence.add(result_confidence);
    }

    void record_inference(std::uint64_t elapsed_ns) noexcept { inference_ns.add(elapsed_ns); }

    [[nodiscard]] std::uint64_t inference_p50_ns() const noexcept { return inference_ns.p50_ns(); }
    [[nodiscard]] std::uint64_t inference_p95_ns() const noexcept { return inference_ns.p95_ns(); }

    void record_drop(DropReason reason) noexcept {
        ++dropped;
        last_drop_reason = reason;
        switch (reason) {
            case DropReason::script_exhausted:
                ++dropped_script_exhausted;
                break;
            case DropReason::backend_busy:
                ++dropped_backend_busy;
                break;
            case DropReason::invalid_frame:
                ++dropped_invalid_frame;
                break;
            case DropReason::not_initialized:
                ++dropped_not_initialized;
                break;
            case DropReason::shutdown:
                ++dropped_shutdown;
                break;
            case DropReason::none:
                break;
        }
    }
};

} // namespace avatar::tracking
