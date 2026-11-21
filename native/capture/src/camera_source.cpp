#include "avatar/capture/camera_source.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace avatar::capture {

namespace {

constexpr std::size_t kMaxPendingEvents = 64;
/// A watchdog restart counts as successful only after this much continuous
/// frame delivery. A few frames from an over-bandwidth mode are not recovery.
constexpr core::TimestampNs kHealthyWindowNs = 5'000'000'000ull;

std::string format_hresult(std::int32_t hresult) {
    const auto value = static_cast<std::uint32_t>(hresult);
    constexpr char kDigits[] = "0123456789ABCDEF";
    std::string text = "hresult=0x";
    for (int shift = 28; shift >= 0; shift -= 4) {
        text.push_back(kDigits[(value >> static_cast<unsigned>(shift)) & 0xFu]);
    }
    return text;
}

std::string describe_mode(const CameraMode& mode) {
    std::string text = std::to_string(mode.width);
    text.push_back('x');
    text += std::to_string(mode.height);
    text.push_back('@');
    const double fps = mode.fps();
    const int whole = static_cast<int>(fps);
    text += std::to_string(whole);
    if (fps - static_cast<double>(whole) > 0.001) {
        const int fraction = static_cast<int>((fps - static_cast<double>(whole)) * 1000.0 + 0.5);
        text.push_back('.');
        text += std::to_string(fraction);
    }
    text.push_back(' ');
    text += pixel_format_name(mode.format);
    return text;
}

bool same_mode(const CameraMode& left, const CameraMode& right) {
    return camera_mode_matches(left, right);
}

} // namespace

std::string_view camera_state_name(CameraState state) noexcept {
    switch (state) {
        case CameraState::closed:
            return "closed";
        case CameraState::opening:
            return "opening";
        case CameraState::streaming:
            return "streaming";
        case CameraState::device_lost:
            return "device_lost";
        case CameraState::busy:
            return "busy";
        case CameraState::permission_denied:
            return "permission_denied";
        case CameraState::stalled:
            return "stalled";
        case CameraState::suspended:
            return "suspended";
    }
    return "unknown";
}

CameraSource::CameraSource(ICameraDeviceBackend& backend, core::FrameClock& clock,
                           CameraConfig config)
    : backend_(backend), clock_(clock), config_(std::move(config)) {
    if (config_.preferences.empty()) {
        config_.preferences = default_camera_preferences();
    }
    config_.ring_capacity = std::max<std::uint32_t>(config_.ring_capacity, 2);
    if (config_.watchdog_multiplier == 0) {
        config_.watchdog_multiplier = 2;
    }
    if (config_.reconnect_poll_ns == 0) {
        config_.reconnect_poll_ns = 2'000'000'000ull;
    }
}

CameraSource::~CameraSource() {
    stop();
}

core::TimestampNs CameraSource::now() noexcept {
    const std::lock_guard<std::mutex> lock(clock_mutex_);
    return clock_.now_ns();
}

core::Status CameraSource::start() {
    if (state_.load() == CameraState::streaming) {
        return core::Status::success();
    }
    // A fresh start re-negotiates from the top of the preference list rather
    // than resuming at a demoted candidate from the previous session.
    candidates_.clear();
    candidate_index_ = 0;
    candidates_device_id_.clear();
    state_.store(CameraState::opening);
    return open_and_start();
}

void CameraSource::stop() {
    backend_.stop();
    accepting_.store(false);
    {
        const std::lock_guard<std::mutex> lock(timestamp_mutex_);
        timestamp_anchored_ = false;
        regression_reported_ = false;
        last_source_ns_ = 0;
        last_timestamp_ns_ = 0;
    }
    last_frame_ns_.store(0);
    observed_interval_ns_.store(0);
    restart_attempted_.store(false);
    restart_ns_.store(0);
    next_reconnect_ns_ = 0;
    reconnect_failure_reported_ = false;
    has_pending_error_.store(false);
    mode_start_ns_ = 0;
    frames_at_mode_start_ = 0;
    shortfall_checked_ = true;
    state_.store(CameraState::closed);
}

FrameRing& CameraSource::ring() noexcept {
    return active_ring_ != nullptr ? *active_ring_ : placeholder_ring_;
}

bool CameraSource::resolve_device() {
    std::vector<CameraDevice> devices;
    const core::Status status = backend_.enumerate(devices);
    if (!status.ok()) {
        emit(CameraEventKind::no_camera, core::DiagnosticCode::cap_008, status.message());
        return false;
    }
    if (config_.device_id.empty()) {
        if (devices.empty()) {
            emit(CameraEventKind::no_camera, core::DiagnosticCode::cap_008, "no capture device");
            return false;
        }
        device_id_ = devices.front().id;
        device_name_ = devices.front().name;
        return true;
    }
    const auto match = std::find_if(
        devices.begin(), devices.end(),
        [this](const CameraDevice& device) { return device.id == config_.device_id; });
    if (match == devices.end()) {
        emit(CameraEventKind::no_camera, core::DiagnosticCode::cap_008,
             "configured device is not present: " + config_.device_id);
        return false;
    }
    device_id_ = match->id;
    device_name_ = match->name;
    return true;
}

bool CameraSource::ensure_ring(const CameraMode& mode) {
    for (const std::unique_ptr<FrameRing>& ring : rings_) {
        if (ring->width() == mode.width && ring->height() == mode.height &&
            ring->format() == mode.format) {
            active_ring_ = ring.get();
            return active_ring_->capacity() > 0;
        }
    }
    auto ring = std::make_unique<FrameRing>(mode.width, mode.height, mode.format,
                                            config_.ring_capacity);
    if (ring->capacity() == 0) {
        return false;
    }
    rings_.push_back(std::move(ring));
    active_ring_ = rings_.back().get();
    return true;
}

void CameraSource::reset_stream_state() noexcept {
    const std::lock_guard<std::mutex> lock(timestamp_mutex_);
    timestamp_anchored_ = false;
    regression_reported_ = false;
    last_source_ns_ = 0;
    last_timestamp_ns_ = 0;
    last_frame_ns_.store(0);
    observed_interval_ns_.store(0);
    restart_attempted_.store(false);
    restart_ns_.store(0);
    reconnect_failure_reported_ = false;
    has_pending_error_.store(false);
}

core::Status CameraSource::open_and_start() {
    if (device_id_.empty() && !resolve_device()) {
        state_.store(CameraState::closed);
        return core::Status::error(core::StatusCode::not_found, "no camera found",
                                   config_.device_id);
    }

    if (candidates_.empty() || candidates_device_id_ != device_id_) {
        CameraCapabilities capabilities;
        const CameraFailure caps_failure = backend_.capabilities(device_id_, capabilities);
        if (!caps_failure.ok() &&
            (caps_failure.reason == CameraFailureReason::permission_denied ||
             caps_failure.reason == CameraFailureReason::device_gone)) {
            // Do not keep probing a device the OS refused or that is gone.
            backend_.close();
            if (caps_failure.reason == CameraFailureReason::permission_denied) {
                state_.store(CameraState::permission_denied);
                emit(CameraEventKind::permission_denied, core::DiagnosticCode::cap_006,
                     caps_failure.message);
                return core::Status::error(core::StatusCode::internal_error,
                                           caps_failure.message);
            }
            state_.store(CameraState::device_lost);
            emit(CameraEventKind::device_lost, core::DiagnosticCode::cap_001,
                 caps_failure.message);
            return core::Status::error(core::StatusCode::not_found, caps_failure.message);
        }
        candidates_ = select_camera_modes(capabilities, config_.preferences);
        candidate_index_ = 0;
        candidates_device_id_ = device_id_;
    }

    if (candidates_.empty()) {
        backend_.close();
        state_.store(CameraState::closed);
        emit(CameraEventKind::format_rejected, core::DiagnosticCode::cap_003,
             "no supported capture format was advertised");
        return core::Status::error(core::StatusCode::unsupported,
                                   "no supported capture format was advertised");
    }

    CameraMode negotiated;
    bool opened = false;
    CameraFailure last_failure;
    while (candidate_index_ < candidates_.size()) {
        const CameraMode candidate = candidates_[candidate_index_];
        const CameraFailure failure = backend_.open(device_id_, candidate);
        if (failure.ok()) {
            negotiated = backend_.negotiated_mode();
            if (negotiated.width == 0 || negotiated.height == 0) {
                negotiated = candidate;
            }
            opened = true;
            break;
        }
        last_failure = failure;
        if (failure.reason == CameraFailureReason::invalid_format) {
            emit(CameraEventKind::format_rejected, core::DiagnosticCode::cap_003,
                 "rejected " + describe_mode(candidate) + ": " + failure.message);
            ++candidate_index_;
            continue;
        }

        backend_.close();
        switch (failure.reason) {
            case CameraFailureReason::busy:
                state_.store(CameraState::busy);
                emit(CameraEventKind::device_busy, core::DiagnosticCode::cap_002,
                     failure.message.empty()
                         ? "camera is in use by another application (Teams, Zoom, OBS or a browser)"
                         : failure.message);
                break;
            case CameraFailureReason::permission_denied:
                state_.store(CameraState::permission_denied);
                emit(CameraEventKind::permission_denied, core::DiagnosticCode::cap_006,
                     failure.message);
                break;
            case CameraFailureReason::device_lost:
            case CameraFailureReason::device_gone:
                state_.store(CameraState::device_lost);
                emit(CameraEventKind::device_lost, core::DiagnosticCode::cap_001,
                     failure.message);
                break;
            default:
                if (state_.load() == CameraState::opening) {
                    state_.store(CameraState::closed);
                }
                break;
        }
        core::StatusCode code = failure.code;
        if (code == core::StatusCode::ok) {
            code = core::StatusCode::internal_error;
        }
        return core::Status::error(code,
                                   failure.message.empty() ? "camera open failed"
                                                           : failure.message,
                                   format_hresult(failure.hresult));
    }

    if (!opened) {
        backend_.close();
        state_.store(CameraState::closed);
        emit(CameraEventKind::format_rejected, core::DiagnosticCode::cap_003,
             "all capture formats were rejected");
        return core::Status::error(core::StatusCode::unsupported,
                                   "all capture formats were rejected",
                                   format_hresult(last_failure.hresult));
    }

    if (!same_mode(negotiated, config_.preferences.front())) {
        emit(CameraEventKind::format_rejected, core::DiagnosticCode::cap_003,
             "negotiated " + describe_mode(negotiated) + " instead of " +
                 describe_mode(config_.preferences.front()));
    }

    if (!ensure_ring(negotiated)) {
        backend_.close();
        state_.store(CameraState::closed);
        return core::Status::error(core::StatusCode::unsupported,
                                   "the capture mode cannot be represented by a frame ring",
                                   describe_mode(negotiated));
    }

    {
        const std::lock_guard<std::mutex> lock(mode_mutex_);
        mode_ = negotiated;
        color_space_ = backend_.color_space();
    }
    reset_stream_state();

    CameraBackendCallbacks callbacks;
    callbacks.on_sample = &CameraSource::on_sample_thunk;
    callbacks.on_error = &CameraSource::on_error_thunk;
    callbacks.context = this;

    accepting_.store(true);
    state_.store(CameraState::streaming);
    const CameraFailure start_failure = backend_.start(callbacks);
    if (!start_failure.ok()) {
        accepting_.store(false);
        backend_.close();
        state_.store(CameraState::closed);
        return core::Status::error(core::StatusCode::internal_error,
                                   start_failure.message.empty() ? "camera start failed"
                                                                 : start_failure.message,
                                   format_hresult(start_failure.hresult));
    }
    watchdog_grace_until_.store(
        now() + std::max(config_.startup_grace_ns,
                         expected_interval_ns() * config_.watchdog_multiplier),
        std::memory_order_release);
    mode_start_ns_ = now();
    frames_at_mode_start_ = frames_published_.load(std::memory_order_relaxed);
    shortfall_checked_ = false;
    return core::Status::success();
}

void CameraSource::on_sample_thunk(void* context, const CameraSample& sample) noexcept {
    static_cast<CameraSource*>(context)->handle_sample(sample);
}

void CameraSource::on_error_thunk(void* context, CameraBackendErrorKind kind,
                                  std::int32_t hresult) noexcept {
    auto* source = static_cast<CameraSource*>(context);
    source->accepting_.store(false);
    source->pending_hresult_.store(hresult);
    source->pending_error_.store(kind);
    source->has_pending_error_.store(true);
}

void CameraSource::handle_sample(const CameraSample& sample) noexcept {
    if (!accepting_.load(std::memory_order_acquire) ||
        state_.load(std::memory_order_acquire) != CameraState::streaming) {
        return;
    }
    FrameRing* ring = active_ring_;
    if (ring == nullptr || sample.data == nullptr || sample.size == 0) {
        return;
    }
    if (sample.format != ring->format() || sample.width != ring->width() ||
        sample.height != ring->height()) {
        return;
    }

    const core::TimestampNs arrival = now();

    bool regressed = false;
    core::TimestampNs timestamp = arrival;
    {
        const std::lock_guard<std::mutex> lock(timestamp_mutex_);
        const bool first = !timestamp_anchored_;
        const bool usable = sample.has_source_timestamp;
        if (first) {
            timestamp_anchored_ = usable;
            anchor_source_ns_ = sample.source_timestamp_ns;
            anchor_clock_ns_ = arrival;
            last_source_ns_ = sample.source_timestamp_ns;
            regression_reported_ = false;
            timestamp = arrival > last_timestamp_ns_ ? arrival : last_timestamp_ns_ + 1;
        } else if (!usable) {
            // No source clock: use arrival and leave the anchor untouched.
            regression_reported_ = false;
            timestamp = arrival > last_timestamp_ns_ ? arrival : last_timestamp_ns_ + 1;
        } else if (sample.source_timestamp_ns > last_source_ns_) {
            timestamp = anchor_clock_ns_ + (sample.source_timestamp_ns - anchor_source_ns_);
            // A driver whose source clock runs faster than wall time must not
            // place a frame in the future relative to its arrival.
            if (timestamp > arrival) {
                timestamp = arrival;
            }
            if (timestamp <= last_timestamp_ns_) {
                timestamp = arrival > last_timestamp_ns_ ? arrival : last_timestamp_ns_ + 1;
            }
            last_source_ns_ = sample.source_timestamp_ns;
            regression_reported_ = false;
        } else {
            // Backwards or duplicate source time. Re-anchor to arrival and keep
            // the mapped timeline monotonic; report once per episode.
            regressed = usable && !regression_reported_;
            regression_reported_ = usable;
            anchor_source_ns_ = sample.source_timestamp_ns;
            anchor_clock_ns_ = arrival;
            last_source_ns_ = sample.source_timestamp_ns;
            timestamp = arrival > last_timestamp_ns_ ? arrival : last_timestamp_ns_ + 1;
        }
        last_timestamp_ns_ = timestamp;
    }

    if (regressed) {
        timestamp_resets_.fetch_add(1);
        emit(CameraEventKind::timestamp_regression, core::DiagnosticCode::cap_005,
             "source timestamps went backwards or repeated; re-anchored to arrival time");
    }

    std::optional<FrameWrite> write = ring->begin_write();
    if (!write) {
        return;
    }

    bool copied = true;
    for (std::size_t plane = 0; plane < kMaxFramePlanes; ++plane) {
        const std::size_t source_size = sample.plane_sizes[plane];
        if (source_size == 0) {
            continue;
        }
        const std::size_t source_offset = sample.plane_offsets[plane];
        if (source_offset > sample.size || source_size > sample.size - source_offset) {
            copied = false;
            break;
        }
        std::uint8_t* destination = write->plane(plane);
        const std::uint32_t destination_stride = write->stride(plane);
        if (destination == nullptr || destination_stride == 0) {
            copied = false;
            break;
        }
        const std::uint32_t source_stride =
            sample.strides[plane] != 0 ? sample.strides[plane] : destination_stride;
        const std::uint8_t* source = sample.data + source_offset;
        if (source_stride == destination_stride) {
            std::memcpy(destination, source, source_size);
            continue;
        }
        const std::size_t rows = source_size / source_stride;
        const std::size_t row_bytes =
            std::min<std::size_t>(source_stride, destination_stride);
        for (std::size_t row = 0; row < rows; ++row) {
            std::memcpy(destination + row * destination_stride,
                        source + row * source_stride, row_bytes);
        }
    }
    if (!copied) {
        ring->abandon(*write);
        return;
    }

    write->frame().color_space = sample.color_space;
    if (!ring->publish(*write, timestamp, arrival, sample.mirrored)) {
        return;
    }

    {
        const std::uint64_t previous_frame = last_frame_ns_.load(std::memory_order_acquire);
        if (previous_frame != 0 && arrival > previous_frame) {
            const std::uint64_t delta = arrival - previous_frame;
            const std::uint64_t previous = observed_interval_ns_.load(std::memory_order_relaxed);
            observed_interval_ns_.store(previous == 0 ? delta : (previous * 7u + delta) / 8u,
                                        std::memory_order_relaxed);
        }
    }
    last_frame_ns_.store(arrival, std::memory_order_release);
    if (restart_attempted_.load(std::memory_order_acquire)) {
        // A restart is only considered successful after a sustained healthy
        // window; otherwise a burst of a few frames would reset the watchdog
        // and cause an endless restart loop on an over-bandwidth mode.
        const core::TimestampNs restart_ns = restart_ns_.load(std::memory_order_acquire);
        if (restart_ns != 0 && arrival > restart_ns + kHealthyWindowNs) {
            restart_attempted_.store(false, std::memory_order_release);
            restart_ns_.store(0, std::memory_order_release);
        }
    }
    frames_published_.fetch_add(1, std::memory_order_relaxed);
    publish_counter_.fetch_add(1, std::memory_order_release);
    {
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        publish_latency_.add(now() - arrival);
    }
}

void CameraSource::process_pending_error(core::TimestampNs current) {
    if (!has_pending_error_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    const CameraBackendErrorKind kind = pending_error_.load(std::memory_order_relaxed);
    const std::int32_t hresult = pending_hresult_.load(std::memory_order_relaxed);
    accepting_.store(false);
    backend_.stop();

    switch (kind) {
        case CameraBackendErrorKind::device_lost:
            state_.store(CameraState::device_lost);
            last_frame_ns_.store(0);
            observed_interval_ns_.store(0);
            restart_attempted_.store(false);
            restart_ns_.store(0);
            reconnect_failure_reported_ = false;
            next_reconnect_ns_ = current + config_.reconnect_poll_ns;
            emit(CameraEventKind::device_lost, core::DiagnosticCode::cap_001,
                 "camera disappeared mid-stream; polling for its return (" +
                     format_hresult(hresult) + ")");
            break;
        case CameraBackendErrorKind::permission_denied:
            state_.store(CameraState::permission_denied);
            emit(CameraEventKind::permission_denied, core::DiagnosticCode::cap_006,
                 "camera permission denied by the OS (" + format_hresult(hresult) + ")");
            break;
        case CameraBackendErrorKind::busy:
            state_.store(CameraState::busy);
            reconnect_failure_reported_ = false;
            next_reconnect_ns_ = current + config_.reconnect_poll_ns;
            emit(CameraEventKind::device_busy, core::DiagnosticCode::cap_002,
                 "camera was taken by another application (Teams, Zoom, OBS or a browser) (" +
                     format_hresult(hresult) + "); polling for its return");
            break;
        case CameraBackendErrorKind::read_error:
            state_.store(CameraState::stalled);
            emit(CameraEventKind::read_error, core::DiagnosticCode::cap_004,
                 "camera read failed (" + format_hresult(hresult) + ")");
            break;
        case CameraBackendErrorKind::internal:
            state_.store(CameraState::stalled);
            emit(CameraEventKind::read_error, core::DiagnosticCode::cap_004,
                 "capture backend error (" + format_hresult(hresult) + ")");
            break;
    }
}

std::uint64_t CameraSource::expected_interval_ns() const noexcept {
    CameraMode mode;
    {
        const std::lock_guard<std::mutex> lock(mode_mutex_);
        mode = mode_;
    }
    const double fps = mode.fps() > 0.0 ? mode.fps() : 30.0;
    return static_cast<std::uint64_t>(1'000'000'000.0 / fps);
}

void CameraSource::check_watchdog(core::TimestampNs current) {
    if (!config_.restart_on_stall) {
        return;
    }
    const core::TimestampNs last = last_frame_ns_.load(std::memory_order_acquire);
    if (last == 0) {
        return;
    }
    if (current < watchdog_grace_until_.load(std::memory_order_acquire)) {
        return;
    }
    const std::uint64_t nominal = expected_interval_ns();
    const std::uint64_t observed = observed_interval_ns_.load(std::memory_order_relaxed);
    const std::uint64_t base = observed != 0 ? std::max(observed, nominal) : nominal;
    std::uint64_t deadline_ns = base * static_cast<std::uint64_t>(config_.watchdog_multiplier);
    deadline_ns = std::max(deadline_ns, config_.watchdog_min_deadline_ns);
    deadline_ns = std::min(deadline_ns, config_.watchdog_max_deadline_ns);
    if (current <= last + deadline_ns) {
        return;
    }

    if (!restart_attempted_.load(std::memory_order_acquire)) {
        emit(CameraEventKind::stream_stalled, core::DiagnosticCode::cap_004,
             "no frames for the watchdog deadline; restarting the stream once");
        backend_.stop();
        accepting_.store(false);
        {
            // A restarted reader restarts its own timestamp base.
            const std::lock_guard<std::mutex> lock(timestamp_mutex_);
            timestamp_anchored_ = false;
            regression_reported_ = false;
        }
        const core::Status status = open_and_start();
        if (status.ok()) {
            restart_attempted_.store(true, std::memory_order_release);
            restart_ns_.store(current, std::memory_order_release);
            last_frame_ns_.store(current, std::memory_order_release);
            watchdog_grace_until_.store(
                current + std::max(config_.startup_grace_ns,
                                   expected_interval_ns() * config_.watchdog_multiplier),
                std::memory_order_release);
            restarts_.fetch_add(1);
            emit(CameraEventKind::stream_restarted, core::DiagnosticCode::unknown,
                 "watchdog restarted the capture stream");
        } else {
            accepting_.store(false);
            state_.store(CameraState::stalled);
            emit(CameraEventKind::stream_stalled, core::DiagnosticCode::cap_004,
                 "watchdog restart failed: " + status.message());
        }
        return;
    }

    // The restart did not produce a healthy stream. Step down the negotiated
    // mode before giving up: on USB 2 a camera can advertise an uncompressed
    // mode that it cannot actually sustain.
    accepting_.store(false);
    backend_.stop();
    if (demote_mode("stream stalled again after one restart")) {
        return;
    }
    const CameraState after = state_.load();
    if (after != CameraState::busy && after != CameraState::permission_denied) {
        state_.store(CameraState::stalled);
    }
    emit(CameraEventKind::stream_stalled, core::DiagnosticCode::cap_004,
         "stream is still stalled after one restart; stopping");
}

void CameraSource::check_shortfall(core::TimestampNs current) {
    if (shortfall_checked_ || candidates_.empty() || mode_start_ns_ == 0) {
        return;
    }
    if (current < mode_start_ns_ + config_.demote_after_ns) {
        return;
    }
    const double elapsed_s = static_cast<double>(current - mode_start_ns_) / 1'000'000'000.0;
    if (elapsed_s <= 0.0) {
        return;
    }
    const double target = negotiated_mode().fps();
    const std::uint64_t delivered =
        frames_published_.load(std::memory_order_relaxed) - frames_at_mode_start_;
    const double actual = static_cast<double>(delivered) / elapsed_s;
    const double threshold =
        target * (static_cast<double>(config_.shortfall_percent) / 100.0);
    if (target > 0.0 && actual < threshold) {
        const std::string detail = "delivered " + std::to_string(static_cast<int>(actual)) +
                                   " fps of " + std::to_string(static_cast<int>(target));
        if (demote_mode(detail)) {
            return;
        }
        shortfall_checked_ = true;
        emit(CameraEventKind::stream_stalled, core::DiagnosticCode::cap_004,
             detail + "; no lower capture mode is available");
        return;
    }
    shortfall_checked_ = true;
}

bool CameraSource::demote_mode(const std::string& reason) {
    if (candidate_index_ + 1 >= candidates_.size()) {
        shortfall_checked_ = true;
        return false;
    }
    const CameraMode previous = negotiated_mode();
    ++candidate_index_;
    const CameraMode next = candidates_[candidate_index_];
    emit(CameraEventKind::format_rejected, core::DiagnosticCode::cap_003,
         "stepping down from " + describe_mode(previous) + " to " + describe_mode(next) + ": " +
             reason);
    accepting_.store(false);
    backend_.stop();
    state_.store(CameraState::opening);
    const core::Status status = open_and_start();
    if (!status.ok()) {
        const CameraState after = state_.load();
        if (after != CameraState::busy && after != CameraState::permission_denied &&
            after != CameraState::device_lost) {
            state_.store(CameraState::stalled);
        }

        emit(CameraEventKind::stream_stalled, core::DiagnosticCode::cap_004,
             "stepping down failed: " + status.message());
        return false;
    }
    emit(CameraEventKind::stream_restarted, core::DiagnosticCode::unknown,
         "stepped down to " + describe_mode(negotiated_mode()));
    return true;
}

void CameraSource::maybe_reconnect(core::TimestampNs current) {
    if (!config_.reconnect || current < next_reconnect_ns_) {
        return;
    }
    next_reconnect_ns_ = current + config_.reconnect_poll_ns;

    std::vector<CameraDevice> devices;
    if (!backend_.enumerate(devices).ok()) {
        return;
    }
    const bool present = std::any_of(
        devices.begin(), devices.end(),
        [this](const CameraDevice& device) { return device.id == device_id_; });
    if (!present) {
        return;
    }

    state_.store(CameraState::opening);
    const core::Status status = open_and_start();
    if (status.ok()) {
        reconnects_.fetch_add(1);
        reconnect_failure_reported_ = false;
        emit(CameraEventKind::device_restored, core::DiagnosticCode::unknown,
             "camera reopened after reconnect");
        return;
    }

    const CameraState failed_state = state_.load();
    if (failed_state != CameraState::busy && failed_state != CameraState::permission_denied) {
        state_.store(CameraState::device_lost);
    }
    if (!reconnect_failure_reported_) {
        reconnect_failure_reported_ = true;
        emit(CameraEventKind::reconnect_failed, core::DiagnosticCode::cap_007,
             "camera returned but reopen failed: " + status.message());
    }
}

bool CameraSource::produce() {
    const core::TimestampNs current = now();
    process_pending_error(current);

    switch (state_.load(std::memory_order_acquire)) {
        case CameraState::streaming:
            check_watchdog(current);
            if (state_.load(std::memory_order_acquire) == CameraState::streaming) {
                check_shortfall(current);
            }
            break;
        case CameraState::device_lost:
        case CameraState::busy:
            maybe_reconnect(current);
            break;
        default:
            break;
    }

    const std::uint64_t published = publish_counter_.load(std::memory_order_acquire);
    if (published == seen_publish_counter_) {
        return false;
    }
    seen_publish_counter_ = published;
    return true;
}

SourceInfo CameraSource::info() const noexcept {
    SourceInfo result;
    result.name = device_name_;
    const std::lock_guard<std::mutex> lock(mode_mutex_);
    result.width = mode_.width;
    result.height = mode_.height;
    result.format = mode_.format;
    return result;
}

CameraMode CameraSource::negotiated_mode() const {
    const std::lock_guard<std::mutex> lock(mode_mutex_);
    return mode_;
}

ColorSpace CameraSource::color_space() const {
    const std::lock_guard<std::mutex> lock(mode_mutex_);
    return color_space_;
}

CaptureLatencyHistogram CameraSource::publish_latency() const {
    const std::lock_guard<std::mutex> lock(stats_mutex_);
    return publish_latency_;
}

std::vector<CameraEvent> CameraSource::take_events() {
    const std::lock_guard<std::mutex> lock(event_mutex_);
    std::vector<CameraEvent> events;
    events.swap(events_);
    return events;
}

void CameraSource::emit(CameraEventKind kind, core::DiagnosticCode code, std::string detail) {
    CameraEvent event;
    event.kind = kind;
    event.code = code;
    event.detail = std::move(detail);
    const std::lock_guard<std::mutex> lock(event_mutex_);
    if (events_.size() >= kMaxPendingEvents) {
        events_.erase(events_.begin());
    }
    events_.push_back(std::move(event));
}

void CameraSource::suspend() {
    const CameraState state = state_.load();
    if (state == CameraState::closed || state == CameraState::suspended) {
        return;
    }
    accepting_.store(false);
    backend_.stop();
    {
        const std::lock_guard<std::mutex> lock(timestamp_mutex_);
        timestamp_anchored_ = false;
        regression_reported_ = false;
        last_source_ns_ = 0;
        last_timestamp_ns_ = 0;
    }
    last_frame_ns_.store(0);
    observed_interval_ns_.store(0);
    restart_attempted_.store(false);
    restart_ns_.store(0);
    next_reconnect_ns_ = 0;
    shortfall_checked_ = true;
    mode_start_ns_ = 0;
    state_.store(CameraState::suspended);
    emit(CameraEventKind::suspended, core::DiagnosticCode::unknown,
         "capture suspended; the device is released");
}

core::Status CameraSource::resume() {
    if (state_.load() == CameraState::streaming) {
        return core::Status::success();
    }
    state_.store(CameraState::opening);
    const core::Status status = open_and_start();
    if (status.ok()) {
        emit(CameraEventKind::resumed, core::DiagnosticCode::unknown,
             "capture resumed with the previous configuration");
    }
    return status;
}

} // namespace avatar::capture
