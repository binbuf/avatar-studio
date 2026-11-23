#pragma once

#include "avatar/instance/activation_channel.h"
#include "avatar/instance/instance_identity.h"
#include "avatar/instance/instance_lock.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <string>
#include <vector>

/// GDScript-facing owner of the single-instance lock and the local activation
/// channel. It is a thin adapter: argument parsing, dictionary building and
/// message forwarding only. The lock, the OS objects and the activation queue
/// live in avatar::instance.
class AvatarSingleInstance : public godot::RefCounted {
    GDCLASS(AvatarSingleInstance, godot::RefCounted)

public:
    AvatarSingleInstance();
    ~AvatarSingleInstance() override;

    /// Claims the per-user lock. Returns a dictionary with `state` one of
    /// `primary`, `secondary-activated`, `secondary-failed` or `error`.
    godot::Dictionary start(const godot::Dictionary& config);
    /// Drains queued activation messages. Called once per frame by the surface;
    /// the surface owns the window raise and any avatar/show action.
    godot::Dictionary poll();
    /// Sends an activation to a running instance without claiming the lock.
    godot::Dictionary activate(const godot::String& avatar, const godot::String& show,
                               int64_t timeout_ms);
    godot::Dictionary get_state() const;
    godot::Dictionary get_identity() const;
    bool is_primary() const;
    bool is_secondary() const;
    bool is_new_instance() const;
    void stop();
    godot::String get_last_error() const;
    godot::String get_copy_sentence() const;
    godot::String get_copy_action() const;

protected:
    static void _bind_methods();
    void _notification(int what);

private:
    enum class State {
        idle = 0,
        primary,
        secondary_activated,
        secondary_failed,
        error,
    };

    static const char* state_name(State state);
    godot::Dictionary build_state() const;
    godot::Dictionary build_identity() const;

    avatar::instance::InstanceIdentity identity_;
    avatar::instance::InstanceLock lock_;
    avatar::instance::ActivationServer server_;
    State state_ = State::idle;
    bool started_ = false;
    bool delivered_ = false;
    bool channel_ready_ = false;
    std::uint64_t activation_count_ = 0;
    std::string detail_;
    std::string channel_error_;
    std::string last_error_;
    std::string last_avatar_;
    std::string last_show_;
    std::string copy_sentence_;
    std::string copy_action_;
};
