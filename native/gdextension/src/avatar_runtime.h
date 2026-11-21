#pragma once

#include "avatar/core/clock.h"
#include "avatar/core/frame_pacer.h"
#include "avatar/core/latency_histogram.h"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <cstdint>

/// Owner of the avatar render loop.
///
/// T16 makes this the one place that configures the output viewport, the
/// preview copy, the fixed 60 Hz cadence and the debug counters. It owns no
/// avatar content or tracking state; T17+ attaches those under the
/// presentation node. The class is deliberately a thin Godot adapter over the
/// framework-free `core::FramePacer` and `core::LatencyHistogram`.
class AvatarRuntime : public godot::Node {
    GDCLASS(AvatarRuntime, godot::Node)

public:
    AvatarRuntime();
    ~AvatarRuntime() override;

    void _ready() override;
    void _process(double delta) override;

    // Shell identity (T02).
    godot::String get_version() const;
    int64_t get_frame_count() const;
    void reset_frame_count();

    // Engine and viewport configuration.
    godot::String get_renderer_name() const;
    bool is_forward_plus() const;
    bool is_low_processor_mode_disabled() const;
    godot::Vector2i get_avatar_viewport_size() const;
    bool is_avatar_viewport_transparent() const;
    int64_t get_avatar_viewport_update_mode() const;
    godot::Transform3D get_presentation_transform() const;

    // One render, one preview copy. `preview_render_count` is always zero:
    // the preview is a TextureRect sampling the SubViewport's ViewportTexture.
    int64_t get_viewport_render_count() const;
    int64_t get_preview_render_count() const;
    bool is_preview_a_copy() const;

    // Pacing and counters.
    godot::String get_pacing_mode() const;
    double get_display_refresh_hz() const;
    double get_target_interval_ms() const;
    double get_last_frame_time_ms() const;
    double get_last_interval_ms() const;
    double get_frame_time_p99_ms() const;
    double get_interval_p99_ms() const;
    double get_jitter_p99_ms() const;
    double get_max_interval_ms() const;
    int64_t get_max_burst_frames() const;
    int64_t get_dropped_frames() const;
    int64_t get_pacing_sample_count() const;
    void reset_pacing_stats();

    // Live/consumer state. A connected consumer keeps the runtime processing
    // even when the window is hidden and disables macOS App Nap.
    void set_consumer_connected(bool connected);
    bool is_consumer_connected() const;
    bool is_app_nap_disabled() const;

    // Debug HUD: hidden by default, updated from C++ at ~4 Hz. No GDScript
    // runs per frame.
    void set_debug_hud_visible(bool visible);
    bool is_debug_hud_visible() const;
    godot::String get_debug_hud_text() const;

protected:
    static void _bind_methods();

private:
    godot::Node* find_scene_node(const godot::String& name) const;
    void configure_engine();
    void configure_scene();
    void configure_viewport();
    void configure_preview();
    void configure_pacing();
    void create_debug_hud();
    void update_debug_hud();
    void sleep_until(avatar::core::TimestampNs deadline_ns);

    godot::SubViewport* avatar_viewport_ = nullptr;
    godot::TextureRect* preview_ = nullptr;
    godot::Node3D* presentation_ = nullptr;
    godot::CanvasLayer* hud_layer_ = nullptr;
    godot::Label* hud_label_ = nullptr;

    avatar::core::SteadyTimeSource time_source_;
    avatar::core::FrameClock clock_;
    avatar::core::FramePacer pacer_;
    avatar::core::LatencyHistogram<2048, 10'000> interval_histogram_;
    avatar::core::LatencyHistogram<2048, 10'000> jitter_histogram_;
    avatar::core::LatencyHistogram<2048, 10'000> frame_time_histogram_;

    godot::String renderer_name_;
    bool forward_plus_ = true;
    bool preview_is_copy_ = false;
    bool consumer_connected_ = false;
    bool app_nap_disabled_ = false;
    bool hud_visible_ = false;
    bool has_last_frame_ = false;

    avatar::core::PacingMode pacing_mode_ = avatar::core::PacingMode::deadline;
    double display_refresh_hz_ = 0.0;

    avatar::core::TimestampNs last_frame_start_ns_ = 0;
    avatar::core::TimestampNs last_interval_ns_ = 0;
    avatar::core::TimestampNs last_frame_time_ns_ = 0;
    avatar::core::TimestampNs max_interval_ns_ = 0;
    avatar::core::TimestampNs last_hud_update_ns_ = 0;
    std::uint32_t current_burst_ = 0;
    std::uint32_t max_burst_ = 0;
    std::uint64_t dropped_frames_ = 0;
    std::uint64_t viewport_render_count_ = 0;
    std::uint64_t preview_render_count_ = 0;
    int64_t frame_count_ = 0;
};
