#include "avatar/capture/camera_device.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace avatar::capture {

namespace {

// Win32 and Media Foundation HRESULTs. They are stable public API; spelling
// them out keeps this translation unit free of Windows headers so the mapping
// is testable on every platform.
constexpr std::int32_t kAccessDenied = static_cast<std::int32_t>(0x80070005u);
constexpr std::int32_t kFileNotFound = static_cast<std::int32_t>(0x80070002u);
constexpr std::int32_t kNotReady = static_cast<std::int32_t>(0x80070015u);
constexpr std::int32_t kGenFailure = static_cast<std::int32_t>(0x8007001Fu);
constexpr std::int32_t kDeviceNotConnected = static_cast<std::int32_t>(0x8007048Fu);
constexpr std::int32_t kNotFound = static_cast<std::int32_t>(0x80070490u);
constexpr std::int32_t kErrorBusy = static_cast<std::int32_t>(0x800700AAu);
constexpr std::int32_t kNoSystemResources = static_cast<std::int32_t>(0x800705AAu);
constexpr std::int32_t kMfInvalidMediaType = static_cast<std::int32_t>(0xC00D36B4u);
constexpr std::int32_t kMfUnsupportedBytestream = static_cast<std::int32_t>(0xC00D36C4u);
constexpr std::int32_t kMfAttributeNotFound = static_cast<std::int32_t>(0xC00D36E6u);
constexpr std::int32_t kMfShutdown = static_cast<std::int32_t>(0xC00D3E85u);
constexpr std::int32_t kMfHwMftFailedStartStreaming = static_cast<std::int32_t>(0xC00D3704u);
constexpr std::int32_t kMfRecordingDeviceInvalidated = static_cast<std::int32_t>(0xC00D3EA2u);
constexpr std::int32_t kMfRecordingDevicePreempted = static_cast<std::int32_t>(0xC00D3EA3u);

struct ResolutionPolicy {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t fps;
};

constexpr std::array<ResolutionPolicy, 4> kResolutionPolicy = {{
    {1280, 720, 60},
    {1920, 1080, 30},
    {1920, 1080, 60},
    {640, 480, 30},
}};

// MJPEG is missing on purpose: FrameRing owns fixed-size CPU formats, so a
// compressed mode needs a decoder before it can be published.
constexpr std::array<PixelFormat, 3> kFormatPolicy = {{
    PixelFormat::nv12,
    PixelFormat::yuy2,
    PixelFormat::bgra8,
}};

bool contains_mode(const std::vector<CameraMode>& modes, const CameraMode& mode) {
    return std::any_of(modes.begin(), modes.end(),
                       [&mode](const CameraMode& candidate) { return candidate == mode; });
}

double fps_distance(const CameraMode& left, const CameraMode& right) {
    return std::abs(left.fps() - right.fps());
}

} // namespace

double CameraMode::fps() const noexcept {
    if (fps_denominator == 0) {
        return 0.0;
    }
    return static_cast<double>(fps_numerator) / static_cast<double>(fps_denominator);
}

bool operator==(const CameraMode& left, const CameraMode& right) noexcept {
    return left.width == right.width && left.height == right.height &&
           left.fps_numerator == right.fps_numerator &&
           left.fps_denominator == right.fps_denominator && left.format == right.format;
}

std::vector<CameraMode> default_camera_preferences() {
    std::vector<CameraMode> modes;
    modes.reserve(kFormatPolicy.size() * kResolutionPolicy.size());
    for (const PixelFormat format : kFormatPolicy) {
        for (const ResolutionPolicy& resolution : kResolutionPolicy) {
            CameraMode mode;
            mode.width = resolution.width;
            mode.height = resolution.height;
            mode.fps_numerator = resolution.fps;
            mode.fps_denominator = 1;
            mode.format = format;
            modes.push_back(mode);
        }
    }
    return modes;
}

std::string_view camera_failure_reason_name(CameraFailureReason reason) noexcept {
    switch (reason) {
        case CameraFailureReason::none:
            return "none";
        case CameraFailureReason::invalid_format:
            return "invalid_format";
        case CameraFailureReason::busy:
            return "busy";
        case CameraFailureReason::permission_denied:
            return "permission_denied";
        case CameraFailureReason::device_lost:
            return "device_lost";
        case CameraFailureReason::device_gone:
            return "device_gone";
        case CameraFailureReason::unsupported:
            return "unsupported";
        case CameraFailureReason::internal:
            return "internal";
    }
    return "unknown";
}

CameraFailureReason classify_camera_hresult(std::int32_t hresult) noexcept {
    switch (hresult) {
        case kAccessDenied:
            return CameraFailureReason::permission_denied;
        case kFileNotFound:
        case kDeviceNotConnected:
        case kNotFound:
        case kMfAttributeNotFound:
            return CameraFailureReason::device_gone;
        case kNotReady:
        case kGenFailure:
        case kErrorBusy:
        case kNoSystemResources:
        case kMfHwMftFailedStartStreaming:
        case kMfRecordingDevicePreempted:
            // Many cameras report exclusive access as the generic
            // ERROR_GEN_FAILURE rather than ERROR_BUSY.
            return CameraFailureReason::busy;
        case kMfInvalidMediaType:
        case kMfUnsupportedBytestream:
            return CameraFailureReason::invalid_format;
        case kMfShutdown:
        case kMfRecordingDeviceInvalidated:
            return CameraFailureReason::device_lost;
        default:
            break;
    }
    return CameraFailureReason::internal;
}

bool camera_mode_matches(const CameraMode& left, const CameraMode& right,
                         double fps_tolerance) noexcept {
    if (left.width != right.width || left.height != right.height || left.format != right.format) {
        return false;
    }
    if (left.fps() == 0.0 || right.fps() == 0.0) {
        return true;
    }
    return fps_distance(left, right) <= fps_tolerance;
}

std::vector<CameraMode> select_camera_modes(const CameraCapabilities& capabilities,
                                            const std::vector<CameraMode>& preferences) {
    std::vector<CameraMode> selected;
    if (preferences.empty()) {
        return selected;
    }

    // A device that could not answer is attempted with the preference list as
    // written; the open call is the real negotiation then.
    if (capabilities.empty()) {
        for (const CameraMode& preference : preferences) {
            if (is_supported_pixel_format(preference.format) &&
                !contains_mode(selected, preference)) {
                selected.push_back(preference);
            }
        }
        return selected;
    }

    for (const CameraMode& preference : preferences) {
        for (const CameraMode& advertised : capabilities.modes) {
            if (camera_mode_matches(preference, advertised) &&
                !contains_mode(selected, advertised)) {
                selected.push_back(advertised);
            }
        }
    }
    if (!selected.empty()) {
        return selected;
    }

    // No exact preferred mode. Return the closest advertised mode for each
    // supported format, best format first, so the caller opens something and
    // reports CAP-003 instead of failing outright.
    for (const PixelFormat format : kFormatPolicy) {
        const CameraMode* best = nullptr;
        double best_score = std::numeric_limits<double>::max();
        for (const CameraMode& advertised : capabilities.modes) {
            if (advertised.format != format) {
                continue;
            }
            double score = std::numeric_limits<double>::max();
            for (const CameraMode& preference : preferences) {
                if (preference.format != format) {
                    continue;
                }
                const double resolution_distance = std::abs(static_cast<double>(advertised.width) -
                                                            static_cast<double>(preference.width)) +
                                                   std::abs(static_cast<double>(advertised.height) -
                                                            static_cast<double>(preference.height));
                const double candidate_score =
                    resolution_distance + 10.0 * fps_distance(advertised, preference);
                score = std::min(score, candidate_score);
            }
            if (score < best_score) {
                best_score = score;
                best = &advertised;
            }
        }
        if (best != nullptr && !contains_mode(selected, *best)) {
            selected.push_back(*best);
        }
    }
    return selected;
}

} // namespace avatar::capture
