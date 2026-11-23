#include "avatar/tracking/cabi_tracker.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <utility>

#include "avatar/capture/video_frame.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace avatar::tracking {

namespace {

#if defined(_WIN32)
std::string path_text(const std::string& path) {
    return std::filesystem::path(path).string();
}
#else
std::string path_text(const std::string& path) {
    return path;
}
#endif

bool to_abi_frame(const capture::VideoFrame& frame, avatar_tracker_frame& abi, std::string& error) {
    abi = {};
    abi.timestamp_ns = frame.timestamp_ns;
    abi.arrival_ns = frame.arrival_ns;
    abi.sequence = frame.sequence;
    abi.width = frame.width;
    abi.height = frame.height;
    abi.format = static_cast<std::uint32_t>(frame.format);
    abi.stride = frame.strides[0];
    std::array<capture::PlaneLayout, capture::kMaxFramePlanes> layout{};
    if (!capture::compute_plane_layout(frame.format, frame.width, frame.height, layout)) {
        error = "frame layout is invalid";
        return false;
    }
    const std::array<const std::uint8_t*, capture::kMaxFramePlanes> planes = {
        frame.planes[0], frame.planes[1], frame.planes[2]};
    for (std::size_t index = 0; index < capture::kMaxFramePlanes; ++index) {
        if (planes[index] != nullptr && layout[index].size != 0) {
            ++abi.plane_count;
        }
    }
    abi.plane0 = planes[0];
    abi.plane1 = planes[1];
    abi.plane2 = planes[2];
    abi.plane0_size = planes[0] != nullptr ? layout[0].size : 0;
    abi.plane1_size = planes[1] != nullptr ? layout[1].size : 0;
    abi.plane2_size = planes[2] != nullptr ? layout[2].size : 0;
    return true;
}

TrackingResult from_abi(const avatar_tracker_result& abi) {
    TrackingResult result;
    result.source_timestamp_ns = abi.source_timestamp_ns;
    result.valid = abi.valid != 0;
    result.features_valid = abi.features_valid != 0;
    result.confidence = abi.confidence;
    for (std::size_t index = 0; index < 4; ++index) {
        result.pose.rotation[index] = abi.pose.rotation[index];
    }
    for (std::size_t index = 0; index < 3; ++index) {
        result.pose.translation[index] = abi.pose.translation[index];
    }
    const std::size_t count = std::min<std::size_t>(abi.landmark_count, LandmarkSet::kCapacity);
    result.landmarks.count = count;
    for (std::size_t index = 0; index < count; ++index) {
        result.landmarks.points[index].x = abi.landmarks[index].x;
        result.landmarks.points[index].y = abi.landmarks[index].y;
        result.landmarks.points[index].z = abi.landmarks[index].z;
    }
    for (std::size_t index = 0; index < BlendshapeSet::kCount; ++index) {
        result.blendshapes.coefficients[index] = abi.blendshapes[index];
    }
    return result;
}

} // namespace

CabiTracker::CabiTracker(std::string library_path) : library_path_(std::move(library_path)) {}

CabiTracker::~CabiTracker() {
    shutdown();
}

bool CabiTracker::load(std::string& error) {
    if (module_ != nullptr) {
        error.clear();
        return true;
    }
    if (library_path_.empty()) {
        error = "tracker library path is empty";
        last_error_ = error;
        return false;
    }
#if defined(_WIN32)
    const std::filesystem::path wide_path(library_path_);
    // LOAD_WITH_ALTERED_SEARCH_PATH is not enough inside a host process that
    // restricts the DLL search path (Godot). Add the tracker's own directory as
    // an explicit user directory so opencv_world3410.dll resolves next to it.
    const std::filesystem::path directory = wide_path.parent_path();
    DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(directory.c_str());
    module_ = LoadLibraryExW(wide_path.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (cookie != nullptr) {
        RemoveDllDirectory(cookie);
    }
    if (module_ == nullptr) {
        error = "cannot load tracker DLL '" + path_text(library_path_) + "' (error " +
                std::to_string(GetLastError()) + ")";
    }
#else
    module_ = dlopen(library_path_.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (module_ == nullptr) {
        error = std::string("cannot load tracker library: ") + dlerror();
    }
#endif
    if (module_ == nullptr) {
        last_error_ = error;
        return false;
    }
    if (!bind_symbols(error)) {
        last_error_ = error;
        unload();
        return false;
    }
    last_error_.clear();
    return true;
}

bool CabiTracker::bind_symbols(std::string& error) {
#if defined(_WIN32)
    create_ = reinterpret_cast<CreateFn>(
        GetProcAddress(static_cast<HMODULE>(module_), "avatar_tracker_create"));
    configure_ = reinterpret_cast<ConfigureFn>(
        GetProcAddress(static_cast<HMODULE>(module_), "avatar_tracker_configure"));
    push_ = reinterpret_cast<PushFn>(
        GetProcAddress(static_cast<HMODULE>(module_), "avatar_tracker_push_frame"));
    poll_ = reinterpret_cast<PollFn>(
        GetProcAddress(static_cast<HMODULE>(module_), "avatar_tracker_poll_result"));
    destroy_ = reinterpret_cast<DestroyFn>(
        GetProcAddress(static_cast<HMODULE>(module_), "avatar_tracker_destroy"));
#else
    create_ = reinterpret_cast<CreateFn>(dlsym(module_, "avatar_tracker_create"));
    configure_ = reinterpret_cast<ConfigureFn>(dlsym(module_, "avatar_tracker_configure"));
    push_ = reinterpret_cast<PushFn>(dlsym(module_, "avatar_tracker_push_frame"));
    poll_ = reinterpret_cast<PollFn>(dlsym(module_, "avatar_tracker_poll_result"));
    destroy_ = reinterpret_cast<DestroyFn>(dlsym(module_, "avatar_tracker_destroy"));
#endif
    if (create_ == nullptr || configure_ == nullptr || push_ == nullptr || poll_ == nullptr ||
        destroy_ == nullptr) {
        error = "tracker library is missing one of the five v1 exports";
        return false;
    }
    return true;
}

bool CabiTracker::initialize(const TrackerConfig& config) {
    if (module_ == nullptr) {
        std::string error;
        if (!load(error)) {
            return false;
        }
    }
    if (handle_ != nullptr) {
        destroy_(handle_);
        handle_ = nullptr;
    }
    avatar_tracker_config abi = AVATAR_TRACKER_CONFIG_INIT;
    abi.input_width = config.input_width;
    abi.input_height = config.input_height;
    if (config.target_fps > 0) {
        abi.frame_interval_ns = 1'000'000'000ull / config.target_fps;
    }
    avatar_tracker_error status = AVATAR_TRACKER_OK;
    handle_ = create_(&abi, &status);
    if (handle_ == nullptr) {
        last_error_ =
            "avatar_tracker_create failed with status " + std::to_string(static_cast<int>(status));
        return false;
    }
    last_error_.clear();
    return true;
}

void CabiTracker::submit(const capture::VideoFrame& frame) {
    if (handle_ == nullptr) {
        stats_.record_drop(DropReason::not_initialized);
        return;
    }
    avatar_tracker_frame abi{};
    std::string error;
    if (!to_abi_frame(frame, abi, error)) {
        last_error_ = error;
        stats_.record_drop(DropReason::invalid_frame);
        return;
    }
    const avatar_tracker_error status = push_(handle_, &abi);
    if (status != AVATAR_TRACKER_OK) {
        last_error_ = "avatar_tracker_push_frame failed with status " +
                      std::to_string(static_cast<int>(status));
        stats_.record_drop(DropReason::invalid_frame);
        return;
    }
    ++stats_.submitted;
}

std::optional<TrackingResult> CabiTracker::latest() {
    if (handle_ == nullptr) {
        return std::nullopt;
    }
    avatar_tracker_result abi{};
    const avatar_tracker_error status = poll_(handle_, &abi);
    if (status == AVATAR_TRACKER_NO_RESULT) {
        return std::nullopt;
    }
    if (status != AVATAR_TRACKER_OK) {
        last_error_ = "avatar_tracker_poll_result failed with status " +
                      std::to_string(static_cast<int>(status));
        return std::nullopt;
    }
    TrackingResult result = from_abi(abi);
    stats_.record_result(result.confidence);
    return result;
}

TrackerStats CabiTracker::stats() const {
    return stats_;
}

void CabiTracker::shutdown() {
    if (destroy_ != nullptr && handle_ != nullptr) {
        destroy_(handle_);
    }
    handle_ = nullptr;
    unload();
}

void CabiTracker::unload() {
    if (module_ != nullptr) {
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(module_));
#else
        dlclose(module_);
#endif
    }
    module_ = nullptr;
    create_ = nullptr;
    configure_ = nullptr;
    push_ = nullptr;
    poll_ = nullptr;
    destroy_ = nullptr;
}

} // namespace avatar::tracking
