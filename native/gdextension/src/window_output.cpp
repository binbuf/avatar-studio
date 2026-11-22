#include "window_output.h"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <cstring>

namespace avatar::output {

WindowOutput::WindowOutput(godot::TextureRect* target) {
    set_target(target);
}

void WindowOutput::set_target(godot::TextureRect* target) {
    target_id_ = target != nullptr ? target->get_instance_id() : 0;
    if (target != nullptr && texture_.is_valid()) {
        target->set_texture(texture_);
    }
}

godot::TextureRect* WindowOutput::resolve_target() const {
    if (target_id_ == 0) {
        return nullptr;
    }
    godot::Object* object = godot::ObjectDB::get_instance(target_id_);
    return godot::Object::cast_to<godot::TextureRect>(object);
}

bool WindowOutput::initialize(const OutputConfig& config) {
    if (!config.valid()) {
        initialized_ = false;
        return false;
    }
    config_ = config;
    stats_ = {};
    initialized_ = true;
    return true;
}

bool WindowOutput::submit(const OutputFrame& frame) {
    if (!initialized_ || !frame.valid() || frame.width != config_.width ||
        frame.height != config_.height) {
        ++stats_.dropped;
        return false;
    }

    godot::PackedByteArray bytes;
    bytes.resize(static_cast<int64_t>(frame.pixels.size()));
    std::memcpy(bytes.ptrw(), frame.pixels.data(), frame.pixels.size());
    const godot::Ref<godot::Image> image = godot::Image::create_from_data(
        static_cast<int32_t>(frame.width), static_cast<int32_t>(frame.height), false,
        godot::Image::FORMAT_RGBA8, bytes);
    if (image.is_null() || image->is_empty()) {
        ++stats_.dropped;
        return false;
    }
    last_frame_ = image;

    if (godot::TextureRect* target = resolve_target(); target != nullptr) {
        if (texture_.is_null() || texture_->get_width() != static_cast<int32_t>(frame.width) ||
            texture_->get_height() != static_cast<int32_t>(frame.height)) {
            texture_ = godot::ImageTexture::create_from_image(image);
        } else {
            texture_->update(image);
        }
        target->set_texture(texture_);
    }

    ++stats_.submitted;
    ++stats_.accepted;
    stats_.last_sequence = frame.sequence;
    stats_.last_submit_ns = frame.render_ns;
    if (frame.content == FrameContent::blank) {
        ++stats_.blank_frames;
    } else {
        ++stats_.avatar_frames;
    }
    return true;
}

void WindowOutput::shutdown() {
    initialized_ = false;
    target_id_ = 0;
    texture_.unref();
    last_frame_.unref();
}

} // namespace avatar::output
