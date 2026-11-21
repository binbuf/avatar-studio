#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "avatar/capture/camera_device.h"
#include "avatar/capture/icamera_source.h"
#include "avatar/core/clock.h"
#include "avatar/core/diagnostics.h"
#include "avatar/core/latency_histogram.h"

namespace avatar::capture {

/// Capture lifecycle. The state machine is platform-neutral and driven by
/// `start()`, `stop()`, `suspend()`, `resume()` and `produce()`.
enum class CameraState : std::uint8_t {
    closed = 0,
    opening,
    streaming,
    device_lost,
    busy,
    permission_denied,
    stalled,
    suspended,
};

[[nodiscard]] std::string_view camera_state_name(CameraState state) noexcept;

enum class CameraEventKind : std::uint8_t {
    none = 0,
    device_lost,
    device_restored,
    device_busy,
    permission_denied,
    format_rejected,
    stream_stalled,
    stream_restarted,
    timestamp_regression,
    no_camera,
    suspended,
    resumed,
    reconnect_failed,
    read_error,
};

/// One state transition the UI or logger should surface. `code` is the stable
/// CAP code for the condition, or `unknown` for a successful recovery such as
/// `device_restored` (a recovery is not a diagnostic condition).
struct CameraEvent {
    CameraEventKind kind = CameraEventKind::none;
    core::DiagnosticCode code = core::DiagnosticCode::unknown;
    std::string detail{};
};

struct CameraConfig {
    /// Empty selects the first enumerated device.
    std::string device_id{};
    /// Empty uses `default_camera_preferences()`.
    std::vector<CameraMode> preferences{};
    bool reconnect = true;
    bool restart_on_stall = true;
    /// The documented hot-plug poll: 2 s.
    std::uint64_t reconnect_poll_ns = 2'000'000'000ull;
    /// Watchdog deadline as a multiple of the observed frame interval (the
    /// nominal interval while the stream warms up).
    std::uint32_t watchdog_multiplier = 2;
    /// Allowance for the first frame after a start or restart. A UVC camera can
    /// take a second to begin streaming after the media type is set; without
    /// this the watchdog would restart a healthy stream during warm-up.
    std::uint64_t startup_grace_ns = 2'000'000'000ull;
    /// A mode that delivers less than `shortfall_percent` of its negotiated
    /// frame rate for this long is demoted to the next preference.
    std::uint64_t demote_after_ns = 3'000'000'000ull;
    std::uint32_t shortfall_percent = 80;
    /// The watchdog deadline is 2x the *observed* frame interval (the nominal
    /// interval while the stream warms up), clamped to this range. The floor
    /// keeps a low-light camera that legitimately drops to a few fps from being
    /// treated as stalled.
    std::uint64_t watchdog_min_deadline_ns = 1'000'000'000ull;
    std::uint64_t watchdog_max_deadline_ns = 5'000'000'000ull;
    std::uint32_t ring_capacity = 4;
};

/// Callback entry to ring publish. One microsecond buckets over a 2.048 ms
/// window; the documented target is < 1 ms.
using CaptureLatencyHistogram = core::LatencyHistogram<2048, 1'000>;

/// The `ICameraSource` implementation every platform shares.
///
/// Ownership and threading:
/// - The injected backend delivers samples on its own capture thread. That
///   thread is the only producer; it copies directly into a `FrameRing` slot.
/// - `produce()` is called by the application (typically once per frame). It
///   never blocks on consumers; it processes a pending backend error, runs the
///   stall watchdog, and performs the 2-second reconnect poll.
/// - A backend error callback only records the error. Recovery runs on the
///   `produce()` thread, which is what makes `stop()` re-entrant.
///
/// Timestamps are mapped onto `FrameClock` by anchoring the first source
/// timestamp to the arrival time and adding source deltas. A backwards or
/// duplicate source timestamp re-anchors to the arrival time, keeps the mapped
/// timeline monotonic and emits `CAP-005` once per episode. The ring is not
/// cleared on re-anchor because `FrameRing::reset()` requires that no lease is
/// alive; stale frames age out of the four-slot ring instead.
class CameraSource final : public ICameraSource {
public:
    CameraSource(ICameraDeviceBackend& backend, core::FrameClock& clock, CameraConfig config = {});
    ~CameraSource() override;

    CameraSource(const CameraSource&) = delete;
    CameraSource& operator=(const CameraSource&) = delete;

    /// Enumerates, negotiates, opens and starts. Idempotent while streaming.
    /// On failure the state names the condition (`busy`, `permission_denied`,
    /// `closed`) and the error message names the likely owner of a busy device.
    [[nodiscard]] core::Status start() override;

    /// Stops delivery. Published frames stay readable until the source dies,
    /// as the `ICameraSource` contract requires.
    void stop() override;

    [[nodiscard]] FrameRing& ring() noexcept override;

    /// Housekeeping plus "did a frame arrive since the last call?".
    [[nodiscard]] bool produce() override;

    [[nodiscard]] SourceInfo info() const noexcept override;

    [[nodiscard]] CameraState state() const noexcept { return state_.load(); }
    [[nodiscard]] CameraMode negotiated_mode() const;
    [[nodiscard]] ColorSpace color_space() const;
    [[nodiscard]] std::string_view device_id() const noexcept { return device_id_; }
    [[nodiscard]] std::string_view device_name() const noexcept { return device_name_; }

    [[nodiscard]] std::uint64_t frames_published() const noexcept {
        return frames_published_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t restarts() const noexcept { return restarts_.load(); }
    [[nodiscard]] std::uint64_t reconnects() const noexcept { return reconnects_.load(); }
    [[nodiscard]] std::uint64_t timestamp_resets() const noexcept {
        return timestamp_resets_.load();
    }

    /// A snapshot of the callback-to-publish histogram. The histogram is
    /// protected because the callback thread writes it.
    [[nodiscard]] CaptureLatencyHistogram publish_latency() const;

    /// Returns and clears the pending events. Safe to call from the app thread.
    [[nodiscard]] std::vector<CameraEvent> take_events();

    /// OS suspend/resume hooks. The app calls these from its power
    /// notification handler; the capture layer stops the device and reopens it
    /// with the previous configuration, resetting the timestamp base.
    void suspend();
    [[nodiscard]] core::Status resume();

private:
    static void on_sample_thunk(void* context, const CameraSample& sample) noexcept;
    static void on_error_thunk(void* context, CameraBackendErrorKind kind,
                               std::int32_t hresult) noexcept;

    void handle_sample(const CameraSample& sample) noexcept;
    void process_pending_error(core::TimestampNs now);
    void check_watchdog(core::TimestampNs now);
    void check_shortfall(core::TimestampNs now);
    [[nodiscard]] bool demote_mode(const std::string& reason);
    void maybe_reconnect(core::TimestampNs now);
    void emit(CameraEventKind kind, core::DiagnosticCode code, std::string detail = {});

    [[nodiscard]] core::Status open_and_start();
    [[nodiscard]] bool resolve_device();
    [[nodiscard]] std::uint64_t expected_interval_ns() const noexcept;
    void reset_stream_state() noexcept;
    [[nodiscard]] bool ensure_ring(const CameraMode& mode);
    [[nodiscard]] core::TimestampNs now() noexcept;

    ICameraDeviceBackend& backend_;
    core::FrameClock& clock_;
    CameraConfig config_;

    mutable std::mutex clock_mutex_{};
    std::atomic<CameraState> state_{CameraState::closed};
    std::string device_id_{};
    std::string device_name_{};
    std::string candidates_device_id_{};
    std::vector<CameraMode> candidates_{};
    std::size_t candidate_index_ = 0;
    mutable std::mutex mode_mutex_{};
    CameraMode mode_{};
    ColorSpace color_space_ = ColorSpace::srgb;

    // Throughput demotion, app thread only.
    core::TimestampNs mode_start_ns_ = 0;
    std::uint64_t frames_at_mode_start_ = 0;
    bool shortfall_checked_ = false;

    // Rings are never destroyed during a session: a consumer may still hold a
    // lease on the previous ring after a mode change, and destroying it would
    // free the slot storage under the lease.
    std::vector<std::unique_ptr<FrameRing>> rings_{};
    FrameRing placeholder_ring_{0, 0, PixelFormat::bgra8, 1};
    FrameRing* active_ring_ = nullptr;

    // Capture-thread counters. Only the capture thread publishes.
    std::atomic<std::uint64_t> publish_counter_{0};
    std::uint64_t seen_publish_counter_ = 0;
    std::atomic<std::uint64_t> frames_published_{0};
    std::atomic<std::uint64_t> restarts_{0};
    std::atomic<std::uint64_t> reconnects_{0};
    std::atomic<std::uint64_t> timestamp_resets_{0};
    std::atomic<std::uint64_t> last_frame_ns_{0};
    std::atomic<std::uint64_t> observed_interval_ns_{0};
    std::atomic<core::TimestampNs> watchdog_grace_until_{0};
    std::atomic<core::TimestampNs> restart_ns_{0};
    std::atomic<bool> accepting_{false};
    std::atomic<bool> restart_attempted_{false};

    // Backend errors are recorded here and handled on the produce() thread.
    std::atomic<CameraBackendErrorKind> pending_error_{CameraBackendErrorKind::internal};
    std::atomic<bool> has_pending_error_{false};
    std::atomic<std::int32_t> pending_hresult_{0};

    // Timestamp mapping, owned by the capture thread.
    std::mutex timestamp_mutex_{};
    bool timestamp_anchored_ = false;
    bool regression_reported_ = false;
    std::uint64_t anchor_source_ns_ = 0;
    std::uint64_t last_source_ns_ = 0;
    core::TimestampNs anchor_clock_ns_ = 0;
    core::TimestampNs last_timestamp_ns_ = 0;

    mutable std::mutex stats_mutex_{};
    CaptureLatencyHistogram publish_latency_{};

    mutable std::mutex event_mutex_{};
    std::vector<CameraEvent> events_{};

    // App-thread state.
    core::TimestampNs next_reconnect_ns_ = 0;
    bool reconnect_failure_reported_ = false;
};

} // namespace avatar::capture
