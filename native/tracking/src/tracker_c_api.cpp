#include "avatar/tracker/tracker_c_api.h"

#include <cstring>
#include <new>
#include <optional>
#include <stdexcept>

#include "avatar/tracking/scripted_tracker.h"
#include "avatar/tracking/tracking_result.h"

namespace {

using avatar::capture::PixelFormat;
using avatar::tracking::ScriptedScenario;
using avatar::tracking::ScriptedTracker;
using avatar::tracking::ScriptedTrackerConfig;
using avatar::tracking::TrackerConfig;
using avatar::tracking::TrackingResult;

void set_error(avatar_tracker_error* out_error, avatar_tracker_error value) {
    if (out_error != nullptr) {
        *out_error = value;
    }
}

bool scenario_from_c(std::uint32_t value, ScriptedScenario& out) noexcept {
    switch (value) {
        case AVATAR_TRACKER_SCENARIO_VALID_TRACKING:
            out = ScriptedScenario::valid_tracking;
            return true;
        case AVATAR_TRACKER_SCENARIO_DROPOUTS:
            out = ScriptedScenario::dropouts;
            return true;
        case AVATAR_TRACKER_SCENARIO_JITTER:
            out = ScriptedScenario::jitter;
            return true;
        case AVATAR_TRACKER_SCENARIO_OUTLIERS:
            out = ScriptedScenario::outliers;
            return true;
        case AVATAR_TRACKER_SCENARIO_MISSING_MOUTH:
            out = ScriptedScenario::missing_mouth;
            return true;
        case AVATAR_TRACKER_SCENARIO_LEFT_RIGHT_SWAP:
            out = ScriptedScenario::left_right_swap;
            return true;
        default:
            return false;
    }
}

bool format_from_c(std::uint32_t value, PixelFormat& out) noexcept {
    switch (value) {
        case AVATAR_TRACKER_PIXEL_FORMAT_NV12:
            out = PixelFormat::nv12;
            return true;
        case AVATAR_TRACKER_PIXEL_FORMAT_YUY2:
            out = PixelFormat::yuy2;
            return true;
        case AVATAR_TRACKER_PIXEL_FORMAT_MJPEG:
            out = PixelFormat::mjpeg;
            return true;
        case AVATAR_TRACKER_PIXEL_FORMAT_BGRA8:
            out = PixelFormat::bgra8;
            return true;
        case AVATAR_TRACKER_PIXEL_FORMAT_RGBA8:
            out = PixelFormat::rgba8;
            return true;
        default:
            return false;
    }
}

avatar_tracker_error to_scripted_config(const avatar_tracker_config& in,
                                        ScriptedTrackerConfig& out) noexcept {
    if (in.abi_version != AVATAR_TRACKER_ABI_VERSION) {
        return AVATAR_TRACKER_ERROR_ABI_MISMATCH;
    }
    if (in.reserved != 0) {
        return AVATAR_TRACKER_ERROR_INVALID_CONFIG;
    }
    if (!scenario_from_c(in.scenario, out.scenario)) {
        return AVATAR_TRACKER_ERROR_INVALID_CONFIG;
    }
    if (in.frame_count == 0 || in.frame_interval_ns == 0) {
        return AVATAR_TRACKER_ERROR_INVALID_CONFIG;
    }
    if (!(in.base_confidence >= 0.0f && in.base_confidence <= 1.0f)) {
        return AVATAR_TRACKER_ERROR_INVALID_CONFIG;
    }
    if ((in.input_width == 0) != (in.input_height == 0)) {
        return AVATAR_TRACKER_ERROR_INVALID_CONFIG;
    }
    if (in.simulate_fault > AVATAR_TRACKER_FAULT_POLL) {
        return AVATAR_TRACKER_ERROR_INVALID_CONFIG;
    }
    out.frame_count = in.frame_count;
    out.seed = in.seed;
    out.base_confidence = in.base_confidence;
    out.frame_interval_ns = in.frame_interval_ns;
    return AVATAR_TRACKER_OK;
}

void result_to_c(const TrackingResult& in, avatar_tracker_result& out) noexcept {
    std::memset(&out, 0, sizeof(out));
    out.source_timestamp_ns = in.source_timestamp_ns;
    out.valid = in.valid ? 1u : 0u;
    out.features_valid = in.features_valid ? 1u : 0u;
    out.confidence = in.confidence;
    for (std::size_t index = 0; index < in.pose.rotation.size(); ++index) {
        out.pose.rotation[index] = in.pose.rotation[index];
    }
    for (std::size_t index = 0; index < in.pose.translation.size(); ++index) {
        out.pose.translation[index] = in.pose.translation[index];
    }
    const std::size_t count = in.landmarks.count < AVATAR_TRACKER_LANDMARK_CAPACITY
                                  ? in.landmarks.count
                                  : AVATAR_TRACKER_LANDMARK_CAPACITY;
    out.landmark_count = static_cast<std::uint32_t>(count);
    for (std::size_t index = 0; index < count; ++index) {
        out.landmarks[index].x = in.landmarks.points[index].x;
        out.landmarks[index].y = in.landmarks.points[index].y;
        out.landmarks[index].z = in.landmarks.points[index].z;
    }
    for (std::size_t index = 0; index < in.blendshapes.coefficients.size(); ++index) {
        out.blendshapes[index] = in.blendshapes.coefficients[index];
    }
}

} // namespace

struct avatar_tracker_handle {
    explicit avatar_tracker_handle(const ScriptedTrackerConfig& config) : tracker(config) {}

    ScriptedTracker tracker;
    std::optional<TrackingResult> pending;
    std::uint32_t fault = AVATAR_TRACKER_FAULT_NONE;
};

extern "C" {

AVATAR_TRACKER_API avatar_tracker_handle* avatar_tracker_create(const avatar_tracker_config* config,
                                                                avatar_tracker_error* out_error) {
    set_error(out_error, AVATAR_TRACKER_OK);
    try {
        const avatar_tracker_config defaults = AVATAR_TRACKER_CONFIG_INIT;
        const avatar_tracker_config& effective = config != nullptr ? *config : defaults;

        ScriptedTrackerConfig scripted;
        const avatar_tracker_error error = to_scripted_config(effective, scripted);
        if (error != AVATAR_TRACKER_OK) {
            set_error(out_error, error);
            return nullptr;
        }

        auto* handle = new (std::nothrow) avatar_tracker_handle(scripted);
        if (handle == nullptr) {
            set_error(out_error, AVATAR_TRACKER_ERROR_INTERNAL);
            return nullptr;
        }
        handle->fault = effective.simulate_fault;

        TrackerConfig tracker_config;
        tracker_config.input_width = effective.input_width;
        tracker_config.input_height = effective.input_height;
        if (!handle->tracker.initialize(tracker_config)) {
            delete handle;
            set_error(out_error, AVATAR_TRACKER_ERROR_INTERNAL);
            return nullptr;
        }
        return handle;
    } catch (...) {
        set_error(out_error, AVATAR_TRACKER_ERROR_INTERNAL);
        return nullptr;
    }
}

AVATAR_TRACKER_API avatar_tracker_error
avatar_tracker_configure(avatar_tracker_handle* handle, const avatar_tracker_config* config) {
    if (handle == nullptr || config == nullptr) {
        return AVATAR_TRACKER_ERROR_INVALID_ARGUMENT;
    }
    try {
        ScriptedTrackerConfig scripted;
        const avatar_tracker_error error = to_scripted_config(*config, scripted);
        if (error != AVATAR_TRACKER_OK) {
            return error;
        }
        if (handle->fault == AVATAR_TRACKER_FAULT_CONFIGURE) {
            throw std::runtime_error("injected tracker configure fault");
        }
        handle->tracker.configure(scripted);
        handle->pending.reset();
        handle->fault = config->simulate_fault;
        return AVATAR_TRACKER_OK;
    } catch (...) {
        return AVATAR_TRACKER_ERROR_INTERNAL;
    }
}

AVATAR_TRACKER_API avatar_tracker_error
avatar_tracker_push_frame(avatar_tracker_handle* handle, const avatar_tracker_frame* frame) {
    if (handle == nullptr || frame == nullptr) {
        return AVATAR_TRACKER_ERROR_INVALID_ARGUMENT;
    }
    if (frame->plane_count > 3) {
        return AVATAR_TRACKER_ERROR_INVALID_ARGUMENT;
    }
    if (frame->plane_count > 0 && frame->plane0 == nullptr) {
        return AVATAR_TRACKER_ERROR_INVALID_ARGUMENT;
    }
    PixelFormat format = PixelFormat::bgra8;
    if (!format_from_c(frame->format, format)) {
        return AVATAR_TRACKER_ERROR_INVALID_ARGUMENT;
    }
    try {
        if (handle->fault == AVATAR_TRACKER_FAULT_PUSH) {
            throw std::runtime_error("injected tracker push fault");
        }
        avatar::capture::VideoFrame input;
        input.timestamp_ns = frame->timestamp_ns;
        input.arrival_ns = frame->arrival_ns;
        input.sequence = frame->sequence;
        input.width = frame->width;
        input.height = frame->height;
        input.format = format;
        input.strides = {frame->stride, 0, 0};
        input.planes = {frame->plane0, frame->plane1, frame->plane2};

        const std::uint32_t before = handle->tracker.cursor();
        handle->tracker.submit(input);
        const std::uint32_t after = handle->tracker.cursor();
        if (after != before) {
            handle->pending = handle->tracker.latest();
        }
        return AVATAR_TRACKER_OK;
    } catch (...) {
        return AVATAR_TRACKER_ERROR_INTERNAL;
    }
}

AVATAR_TRACKER_API avatar_tracker_error
avatar_tracker_poll_result(avatar_tracker_handle* handle, avatar_tracker_result* out_result) {
    if (handle == nullptr || out_result == nullptr) {
        return AVATAR_TRACKER_ERROR_INVALID_ARGUMENT;
    }
    std::memset(out_result, 0, sizeof(*out_result));
    try {
        if (handle->fault == AVATAR_TRACKER_FAULT_POLL) {
            throw std::runtime_error("injected tracker poll fault");
        }
        if (!handle->pending.has_value()) {
            return AVATAR_TRACKER_NO_RESULT;
        }
        result_to_c(*handle->pending, *out_result);
        handle->pending.reset();
        return AVATAR_TRACKER_OK;
    } catch (...) {
        std::memset(out_result, 0, sizeof(*out_result));
        return AVATAR_TRACKER_ERROR_INTERNAL;
    }
}

AVATAR_TRACKER_API void avatar_tracker_destroy(avatar_tracker_handle* handle) {
    delete handle;
}

} // extern "C"
