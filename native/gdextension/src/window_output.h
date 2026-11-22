#pragma once

#include "avatar/output/video_output.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/core/object.hpp>

#include <cstdint>

namespace avatar::output {

/// The always-available window transport. It presents the gated frame into a
/// TextureRect in the app window, so a blank frame is visibly transparent (or
/// the configured background) rather than a frozen avatar.
///
/// This is a GDExtension adapter, not framework-free core: it needs an engine
/// texture to display. The gate itself lives in OutputManager and is identical
/// for Null, File and Window.
class WindowOutput final : public IVideoOutput {
public:
    explicit WindowOutput(godot::TextureRect* target = nullptr);

    bool initialize(const OutputConfig& config) override;
    bool submit(const OutputFrame& frame) override;
    void shutdown() override;

    [[nodiscard]] OutputStats stats() const override { return stats_; }
    [[nodiscard]] OutputConfig config() const override { return config_; }
    [[nodiscard]] bool is_initialized() const override { return initialized_; }
    [[nodiscard]] const char* name() const noexcept override { return "window"; }

    void set_target(godot::TextureRect* target);
    [[nodiscard]] godot::Ref<godot::Image> last_frame() const { return last_frame_; }
    [[nodiscard]] bool has_target() const noexcept { return target_id_ != 0; }

private:
    [[nodiscard]] godot::TextureRect* resolve_target() const;

    std::uint64_t target_id_ = 0;
    godot::Ref<godot::ImageTexture> texture_;
    godot::Ref<godot::Image> last_frame_;
    OutputConfig config_{};
    OutputStats stats_{};
    bool initialized_ = false;
};

} // namespace avatar::output
