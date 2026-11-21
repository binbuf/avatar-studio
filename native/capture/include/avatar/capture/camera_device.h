#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "avatar/capture/video_frame.h"
#include "avatar/core/status.h"

namespace avatar::capture {

/// A physical or virtual capture device.
///
/// `id` is the platform's stable device identity, not an array index: on
/// Windows it is the Media Foundation symbolic link
/// (`MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK`), which survives
/// reconnects and reordering. Persisted settings must store `id`, never a
/// position.
struct CameraDevice {
    std::string id{};
    std::string name{};
};

/// One advertised (resolution, frame rate, pixel format) combination.
struct CameraMode {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fps_numerator = 0;
    std::uint32_t fps_denominator = 1;
    PixelFormat format = PixelFormat::nv12;

    [[nodiscard]] double fps() const noexcept;
};

[[nodiscard]] bool operator==(const CameraMode& left, const CameraMode& right) noexcept;

/// What a device advertises. An empty list means "the device did not answer";
/// the caller then tries its preference list directly instead of guessing.
struct CameraCapabilities {
    std::vector<CameraMode> modes{};

    [[nodiscard]] bool empty() const noexcept { return modes.empty(); }
};

/// Why a backend operation failed. The capture state machine, not the platform
/// backend, owns the mapping to user-visible diagnostics.
enum class CameraFailureReason : std::uint8_t {
    none = 0,
    invalid_format,
    busy,
    permission_denied,
    device_lost,
    device_gone,
    unsupported,
    internal,
};

struct CameraFailure {
    CameraFailureReason reason = CameraFailureReason::none;
    core::StatusCode code = core::StatusCode::ok;
    std::int32_t hresult = 0;
    std::string message{};

    [[nodiscard]] bool ok() const noexcept { return reason == CameraFailureReason::none; }
};

/// A borrowed view of one sample delivered by a platform backend.
///
/// `data` is valid only for the duration of the callback. The capture source
/// copies the planes into a ring slot synchronously; a backend must not retain
/// the pointer or the sample object.
struct CameraSample {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PixelFormat format = PixelFormat::nv12;
    ColorSpace color_space = ColorSpace::bt709_limited;
    std::array<std::uint32_t, kMaxFramePlanes> strides{};
    std::array<std::size_t, kMaxFramePlanes> plane_offsets{};
    std::array<std::size_t, kMaxFramePlanes> plane_sizes{};
    std::uint64_t source_timestamp_ns = 0;
    bool has_source_timestamp = false;
    bool mirrored = false;
};

enum class CameraBackendErrorKind : std::uint8_t {
    device_lost = 0,
    permission_denied,
    busy,
    read_error,
    internal,
};

/// Backend callbacks run on the platform's capture thread (Media Foundation's
/// work queue on Windows). They must not block and must not call back into the
/// backend: a backend error is recorded by the source and handled on the next
/// `produce()` call.
struct CameraBackendCallbacks {
    void (*on_sample)(void* context, const CameraSample& sample) noexcept = nullptr;
    void (*on_error)(void* context, CameraBackendErrorKind kind, std::int32_t hresult) noexcept =
        nullptr;
    void* context = nullptr;
};

/// The platform seam below `ICameraSource`.
///
/// The Windows implementation wraps an async `IMFSourceReader`; the fake in the
/// unit tests implements the same interface, so negotiation, timestamp
/// mapping, the watchdog and reconnect are tested without a camera. macOS
/// (T52) and Linux (T55) implement this interface, not a second state machine.
class ICameraDeviceBackend {
public:
    virtual ~ICameraDeviceBackend() = default;

    /// Lists devices. Never opens or streams.
    [[nodiscard]] virtual core::Status enumerate(std::vector<CameraDevice>& devices) = 0;

    /// Native modes for one device. May fail (for example when another
    /// application holds the device); the caller falls back to its preference
    /// list.
    [[nodiscard]] virtual CameraFailure capabilities(std::string_view device_id,
                                                     CameraCapabilities& out) = 0;

    /// Opens a device and selects `mode` explicitly. No automatic conversion:
    /// an unsupported mode fails with `invalid_format` so the caller can step
    /// down the preference list.
    [[nodiscard]] virtual CameraFailure open(std::string_view device_id,
                                             const CameraMode& mode) = 0;

    /// Starts asynchronous delivery. The first sample may arrive on another
    /// thread before this returns.
    [[nodiscard]] virtual CameraFailure start(const CameraBackendCallbacks& callbacks) = 0;

    /// Stops delivery and waits out an in-flight callback. Safe to call twice.
    virtual void stop() noexcept = 0;

    /// Releases the device. Implies `stop()`.
    virtual void close() noexcept = 0;

    [[nodiscard]] virtual bool running() const noexcept = 0;

    /// The mode actually selected by `open`, which may differ from the request
    /// only if the platform silently substituted one (it is reported, never
    /// hidden).
    [[nodiscard]] virtual CameraMode negotiated_mode() const noexcept = 0;

    [[nodiscard]] virtual ColorSpace color_space() const noexcept = 0;

    [[nodiscard]] virtual std::string_view last_error() const noexcept = 0;
};

/// Creates the backend for the current platform. Non-Windows builds return a
/// stub that enumerates nothing; the capture state machine still works and
/// reports "no camera".
[[nodiscard]] std::unique_ptr<ICameraDeviceBackend> create_platform_camera_backend();

/// Format preference (NV12, YUY2, BGRA8) crossed with the resolution policy
/// (720p60, 1080p30, 1080p60, 640x480@30). MJPEG is deliberately absent until
/// a decoder exists: `FrameRing` owns fixed-size CPU formats only, so a
/// compressed mode cannot be published without an implicit conversion.
[[nodiscard]] std::vector<CameraMode> default_camera_preferences();

[[nodiscard]] std::string_view camera_failure_reason_name(CameraFailureReason reason) noexcept;

/// Maps a Win32/Media Foundation HRESULT to a failure reason. Media
/// Foundation's exclusive-access failure is a generic `ERROR_GEN_FAILURE` on
/// many devices, so it is classified as busy.
[[nodiscard]] CameraFailureReason classify_camera_hresult(std::int32_t hresult) noexcept;

/// Exact width, height and format, with a small frame-rate tolerance because
/// devices report 29.97 as 30000/1001 or 2997/100.
[[nodiscard]] bool camera_mode_matches(const CameraMode& left, const CameraMode& right,
                                       double fps_tolerance = 0.75) noexcept;

/// Resolves the preference list against advertised capabilities.
///
/// Exact preferred modes come first, in preference order. When none match, the
/// closest advertised mode per supported format is returned, so the caller can
/// still open something and report the step-down (`CAP-003`).
[[nodiscard]] std::vector<CameraMode> select_camera_modes(
    const CameraCapabilities& capabilities, const std::vector<CameraMode>& preferences);

} // namespace avatar::capture
