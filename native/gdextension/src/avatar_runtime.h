#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/string.hpp>

// The only class exposed by the shell GDExtension. It owns no business logic:
// T16 and later tasks add the runtime services behind it. Keeping this adapter
// tiny proves the C++/Godot boundary, the pinned toolchain, and headless loading
// before any rendering work starts.
class AvatarRuntime : public godot::Node {
    GDCLASS(AvatarRuntime, godot::Node)

    int64_t frame_count_ = 0;

protected:
    static void _bind_methods();

public:
    AvatarRuntime();
    ~AvatarRuntime() override = default;

    void _process(double delta) override;

    godot::String get_version() const;
    int64_t get_frame_count() const;
    void reset_frame_count();
};
