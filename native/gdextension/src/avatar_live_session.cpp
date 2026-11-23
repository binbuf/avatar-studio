#include "avatar_live_session.h"

#include "avatar/core/diagnostics.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <string>

using namespace godot;

namespace {

[[nodiscard]] String string_view_text(std::string_view value) {
    return String(value.data());
}

} // namespace

AvatarLiveSession::AvatarLiveSession() = default;

AvatarLiveSession::~AvatarLiveSession() {
    global_hotkey_.stop();
}

void AvatarLiveSession::_bind_methods() {
    ClassDB::bind_method(D_METHOD("start_live", "now_ns"), &AvatarLiveSession::start_live);
    ClassDB::bind_method(D_METHOD("stop_live", "now_ns"), &AvatarLiveSession::stop_live);
    ClassDB::bind_method(D_METHOD("is_live"), &AvatarLiveSession::is_live);
    ClassDB::bind_method(D_METHOD("tick", "now_ns"), &AvatarLiveSession::tick);
    ClassDB::bind_method(D_METHOD("report_frame", "now_ns", "frame_time_ms"),
                         &AvatarLiveSession::report_frame);

    ClassDB::bind_method(D_METHOD("set_camera_status", "status"),
                         &AvatarLiveSession::set_camera_status);
    ClassDB::bind_method(D_METHOD("set_face_status", "status", "confidence"),
                         &AvatarLiveSession::set_face_status);
    ClassDB::bind_method(D_METHOD("set_tracker_available", "available"),
                         &AvatarLiveSession::set_tracker_available);
    ClassDB::bind_method(D_METHOD("set_output_state", "state"),
                         &AvatarLiveSession::set_output_state);
    ClassDB::bind_method(D_METHOD("set_inputs", "inputs"), &AvatarLiveSession::set_inputs);

    ClassDB::bind_method(D_METHOD("bind_output", "manager"), &AvatarLiveSession::bind_output);
    ClassDB::bind_method(D_METHOD("toggle_panic", "reason"), &AvatarLiveSession::toggle_panic);
    ClassDB::bind_method(D_METHOD("set_panic", "active", "reason", "now_ns"),
                         &AvatarLiveSession::set_panic);
    ClassDB::bind_method(D_METHOD("is_panic"), &AvatarLiveSession::is_panic);
    ClassDB::bind_method(D_METHOD("get_panic_reason"), &AvatarLiveSession::get_panic_reason);

    ClassDB::bind_method(D_METHOD("get_status"), &AvatarLiveSession::get_status);
    ClassDB::bind_method(D_METHOD("get_hud", "now_ns"), &AvatarLiveSession::get_hud);
    ClassDB::bind_method(D_METHOD("get_hud_text", "now_ns"), &AvatarLiveSession::get_hud_text);
    ClassDB::bind_method(D_METHOD("get_gate_signal", "now_ns"),
                         &AvatarLiveSession::get_gate_signal);

    ClassDB::bind_method(D_METHOD("request_edit", "kind"), &AvatarLiveSession::request_edit);
    ClassDB::bind_method(D_METHOD("handle_hotkey", "action"), &AvatarLiveSession::handle_hotkey);

    ClassDB::bind_method(D_METHOD("start_global_hotkey"), &AvatarLiveSession::start_global_hotkey);
    ClassDB::bind_method(D_METHOD("stop_global_hotkey"), &AvatarLiveSession::stop_global_hotkey);
    ClassDB::bind_method(D_METHOD("consume_global_hotkey"),
                         &AvatarLiveSession::consume_global_hotkey);
    ClassDB::bind_method(D_METHOD("get_global_hotkey_status"),
                         &AvatarLiveSession::get_global_hotkey_status);

    ClassDB::bind_method(D_METHOD("set_debug_state", "name"), &AvatarLiveSession::set_debug_state);
    ClassDB::bind_method(D_METHOD("clear_debug_state"), &AvatarLiveSession::clear_debug_state);
}

void AvatarLiveSession::start_live(int64_t now_ns) {
    session_.start(static_cast<avatar::core::TimestampNs>(std::max<int64_t>(0, now_ns)));
}

void AvatarLiveSession::stop_live(int64_t now_ns) {
    session_.stop(static_cast<avatar::core::TimestampNs>(std::max<int64_t>(0, now_ns)));
    apply_panic(false, "none");
}

bool AvatarLiveSession::is_live() const {
    return session_.is_live();
}

void AvatarLiveSession::tick(int64_t now_ns) {
    session_.tick(static_cast<avatar::core::TimestampNs>(std::max<int64_t>(0, now_ns)));
}

void AvatarLiveSession::report_frame(int64_t now_ns, double frame_time_ms) {
    session_.report_frame(static_cast<avatar::core::TimestampNs>(std::max<int64_t>(0, now_ns)),
                          frame_time_ms);
}

void AvatarLiveSession::set_camera_status(const String& status) {
    session_.set_camera(parse_camera(status));
}

void AvatarLiveSession::set_face_status(const String& status, double confidence) {
    const avatar::live::FaceState face = parse_face(status);
    session_.set_face(face, static_cast<float>(std::clamp(confidence, 0.0, 1.0)));
    if (face == avatar::live::FaceState::unavailable) {
        session_.set_tracker_available(false);
    }
}

void AvatarLiveSession::set_tracker_available(bool available) {
    session_.set_tracker_available(available);
}

void AvatarLiveSession::set_output_state(const String& state) {
    session_.set_output_state(parse_output(state));
}

void AvatarLiveSession::set_inputs(const Dictionary& inputs) {
    avatar::live::Inputs next = session_.inputs();
    if (inputs.has("camera")) {
        next.camera = parse_camera(inputs["camera"]);
    }
    if (inputs.has("face")) {
        next.face = parse_face(inputs["face"]);
    }
    if (inputs.has("tracker_available")) {
        next.tracker_available = static_cast<bool>(inputs["tracker_available"]);
    }
    if (inputs.has("confidence")) {
        next.confidence =
            static_cast<float>(std::clamp(static_cast<double>(inputs["confidence"]), 0.0, 1.0));
    }
    session_.set_inputs(next);
}

void AvatarLiveSession::bind_output(Object* manager) {
    output_manager_ = Object::cast_to<AvatarOutputManager>(manager);
}

Dictionary AvatarLiveSession::toggle_panic(const String& reason) {
    const bool active = session_.toggle_panic(parse_reason(reason));
    apply_panic(active, reason);
    Dictionary out;
    out["panic"] = active;
    out["reason"] = get_panic_reason();
    return out;
}

Dictionary AvatarLiveSession::set_panic(bool active, const String& reason, int64_t now_ns) {
    (void)now_ns;
    session_.set_panic(active, parse_reason(reason));
    apply_panic(active, reason);
    Dictionary out;
    out["panic"] = active;
    out["reason"] = get_panic_reason();
    return out;
}

bool AvatarLiveSession::is_panic() const {
    return session_.is_panic();
}

String AvatarLiveSession::get_panic_reason() const {
    return string_view_text(avatar::live::LiveSession::panic_reason_name(session_.panic_reason()));
}

Dictionary AvatarLiveSession::get_status() const {
    const avatar::live::Status status = session_.status();
    Dictionary out;
    out["code"] = format_code(status.code);
    out["severity"] = string_view_text(avatar::core::severity_name(status.severity));
    out["sentence"] = string_view_text(status.sentence);
    out["action"] = string_view_text(status.action);
    out["healthy"] = status.healthy;
    out["blocks_output"] = status.blocks_output;
    out["live"] = status.live;
    out["face_state"] = string_view_text(avatar::live::LiveSession::face_state_name(status.face));
    out["output_state"] =
        string_view_text(avatar::live::LiveSession::output_state_name(status.output));
    out["panic"] = session_.is_panic();
    return out;
}

Dictionary AvatarLiveSession::get_hud(int64_t now_ns) const {
    const auto stamp = static_cast<avatar::core::TimestampNs>(std::max<int64_t>(0, now_ns));
    const avatar::live::Metrics metrics = session_.metrics(stamp);
    Dictionary out;
    out["live"] = metrics.live;
    out["panic"] = metrics.panic;
    out["elapsed_s"] = metrics.elapsed_s;
    out["fps"] = metrics.fps;
    out["frame_time_ms"] = metrics.frame_time_ms;
    out["confidence"] = metrics.confidence;
    out["face_state"] = string_view_text(avatar::live::LiveSession::face_state_name(metrics.face));
    out["output_state"] =
        string_view_text(avatar::live::LiveSession::output_state_name(metrics.output));
    out["status"] = get_status();
    out["hud_text"] = get_hud_text(now_ns);
    out["global_hotkey"] = get_global_hotkey_status();
    return out;
}

String AvatarLiveSession::get_hud_text(int64_t now_ns) const {
    return String(
        session_.hud_text(static_cast<avatar::core::TimestampNs>(std::max<int64_t>(0, now_ns)))
            .c_str());
}

Dictionary AvatarLiveSession::get_gate_signal(int64_t now_ns) const {
    const avatar::live::Inputs& inputs = session_.inputs();
    const bool camera_ok = inputs.camera == avatar::live::CameraState::ok;
    const bool face_present = inputs.face != avatar::live::FaceState::not_visible &&
                              inputs.face != avatar::live::FaceState::unavailable;
    const bool result_valid =
        camera_ok && inputs.tracker_available && face_present && !session_.is_panic();
    const bool features_valid = inputs.face == avatar::live::FaceState::tracked ||
                                inputs.face == avatar::live::FaceState::degraded;
    Dictionary out;
    out["result_valid"] = result_valid;
    out["features_valid"] = features_valid;
    out["frame_available"] = true;
    out["source_timestamp_ns"] = std::max<int64_t>(0, now_ns - 16'000'000);
    out["render_time_ns"] = std::max<int64_t>(0, now_ns);
    out["panic"] = session_.is_panic();
    return out;
}

Dictionary AvatarLiveSession::request_edit(const String& kind) const {
    Dictionary out;
    if (!is_edit_kind(kind)) {
        out["allowed"] = false;
        out["requires_stop"] = false;
        out["sentence"] = String("That setting is not part of the Live surface.");
        out["action"] = String();
        out["kind"] = kind;
        return out;
    }
    const avatar::live::EditDecision decision = session_.request_edit(parse_edit(kind));
    out["allowed"] = decision.allowed;
    out["requires_stop"] = decision.requires_stop;
    out["sentence"] = String(decision.sentence.c_str());
    out["action"] = String(decision.action.c_str());
    out["kind"] = kind;
    return out;
}

bool AvatarLiveSession::handle_hotkey(const String& action) {
    if (action != String("panic")) {
        return false;
    }
    const bool handled = session_.handle_hotkey(avatar::live::Hotkey::panic);
    if (handled) {
        apply_panic(session_.is_panic(), "hotkey");
    }
    return handled;
}

void AvatarLiveSession::start_global_hotkey() {
    global_hotkey_.start();
}

void AvatarLiveSession::stop_global_hotkey() {
    global_hotkey_.stop();
}

bool AvatarLiveSession::consume_global_hotkey() {
    if (!global_hotkey_.consume_pressed()) {
        return false;
    }
    (void)session_.toggle_panic(avatar::live::PanicReason::global_hotkey);
    apply_panic(session_.is_panic(), "global_hotkey");
    return true;
}

Dictionary AvatarLiveSession::get_global_hotkey_status() const {
    Dictionary out;
    out["registered"] = global_hotkey_.is_registered();
    out["available"] = global_hotkey_.is_registered();
    out["detail"] = String(global_hotkey_.detail().c_str());
    return out;
}

void AvatarLiveSession::set_debug_state(const String& name) {
    avatar::live::Inputs inputs = session_.inputs();
    if (name == String("healthy")) {
        inputs.camera = avatar::live::CameraState::ok;
        inputs.face = avatar::live::FaceState::tracked;
        inputs.tracker_available = true;
        inputs.confidence = 0.92F;
    } else if (name == String("camera_lost")) {
        inputs.camera = avatar::live::CameraState::lost;
        inputs.face = avatar::live::FaceState::tracked;
        inputs.tracker_available = true;
        inputs.confidence = 0.9F;
    } else if (name == String("face_not_visible")) {
        inputs.camera = avatar::live::CameraState::ok;
        inputs.face = avatar::live::FaceState::not_visible;
        inputs.tracker_available = true;
        inputs.confidence = 0.0F;
    } else if (name == String("features_hidden")) {
        inputs.camera = avatar::live::CameraState::ok;
        inputs.face = avatar::live::FaceState::features_hidden;
        inputs.tracker_available = true;
        inputs.confidence = 0.7F;
    } else if (name == String("degraded")) {
        inputs.camera = avatar::live::CameraState::ok;
        inputs.face = avatar::live::FaceState::degraded;
        inputs.tracker_available = true;
        inputs.confidence = 0.35F;
    } else if (name == String("tracker_unavailable")) {
        inputs.camera = avatar::live::CameraState::ok;
        inputs.face = avatar::live::FaceState::unavailable;
        inputs.tracker_available = false;
        inputs.confidence = 0.0F;
    } else {
        inputs.camera = avatar::live::CameraState::ok;
        inputs.face = avatar::live::FaceState::unavailable;
        inputs.tracker_available = false;
        inputs.confidence = 0.0F;
    }
    session_.set_inputs(inputs);
}

void AvatarLiveSession::clear_debug_state() {
    set_debug_state("default");
}

void AvatarLiveSession::apply_panic(bool active, const String& reason) {
    (void)reason;
    if (output_manager_.is_valid()) {
        output_manager_->set_panic(
            active, static_cast<int64_t>(Time::get_singleton()->get_ticks_usec() * 1000ULL));
    }
}

String AvatarLiveSession::format_code(avatar::core::DiagnosticCode code) {
    if (code == avatar::core::DiagnosticCode::unknown) {
        return String();
    }
    return String(avatar::core::format_diagnostic_code(code).c_str());
}

avatar::live::CameraState AvatarLiveSession::parse_camera(const String& value) {
    if (value == String("lost")) {
        return avatar::live::CameraState::lost;
    }
    if (value == String("busy")) {
        return avatar::live::CameraState::busy;
    }
    if (value == String("denied")) {
        return avatar::live::CameraState::denied;
    }
    if (value == String("none")) {
        return avatar::live::CameraState::none;
    }
    return avatar::live::CameraState::ok;
}

avatar::live::FaceState AvatarLiveSession::parse_face(const String& value) {
    if (value == String("not_visible")) {
        return avatar::live::FaceState::not_visible;
    }
    if (value == String("features_hidden")) {
        return avatar::live::FaceState::features_hidden;
    }
    if (value == String("degraded")) {
        return avatar::live::FaceState::degraded;
    }
    if (value == String("unavailable")) {
        return avatar::live::FaceState::unavailable;
    }
    return avatar::live::FaceState::tracked;
}

avatar::live::OutputState AvatarLiveSession::parse_output(const String& value) {
    if (value == String("avatar") || value == String("live") || value == String("held")) {
        return avatar::live::OutputState::live;
    }
    if (value == String("blank") || value == String("blanked")) {
        return avatar::live::OutputState::blanked;
    }
    if (value == String("hidden")) {
        return avatar::live::OutputState::hidden;
    }
    if (value == String("stopped")) {
        return avatar::live::OutputState::stopped;
    }
    return avatar::live::OutputState::placeholder;
}

avatar::live::PanicReason AvatarLiveSession::parse_reason(const String& value) {
    if (value == String("button")) {
        return avatar::live::PanicReason::button;
    }
    if (value == String("hotkey")) {
        return avatar::live::PanicReason::hotkey;
    }
    if (value == String("global_hotkey")) {
        return avatar::live::PanicReason::global_hotkey;
    }
    if (value == String("external")) {
        return avatar::live::PanicReason::external;
    }
    return avatar::live::PanicReason::none;
}

avatar::live::EditKind AvatarLiveSession::parse_edit(const String& value) {
    if (value == String("calibration")) {
        return avatar::live::EditKind::calibration;
    }
    if (value == String("camera")) {
        return avatar::live::EditKind::camera;
    }
    if (value == String("resolution")) {
        return avatar::live::EditKind::resolution;
    }
    if (value == String("transport")) {
        return avatar::live::EditKind::transport;
    }
    if (value == String("outfit")) {
        return avatar::live::EditKind::outfit;
    }
    if (value == String("appearance")) {
        return avatar::live::EditKind::appearance;
    }
    return avatar::live::EditKind::avatar;
}

bool AvatarLiveSession::is_edit_kind(const String& value) {
    return value == String("avatar") || value == String("calibration") ||
           value == String("camera") || value == String("resolution") ||
           value == String("transport") || value == String("outfit") ||
           value == String("appearance");
}
