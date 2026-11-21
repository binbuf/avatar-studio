#include "avatar_runtime.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>

using namespace godot;

#ifndef AVATAR_RUNTIME_VERSION
#define AVATAR_RUNTIME_VERSION "0.1.0"
#endif

AvatarRuntime::AvatarRuntime() {
    // The frame counter is the minimal proof that the C++ side is receiving
    // process notifications. It is not the pacing design; T16 owns that.
    set_process(true);
}

void AvatarRuntime::_process(double delta) {
    (void)delta;
    ++frame_count_;
}

String AvatarRuntime::get_version() const {
    return String(AVATAR_RUNTIME_VERSION);
}

int64_t AvatarRuntime::get_frame_count() const {
    return frame_count_;
}

void AvatarRuntime::reset_frame_count() {
    frame_count_ = 0;
}

void AvatarRuntime::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_version"), &AvatarRuntime::get_version);
    ClassDB::bind_method(D_METHOD("get_frame_count"), &AvatarRuntime::get_frame_count);
    ClassDB::bind_method(D_METHOD("reset_frame_count"), &AvatarRuntime::reset_frame_count);

    ADD_PROPERTY(
            PropertyInfo(Variant::INT, "frame_count", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_READ_ONLY),
            "", "get_frame_count");
}
