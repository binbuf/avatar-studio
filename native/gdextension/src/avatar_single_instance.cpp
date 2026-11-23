#include "avatar_single_instance.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* kSecondInstanceCode = "CFG-007";
constexpr const char* kSecondInstanceSentence = "Avatar Studio is already running";
constexpr const char* kSecondInstanceAction = "Activate the existing window";
constexpr const char* kActivatedAction = "Brought the existing window forward";
constexpr const char* kTestInstanceSentence =
    "Test instance: virtual camera and output registration are off.";
constexpr const char* kTestInstanceAction = "Use this window for testing only.";

std::string to_utf8(const godot::String& value) {
    return std::string(value.utf8().get_data());
}

godot::String to_godot(const std::string& value) {
    return godot::String::utf8(value.c_str());
}

} // namespace

AvatarSingleInstance::AvatarSingleInstance() = default;

AvatarSingleInstance::~AvatarSingleInstance() {
    stop();
}

void AvatarSingleInstance::_bind_methods() {
    godot::ClassDB::bind_method(godot::D_METHOD("start", "config"), &AvatarSingleInstance::start);
    godot::ClassDB::bind_method(godot::D_METHOD("poll"), &AvatarSingleInstance::poll);
    godot::ClassDB::bind_method(godot::D_METHOD("activate", "avatar", "show", "timeout_ms"),
                                &AvatarSingleInstance::activate, DEFVAL(godot::String()),
                                DEFVAL(godot::String()), DEFVAL(1500));
    godot::ClassDB::bind_method(godot::D_METHOD("get_state"), &AvatarSingleInstance::get_state);
    godot::ClassDB::bind_method(godot::D_METHOD("get_identity"),
                                &AvatarSingleInstance::get_identity);
    godot::ClassDB::bind_method(godot::D_METHOD("is_primary"), &AvatarSingleInstance::is_primary);
    godot::ClassDB::bind_method(godot::D_METHOD("is_secondary"),
                                &AvatarSingleInstance::is_secondary);
    godot::ClassDB::bind_method(godot::D_METHOD("is_new_instance"),
                                &AvatarSingleInstance::is_new_instance);
    godot::ClassDB::bind_method(godot::D_METHOD("stop"), &AvatarSingleInstance::stop);
    godot::ClassDB::bind_method(godot::D_METHOD("get_last_error"),
                                &AvatarSingleInstance::get_last_error);
    godot::ClassDB::bind_method(godot::D_METHOD("get_copy_sentence"),
                                &AvatarSingleInstance::get_copy_sentence);
    godot::ClassDB::bind_method(godot::D_METHOD("get_copy_action"),
                                &AvatarSingleInstance::get_copy_action);
}

void AvatarSingleInstance::_notification(int what) {
    if (what == NOTIFICATION_PREDELETE) {
        stop();
    }
}

const char* AvatarSingleInstance::state_name(State state) {
    switch (state) {
        case State::primary:
            return "primary";
        case State::secondary_activated:
            return "secondary-activated";
        case State::secondary_failed:
            return "secondary-failed";
        case State::error:
            return "error";
        case State::idle:
        default:
            return "idle";
    }
}

godot::Dictionary AvatarSingleInstance::start(const godot::Dictionary& config) {
    stop();
    started_ = true;
    delivered_ = false;
    channel_ready_ = false;
    activation_count_ = 0;
    detail_.clear();
    channel_error_.clear();
    last_error_.clear();
    last_avatar_.clear();
    last_show_.clear();

    std::string app_id = "avatar-studio";
    if (config.has("app_id")) {
        app_id = to_utf8(static_cast<godot::String>(config["app_id"]));
    }
    const bool new_instance =
        config.has("new_instance") && static_cast<bool>(config["new_instance"]);
    int timeout_ms = 1500;
    if (config.has("timeout_ms")) {
        timeout_ms = std::clamp(static_cast<int>(config["timeout_ms"]), 50, 30000);
    }

    identity_ = avatar::instance::current_identity(app_id, new_instance);
    copy_sentence_ = new_instance ? kTestInstanceSentence : kSecondInstanceSentence;
    copy_action_ = new_instance ? kTestInstanceAction : kSecondInstanceAction;

    const auto lock_state = lock_.acquire(identity_);
    if (lock_state == avatar::instance::LockState::error) {
        state_ = State::error;
        last_error_ = lock_.last_error();
        copy_sentence_ = "Avatar Studio could not start";
        copy_action_ = "Close other instances and try again";
        return build_state();
    }
    if (lock_state == avatar::instance::LockState::secondary) {
        avatar::instance::ActivationMessage message;
        if (config.has("avatar")) {
            message.avatar_path = to_utf8(static_cast<godot::String>(config["avatar"]));
        }
        if (config.has("show")) {
            message.show_path = to_utf8(static_cast<godot::String>(config["show"]));
        }
        const auto result = avatar::instance::send_activation(identity_, message, timeout_ms);
        delivered_ = result.outcome == avatar::instance::ActivationDelivery::delivered;
        detail_ = result.detail;
        state_ = delivered_ ? State::secondary_activated : State::secondary_failed;
        if (!delivered_) {
            last_error_ = result.detail;
        }
        copy_sentence_ = kSecondInstanceSentence;
        copy_action_ = kSecondInstanceAction;
        return build_state();
    }

    // Primary. A channel failure does not stop the application; it only means a
    // later launch cannot activate this window.
    std::string error;
    channel_ready_ = server_.start(identity_, error);
    channel_error_ = channel_ready_ ? std::string() : error;
    state_ = State::primary;
    if (new_instance) {
        copy_sentence_ = kTestInstanceSentence;
        copy_action_ = kTestInstanceAction;
    } else {
        copy_sentence_.clear();
        copy_action_.clear();
    }
    return build_state();
}

godot::Dictionary AvatarSingleInstance::poll() {
    std::vector<avatar::instance::ActivationMessage> messages;
    if (state_ == State::primary && channel_ready_) {
        messages = server_.poll();
    }
    godot::Dictionary out;
    out["count"] = static_cast<int64_t>(messages.size());
    out["pending"] = !messages.empty();
    out["total"] = static_cast<int64_t>(activation_count_ + messages.size());
    out["channel_ready"] = channel_ready_;
    if (!messages.empty()) {
        const auto& message = messages.back();
        activation_count_ += messages.size();
        last_avatar_ = message.avatar_path;
        last_show_ = message.show_path;
        detail_ = "activated";
        copy_sentence_ = kSecondInstanceSentence;
        copy_action_ = kActivatedAction;
    }
    out["avatar"] = to_godot(last_avatar_);
    out["show"] = to_godot(last_show_);
    out["activation_count"] = static_cast<int64_t>(activation_count_);
    return out;
}

godot::Dictionary AvatarSingleInstance::activate(const godot::String& avatar,
                                                 const godot::String& show, int64_t timeout_ms) {
    if (identity_.app_id.empty()) {
        identity_ = avatar::instance::current_identity("avatar-studio", false);
    }
    avatar::instance::ActivationMessage message;
    message.avatar_path = to_utf8(avatar);
    message.show_path = to_utf8(show);
    const auto result = avatar::instance::send_activation(
        identity_, message, static_cast<int>(std::clamp<int64_t>(timeout_ms, 50, 30000)));
    godot::Dictionary out;
    out["delivered"] = result.outcome == avatar::instance::ActivationDelivery::delivered;
    out["not_running"] = result.outcome == avatar::instance::ActivationDelivery::not_running;
    out["detail"] = to_godot(result.detail);
    return out;
}

godot::Dictionary AvatarSingleInstance::get_state() const {
    return build_state();
}

godot::Dictionary AvatarSingleInstance::get_identity() const {
    return build_identity();
}

bool AvatarSingleInstance::is_primary() const {
    return state_ == State::primary;
}

bool AvatarSingleInstance::is_secondary() const {
    return state_ == State::secondary_activated || state_ == State::secondary_failed;
}

bool AvatarSingleInstance::is_new_instance() const {
    return identity_.new_instance;
}

void AvatarSingleInstance::stop() {
    server_.stop();
    lock_.release();
    if (started_) {
        state_ = State::idle;
        started_ = false;
        channel_ready_ = false;
    }
}

godot::String AvatarSingleInstance::get_last_error() const {
    return to_godot(last_error_);
}

godot::String AvatarSingleInstance::get_copy_sentence() const {
    return to_godot(copy_sentence_);
}

godot::String AvatarSingleInstance::get_copy_action() const {
    return to_godot(copy_action_);
}

godot::Dictionary AvatarSingleInstance::build_state() const {
    godot::Dictionary out;
    out["state"] = state_name(state_);
    out["primary"] = state_ == State::primary;
    out["secondary"] = is_secondary();
    out["new_instance"] = identity_.new_instance;
    out["delivered"] = delivered_;
    out["detail"] = to_godot(detail_);
    out["error"] = to_godot(last_error_);
    out["channel_ready"] = channel_ready_;
    out["channel_error"] = to_godot(channel_error_);
    out["reclaimed_stale"] = lock_.reclaimed_stale();
    out["activation_count"] = static_cast<int64_t>(activation_count_);
    out["identity"] = build_identity();
    godot::Dictionary copy;
    copy["code"] = kSecondInstanceCode;
    copy["sentence"] = to_godot(copy_sentence_);
    copy["action"] = to_godot(copy_action_);
    out["copy"] = copy;
    return out;
}

godot::Dictionary AvatarSingleInstance::build_identity() const {
    godot::Dictionary out;
    out["app_id"] = to_godot(identity_.app_id);
    out["scope"] = to_godot(identity_.scope);
    out["lock_name"] = to_godot(identity_.lock_name);
    out["activation_name"] = to_godot(identity_.activation_name);
    out["shared_memory_name"] = to_godot(identity_.shared_memory_name);
    out["state_path"] = to_godot(identity_.state_path);
    out["pid"] = static_cast<int64_t>(identity_.pid);
    out["new_instance"] = identity_.new_instance;
    out["single_instance"] = identity_.single_instance;
    out["virtual_camera_enabled"] = identity_.virtual_camera_enabled;
    out["output_registration_enabled"] = identity_.output_registration_enabled;
    return out;
}
