#pragma once

#include <cstdint>
#include <string>

#include "avatar/tracker/tracker_c_api.h"
#include "avatar/tracking/itracking_backend.h"

namespace avatar::tracking {

/// Production `ITrackingBackend` over the tracker C ABI (T26).
///
/// The application never links a vendor library: this class loads the pinned
/// `libavatartracker.dll` (or the platform equivalent) at runtime, resolves the
/// five v1 exports and copies frames/results across the POD boundary. The T11
/// backend and the M0 harness use the same ABI; this is the app-side adapter
/// the Live pipeline owns.
///
/// Threading follows the ABI contract: `submit` never waits for inference and
/// `latest` returns only the newest result. The class is not internally locked;
/// call it from the render thread (the capture thread writes the ring, not this
/// object).
class CabiTracker final : public ITrackingBackend {
public:
    explicit CabiTracker(std::string library_path = {});
    ~CabiTracker() override;

    CabiTracker(const CabiTracker&) = delete;
    CabiTracker& operator=(const CabiTracker&) = delete;

    /// Loads the library and creates the backend. A missing library, a missing
    /// export or a create failure leaves the object uninitialized and returns
    /// false with `last_error()` set.
    bool load(std::string& error);

    bool initialize(const TrackerConfig& config) override;
    void submit(const capture::VideoFrame& frame) override;
    [[nodiscard]] std::optional<TrackingResult> latest() override;
    [[nodiscard]] TrackerStats stats() const override;
    void shutdown() override;

    [[nodiscard]] bool is_loaded() const noexcept { return module_ != nullptr; }
    [[nodiscard]] bool is_initialized() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }
    [[nodiscard]] const std::string& library_path() const noexcept { return library_path_; }

private:
    bool bind_symbols(std::string& error);
    void unload();

    std::string library_path_;
    std::string last_error_;
    void* module_ = nullptr;
    avatar_tracker_handle* handle_ = nullptr;

    using CreateFn = avatar_tracker_handle* (*)(const avatar_tracker_config*,
                                                avatar_tracker_error*);
    using ConfigureFn = avatar_tracker_error (*)(avatar_tracker_handle*,
                                                 const avatar_tracker_config*);
    using PushFn = avatar_tracker_error (*)(avatar_tracker_handle*, const avatar_tracker_frame*);
    using PollFn = avatar_tracker_error (*)(avatar_tracker_handle*, avatar_tracker_result*);
    using DestroyFn = void (*)(avatar_tracker_handle*);

    CreateFn create_ = nullptr;
    ConfigureFn configure_ = nullptr;
    PushFn push_ = nullptr;
    PollFn poll_ = nullptr;
    DestroyFn destroy_ = nullptr;

    TrackerStats stats_{};
};

} // namespace avatar::tracking
