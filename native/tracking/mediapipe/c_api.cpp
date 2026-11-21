#include "backend.h"

#include <filesystem>
#include <stdexcept>

#define NOMINMAX
#include <windows.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace avatar::tracker_backend {
std::string bundled_model_path() {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path.data(),
                                           static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) {
        throw std::runtime_error("cannot locate tracker module");
    }
    path.resize(length);
    return (std::filesystem::path(path).parent_path() / L"tracker-models" / L"face_landmarker.task")
        .u8string();
}
} // namespace avatar::tracker_backend

using avatar::tracker_backend::Runtime;

struct avatar_tracker_handle {
    std::mutex mutex;
    std::unique_ptr<Runtime> runtime;
    uint32_t fault = AVATAR_TRACKER_FAULT_NONE;
};

extern "C" {
AVATAR_TRACKER_API avatar_tracker_handle* avatar_tracker_create(const avatar_tracker_config* config,
                                                                avatar_tracker_error* error) {
    if (error) {
        *error = AVATAR_TRACKER_OK;
    }
    try {
        const avatar_tracker_config defaults = AVATAR_TRACKER_CONFIG_INIT;
        const auto& c = config ? *config : defaults;
        const auto status = avatar::tracker_backend::validate_config(c);
        if (status != AVATAR_TRACKER_OK) {
            if (error) {
                *error = status;
            }
            return nullptr;
        }
        auto handle = std::make_unique<avatar_tracker_handle>();
        handle->runtime = std::make_unique<Runtime>(avatar::tracker_backend::bundled_model_path(),
                                                    c.input_width ? c.input_width : 256);
        handle->fault = c.simulate_fault;
        return handle.release();
    } catch (...) {
        if (error) {
            *error = AVATAR_TRACKER_ERROR_INTERNAL;
        }
        return nullptr;
    }
}

AVATAR_TRACKER_API avatar_tracker_error
avatar_tracker_configure(avatar_tracker_handle* handle, const avatar_tracker_config* config) {
    if (!handle || !config) {
        return AVATAR_TRACKER_ERROR_INVALID_ARGUMENT;
    }
    try {
        const auto status = avatar::tracker_backend::validate_config(*config);
        if (status != AVATAR_TRACKER_OK) {
            return status;
        }
        std::lock_guard<std::mutex> lock(handle->mutex);
        if (handle->fault == AVATAR_TRACKER_FAULT_CONFIGURE) {
            throw std::runtime_error("injected configure fault");
        }
        auto replacement =
            std::make_unique<Runtime>(avatar::tracker_backend::bundled_model_path(),
                                      config->input_width ? config->input_width : 256);
        handle->runtime.swap(replacement);
        handle->fault = config->simulate_fault;
        return AVATAR_TRACKER_OK;
    } catch (...) {
        return AVATAR_TRACKER_ERROR_INTERNAL;
    }
}

AVATAR_TRACKER_API avatar_tracker_error
avatar_tracker_push_frame(avatar_tracker_handle* handle, const avatar_tracker_frame* frame) {
    if (!handle || !frame) {
        return AVATAR_TRACKER_ERROR_INVALID_ARGUMENT;
    }
    try {
        const auto status = avatar::tracker_backend::validate_frame(*frame);
        if (status != AVATAR_TRACKER_OK) {
            return status;
        }
        std::unique_lock<std::mutex> lock(handle->mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            return AVATAR_TRACKER_OK;
        }
        if (handle->fault == AVATAR_TRACKER_FAULT_PUSH) {
            throw std::runtime_error("injected push fault");
        }
        return handle->runtime->push(*frame);
    } catch (...) {
        return AVATAR_TRACKER_ERROR_INTERNAL;
    }
}

AVATAR_TRACKER_API avatar_tracker_error avatar_tracker_poll_result(avatar_tracker_handle* handle,
                                                                   avatar_tracker_result* result) {
    if (!handle || !result) {
        return AVATAR_TRACKER_ERROR_INVALID_ARGUMENT;
    }
    *result = {};
    try {
        std::unique_lock<std::mutex> lock(handle->mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            return AVATAR_TRACKER_NO_RESULT;
        }
        if (handle->fault == AVATAR_TRACKER_FAULT_POLL) {
            throw std::runtime_error("injected poll fault");
        }
        return handle->runtime->poll(*result);
    } catch (...) {
        return AVATAR_TRACKER_ERROR_INTERNAL;
    }
}

AVATAR_TRACKER_API void avatar_tracker_destroy(avatar_tracker_handle* handle) {
    try {
        delete handle;
    } catch (...) {
        // No exception may cross the C ABI, including during teardown.
    }
}
} // extern "C"
