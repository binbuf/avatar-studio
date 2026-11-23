#include "avatar/live/live_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>

namespace avatar::live {
namespace {

constexpr TimestampNs kFpsWindowNs = 1'000'000'000;

[[nodiscard]] std::string format_elapsed(double seconds) {
    const auto total = static_cast<std::uint64_t>(std::max(0.0, seconds));
    const auto hours = total / 3600U;
    const auto minutes = (total % 3600U) / 60U;
    const auto secs = total % 60U;
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%02llu:%02llu:%02llu",
                  static_cast<unsigned long long>(hours), static_cast<unsigned long long>(minutes),
                  static_cast<unsigned long long>(secs));
    return std::string(buffer);
}

[[nodiscard]] std::string confidence_text(const Inputs& inputs) {
    if (!inputs.tracker_available || inputs.face == FaceState::unavailable) {
        return "-";
    }
    const long percent = std::lround(std::clamp(inputs.confidence, 0.0F, 1.0F) * 100.0F);
    return std::to_string(percent) + "%";
}

[[nodiscard]] std::string edit_target(EditKind kind) {
    switch (kind) {
        case EditKind::avatar:
            return "avatar";
        case EditKind::calibration:
            return "face calibration";
        case EditKind::camera:
            return "camera";
        case EditKind::resolution:
            return "output resolution";
        case EditKind::transport:
            return "transport";
        case EditKind::outfit:
            return "outfit";
        case EditKind::appearance:
            return "appearance";
    }
    return "setting";
}

} // namespace

void LiveSession::start(TimestampNs now_ns) {
    live_ = true;
    panic_ = false;
    panic_reason_ = PanicReason::none;
    output_ = OutputState::placeholder;
    start_ns_ = now_ns;
    last_tick_ns_ = now_ns;
    last_frame_ns_ = now_ns;
    frame_time_ms_ = 0.0;
    clear_frames();
}

void LiveSession::stop(TimestampNs now_ns) {
    live_ = false;
    panic_ = false;
    panic_reason_ = PanicReason::none;
    output_ = OutputState::stopped;
    start_ns_ = 0;
    last_tick_ns_ = now_ns;
    last_frame_ns_ = 0;
    frame_time_ms_ = 0.0;
    clear_frames();
}

void LiveSession::tick(TimestampNs now_ns) {
    last_tick_ns_ = now_ns;
}

void LiveSession::report_frame(TimestampNs now_ns, double frame_time_ms) {
    if (!live_) {
        return;
    }
    if (frame_time_ms >= 0.0 && std::isfinite(frame_time_ms)) {
        frame_time_ms_ =
            frame_time_ms_ == 0.0 ? frame_time_ms : frame_time_ms_ * 0.9 + frame_time_ms * 0.1;
    }
    last_frame_ns_ = now_ns;
    frame_times_[frame_head_] = now_ns;
    frame_head_ = (frame_head_ + 1) % kFrameWindow;
    frame_count_ = std::min(frame_count_ + 1, kFrameWindow);
}

void LiveSession::set_face(FaceState state, float confidence) noexcept {
    inputs_.face = state;
    inputs_.confidence = std::clamp(confidence, 0.0F, 1.0F);
    if (state == FaceState::unavailable) {
        inputs_.tracker_available = false;
    }
}

void LiveSession::set_output_state(OutputState state) noexcept {
    if (panic_) {
        output_ = OutputState::hidden;
        return;
    }
    output_ = state;
}

void LiveSession::set_panic(bool active, PanicReason reason) noexcept {
    if (active == panic_) {
        return;
    }
    panic_ = active;
    panic_reason_ = active ? reason : PanicReason::none;
    output_ = active ? OutputState::hidden : OutputState::blanked;
}

bool LiveSession::toggle_panic(PanicReason reason) noexcept {
    set_panic(!panic_, reason);
    return panic_;
}

Status LiveSession::status() const noexcept {
    Status status;
    status.live = live_;
    status.face = inputs_.face;
    status.output = panic_ ? OutputState::hidden : output_;

    if (!live_) {
        status.healthy = true;
        status.blocks_output = true;
        status.sentence = "Live is stopped";
        status.action = "Choose Go Live to start the output.";
        return status;
    }

    if (panic_) {
        status.healthy = false;
        status.blocks_output = true;
        status.code = DiagnosticCode::out_007;
        status.severity = Severity::info;
        status.sentence = "Output is hidden";
        status.action = "Press Ctrl/Cmd+Shift+H or choose Restore output to show the avatar again.";
        return status;
    }

    switch (inputs_.camera) {
        case CameraState::lost:
            status.healthy = false;
            status.blocks_output = true;
            status.code = DiagnosticCode::cap_001;
            status.severity = Severity::error;
            status.sentence = "Camera disconnected";
            status.action = "Reconnect the camera; the output stays blank until it returns.";
            return status;
        case CameraState::busy:
            status.healthy = false;
            status.blocks_output = true;
            status.code = DiagnosticCode::cap_002;
            status.severity = Severity::error;
            status.sentence = "Another app is using this camera";
            status.action = "Close the other app, then retry.";
            return status;
        case CameraState::denied:
            status.healthy = false;
            status.blocks_output = true;
            status.code = DiagnosticCode::cap_006;
            status.severity = Severity::error;
            status.sentence = "Camera access is blocked by Windows/macOS";
            status.action = "Open the OS privacy settings and allow camera access.";
            return status;
        case CameraState::none:
            status.healthy = false;
            status.blocks_output = true;
            status.code = DiagnosticCode::cap_008;
            status.severity = Severity::info;
            status.sentence = "No camera found";
            status.action = "Connect a camera, then rescan.";
            return status;
        case CameraState::ok:
            break;
    }

    if (!inputs_.tracker_available) {
        status.healthy = false;
        status.blocks_output = true;
        status.code = DiagnosticCode::trk_007;
        status.severity = Severity::error;
        status.sentence = "Face tracking isn't running";
        status.action = "Restart the app or check the tracker.";
        return status;
    }

    switch (inputs_.face) {
        case FaceState::not_visible:
            status.healthy = false;
            status.blocks_output = true;
            status.code = DiagnosticCode::trk_001;
            status.severity = Severity::info;
            status.sentence = "Can't see your face";
            status.action = "Face the camera and check the lighting.";
            return status;
        case FaceState::features_hidden:
            status.healthy = false;
            status.blocks_output = true;
            status.code = DiagnosticCode::trk_008;
            status.severity = Severity::warning;
            status.sentence = "Can't see your eyes and mouth";
            status.action = "Remove sunglasses or a hand, and face the camera.";
            return status;
        case FaceState::degraded:
            status.healthy = false;
            status.blocks_output = false;
            status.code = DiagnosticCode::trk_004;
            status.severity = Severity::warning;
            status.sentence = "Face tracking is struggling";
            status.action = "Lower the capture resolution or add brighter light.";
            return status;
        case FaceState::unavailable:
            status.healthy = false;
            status.blocks_output = true;
            status.code = DiagnosticCode::trk_007;
            status.severity = Severity::error;
            status.sentence = "Face tracking isn't running";
            status.action = "Restart the app or check the tracker.";
            return status;
        case FaceState::tracked:
            break;
    }

    if (output_ == OutputState::blanked) {
        status.healthy = false;
        status.blocks_output = true;
        status.code = DiagnosticCode::out_007;
        status.severity = Severity::info;
        status.sentence = "The output is blank";
        status.action = "Check the camera and face tracking; the output resumes automatically.";
        return status;
    }

    return status;
}

double LiveSession::elapsed_seconds(TimestampNs now_ns) const noexcept {
    if (!live_ || now_ns <= start_ns_) {
        return 0.0;
    }
    return static_cast<double>(now_ns - start_ns_) / 1'000'000'000.0;
}

Metrics LiveSession::metrics(TimestampNs now_ns) const noexcept {
    Metrics metrics;
    metrics.live = live_;
    metrics.panic = panic_;
    metrics.elapsed_s = elapsed_seconds(now_ns);
    metrics.frame_time_ms = frame_time_ms_;
    metrics.confidence = inputs_.confidence;
    metrics.face = inputs_.face;
    metrics.output = panic_ ? OutputState::hidden : output_;

    if (frame_count_ >= 2 && live_) {
        TimestampNs oldest = now_ns;
        std::size_t in_window = 0;
        for (std::size_t i = 0; i < frame_count_; ++i) {
            const TimestampNs stamp = frame_times_[i];
            if (stamp != 0 && now_ns >= stamp && now_ns - stamp <= kFpsWindowNs) {
                oldest = std::min(oldest, stamp);
                ++in_window;
            }
        }
        if (in_window >= 2 && now_ns > oldest) {
            const double span_s = static_cast<double>(now_ns - oldest) / 1'000'000'000.0;
            metrics.fps = span_s > 0.0 ? static_cast<double>(in_window - 1) / span_s : 0.0;
        }
    }
    return metrics;
}

std::string LiveSession::hud_text(TimestampNs now_ns) const {
    const Metrics current = metrics(now_ns);
    const Status current_status = status();
    std::ostringstream out;
    out << (live_ ? "LIVE" : "STOPPED");
    out << "  " << format_elapsed(current.elapsed_s);
    out << "\nfps " << std::fixed << std::setprecision(0) << current.fps << "  frame "
        << std::setprecision(1) << current.frame_time_ms << " ms";
    out << "\ntracking " << confidence_text(inputs_) << "  face " << face_state_name(inputs_.face);
    out << "\noutput " << output_state_name(current.output);
    if (live_ && !current_status.healthy) {
        out << "\n"
            << core::format_diagnostic_code(current_status.code) << "  " << current_status.sentence;
        if (!current_status.action.empty()) {
            out << "\n-> " << current_status.action;
        }
    }
    return out.str();
}

EditDecision LiveSession::request_edit(EditKind kind) const {
    EditDecision decision;
    if (!live_) {
        decision.allowed = true;
        decision.sentence = "Editing is available while Live is stopped.";
        return decision;
    }
    if (kind == EditKind::outfit || kind == EditKind::appearance) {
        decision.allowed = true;
        decision.sentence = "Outfit and appearance changes are safe while live.";
        return decision;
    }
    decision.allowed = false;
    decision.requires_stop = true;
    decision.sentence = "Stop live output before changing the " + edit_target(kind) + ".";
    decision.action = "Stop and switch";
    return decision;
}

bool LiveSession::handle_hotkey(Hotkey hotkey) noexcept {
    switch (hotkey) {
        case Hotkey::panic:
            (void)toggle_panic(PanicReason::hotkey);
            return true;
    }
    return false;
}

std::string_view LiveSession::camera_state_name(CameraState state) noexcept {
    switch (state) {
        case CameraState::ok:
            return "ok";
        case CameraState::lost:
            return "lost";
        case CameraState::busy:
            return "busy";
        case CameraState::denied:
            return "denied";
        case CameraState::none:
            return "none";
    }
    return "unknown";
}

std::string_view LiveSession::face_state_name(FaceState state) noexcept {
    switch (state) {
        case FaceState::tracked:
            return "tracked";
        case FaceState::not_visible:
            return "not visible";
        case FaceState::features_hidden:
            return "eyes/mouth hidden";
        case FaceState::degraded:
            return "degraded";
        case FaceState::unavailable:
            return "unavailable";
    }
    return "unknown";
}

std::string_view LiveSession::output_state_name(OutputState state) noexcept {
    switch (state) {
        case OutputState::placeholder:
            return "window (placeholder)";
        case OutputState::live:
            return "window (live)";
        case OutputState::blanked:
            return "blank";
        case OutputState::hidden:
            return "hidden (panic)";
        case OutputState::stopped:
            return "stopped";
    }
    return "unknown";
}

std::string_view LiveSession::panic_reason_name(PanicReason reason) noexcept {
    switch (reason) {
        case PanicReason::none:
            return "none";
        case PanicReason::button:
            return "button";
        case PanicReason::hotkey:
            return "hotkey";
        case PanicReason::global_hotkey:
            return "global_hotkey";
        case PanicReason::external:
            return "external";
    }
    return "unknown";
}

std::string_view LiveSession::edit_kind_name(EditKind kind) noexcept {
    switch (kind) {
        case EditKind::avatar:
            return "avatar";
        case EditKind::calibration:
            return "calibration";
        case EditKind::camera:
            return "camera";
        case EditKind::resolution:
            return "resolution";
        case EditKind::transport:
            return "transport";
        case EditKind::outfit:
            return "outfit";
        case EditKind::appearance:
            return "appearance";
    }
    return "unknown";
}

void LiveSession::clear_frames() noexcept {
    frame_times_.fill(0);
    frame_head_ = 0;
    frame_count_ = 0;
}

} // namespace avatar::live
