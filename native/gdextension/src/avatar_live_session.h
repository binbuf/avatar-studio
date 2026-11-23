#pragma once

#include "avatar/live/live_session.h"
#include "avatar_output_manager.h"
#include "global_hotkey.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>

/// GDScript-facing Live state. This is a thin adapter over the framework-free
/// `avatar::live::LiveSession`: string parsing, dictionary building and the
/// optional OS hotkey only. It owns no pixels and does not read a camera.
class AvatarLiveSession : public godot::RefCounted {
    GDCLASS(AvatarLiveSession, godot::RefCounted)

public:
    AvatarLiveSession();
    ~AvatarLiveSession() override;

    void start_live(int64_t now_ns);
    void stop_live(int64_t now_ns);
    bool is_live() const;
    void tick(int64_t now_ns);
    void report_frame(int64_t now_ns, double frame_time_ms);

    void set_camera_status(const godot::String& status);
    void set_face_status(const godot::String& status, double confidence);
    void set_tracker_available(bool available);
    void set_output_state(const godot::String& state);
    void set_inputs(const godot::Dictionary& inputs);

    void bind_output(godot::Object* manager);
    godot::Dictionary toggle_panic(const godot::String& reason);
    godot::Dictionary set_panic(bool active, const godot::String& reason, int64_t now_ns);
    bool is_panic() const;
    godot::String get_panic_reason() const;

    godot::Dictionary get_status() const;
    godot::Dictionary get_hud(int64_t now_ns) const;
    godot::String get_hud_text(int64_t now_ns) const;
    godot::Dictionary get_gate_signal(int64_t now_ns) const;

    godot::Dictionary request_edit(const godot::String& kind) const;
    bool handle_hotkey(const godot::String& action);

    void start_global_hotkey();
    void stop_global_hotkey();
    bool consume_global_hotkey();
    godot::Dictionary get_global_hotkey_status() const;

    void set_debug_state(const godot::String& name);
    void clear_debug_state();

protected:
    static void _bind_methods();

private:
    static avatar::live::CameraState parse_camera(const godot::String& value);
    static avatar::live::FaceState parse_face(const godot::String& value);
    static avatar::live::OutputState parse_output(const godot::String& value);
    static avatar::live::PanicReason parse_reason(const godot::String& value);
    static avatar::live::EditKind parse_edit(const godot::String& value);
    static bool is_edit_kind(const godot::String& value);

    void apply_panic(bool active, const godot::String& reason);
    static godot::String format_code(avatar::core::DiagnosticCode code);

    avatar::live::LiveSession session_{};
    avatar::platform::GlobalHotkey global_hotkey_{};
    godot::Ref<AvatarOutputManager> output_manager_{};
};
