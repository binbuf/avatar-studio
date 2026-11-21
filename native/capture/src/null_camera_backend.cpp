// Non-Windows capture backend stub.
//
// macOS (T52) and Linux (T55) implement ICameraDeviceBackend for their own
// capture APIs. Until then the state machine still builds and reports "no
// camera" instead of failing to compile, so the shared tests and the
// --list-cameras smoke run on every platform.

#if !defined(_WIN32)

#include <string_view>
#include <vector>

#include "avatar/capture/camera_device.h"

namespace avatar::capture {

namespace {

class NullCameraBackend final : public ICameraDeviceBackend {
public:
    core::Status enumerate(std::vector<CameraDevice>& devices) override {
        devices.clear();
        return core::Status::success();
    }

    CameraFailure capabilities(std::string_view device_id, CameraCapabilities& out) override {
        (void)device_id;
        out.modes.clear();
        CameraFailure failure;
        failure.reason = CameraFailureReason::unsupported;
        failure.code = core::StatusCode::unsupported;
        failure.message = "no capture backend is implemented on this platform yet";
        last_error_ = failure.message;
        return failure;
    }

    CameraFailure open(std::string_view device_id, const CameraMode& mode) override {
        (void)device_id;
        (void)mode;
        CameraFailure failure;
        failure.reason = CameraFailureReason::unsupported;
        failure.code = core::StatusCode::unsupported;
        failure.message = "no capture backend is implemented on this platform yet";
        last_error_ = failure.message;
        return failure;
    }

    CameraFailure start(const CameraBackendCallbacks& callbacks) override {
        (void)callbacks;
        CameraFailure failure;
        failure.reason = CameraFailureReason::unsupported;
        failure.code = core::StatusCode::unsupported;
        failure.message = "no capture backend is implemented on this platform yet";
        last_error_ = failure.message;
        return failure;
    }

    void stop() noexcept override {}
    void close() noexcept override {}

    [[nodiscard]] bool running() const noexcept override { return false; }
    [[nodiscard]] CameraMode negotiated_mode() const noexcept override { return CameraMode{}; }
    [[nodiscard]] ColorSpace color_space() const noexcept override { return ColorSpace::srgb; }
    [[nodiscard]] std::string_view last_error() const noexcept override { return last_error_; }

private:
    std::string last_error_{};
};

} // namespace

std::unique_ptr<ICameraDeviceBackend> create_platform_camera_backend() {
    return std::make_unique<NullCameraBackend>();
}

} // namespace avatar::capture

#endif // !_WIN32
