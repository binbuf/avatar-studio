#include "avatar_runtime.h"

#include "platform_power.h"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>
#include <limits>

using namespace godot;
using avatar::core::PacingDecision;
using avatar::core::PacingMode;
using avatar::core::TimestampNs;

#ifndef AVATAR_RUNTIME_VERSION
#define AVATAR_RUNTIME_VERSION "0.1.0"
#endif

namespace {

constexpr int32_t kTargetFps = 60;
constexpr int32_t kViewportWidth = 1920;
constexpr int32_t kViewportHeight = 1080;
constexpr TimestampNs kHudIntervalNs = 250'000'000;

String pacing_mode_name(PacingMode mode) {
    return mode == PacingMode::vsync ? String("vsync") : String("deadline");
}

} // namespace

AvatarRuntime::AvatarRuntime() : clock_(time_source_) {
    set_process(true);
}

AvatarRuntime::~AvatarRuntime() {
    if (app_nap_disabled_) {
        avatar::platform::enable_app_nap();
    }
}

void AvatarRuntime::_ready() {
    // A consumer can keep the render loop alive while the window is hidden or
    // the tree is paused, so never let the scene tree or OS idle policy stop
    // processing.
    set_process_mode(Node::PROCESS_MODE_ALWAYS);
    configure_engine();
    configure_scene();
    configure_viewport();
    configure_preview();
    create_debug_hud();
    configure_pacing();

    const TimestampNs now = clock_.now_ns();
    pacer_.reset(now);
    last_hud_update_ns_ = now;
    update_debug_hud();
}

void AvatarRuntime::_process(double delta) {
    (void)delta;
    TimestampNs now = clock_.now_ns();
    const PacingDecision decision = pacer_.begin_frame(now);

    // If the main loop arrived before its deadline, wait for the deadline
    // before doing this frame's work. Rendering first and sleeping afterwards
    // would put two frames inside one slot.
    if (pacing_mode_ == PacingMode::deadline && now < decision.deadline_ns) {
        sleep_until(decision.deadline_ns);
        now = clock_.now_ns();
    }

    if (has_last_frame_) {
        const TimestampNs interval = now - last_frame_start_ns_;
        last_interval_ns_ = interval;
        max_interval_ns_ = std::max(max_interval_ns_, interval);
        interval_histogram_.add(interval);
        const TimestampNs target = pacer_.interval_ns();
        jitter_histogram_.add(interval > target ? interval - target : target - interval);

        // A burst is a frame that arrives in less than half a slot: exactly
        // the shape a catch-up loop would produce. The pacer never schedules
        // one, so this counter is a regression alarm.
        if (interval * 2 < pacer_.interval_ns()) {
            ++current_burst_;
            max_burst_ = std::max(max_burst_, current_burst_);
        } else {
            current_burst_ = 0;
        }
    }
    last_frame_start_ns_ = now;
    has_last_frame_ = true;

    dropped_frames_ += decision.dropped;

    ++frame_count_;
    if (avatar_viewport_ != nullptr) {
        ++viewport_render_count_;
    }
    // preview_render_count_ deliberately stays zero. The preview samples the
    // avatar viewport texture; it has no scene of its own to render.

    if (now - last_hud_update_ns_ >= kHudIntervalNs) {
        update_debug_hud();
        last_hud_update_ns_ = now;
    }

    const TimestampNs work_end = clock_.now_ns();
    const TimestampNs frame_time = work_end - now;
    last_frame_time_ns_ = frame_time;
    frame_time_histogram_.add(frame_time);

    if (pacing_mode_ == PacingMode::deadline) {
        sleep_until(pacer_.next_deadline_ns());
    }
}

void AvatarRuntime::sleep_until(TimestampNs deadline_ns) {
    const TimestampNs after_work = clock_.now_ns();
    if (after_work >= deadline_ns) {
        return;
    }
    const TimestampNs sleep_ns = deadline_ns - after_work;
    const std::uint64_t sleep_us = (sleep_ns + 999) / 1000;
    if (sleep_us > 0) {
        OS::get_singleton()->delay_usec(static_cast<int32_t>(std::min<std::uint64_t>(
            sleep_us, static_cast<std::uint64_t>(std::numeric_limits<int32_t>::max()))));
    }
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

String AvatarRuntime::get_renderer_name() const {
    return renderer_name_;
}

bool AvatarRuntime::is_forward_plus() const {
    return forward_plus_;
}

bool AvatarRuntime::is_low_processor_mode_disabled() const {
    const OS* os = OS::get_singleton();
    return os != nullptr && !os->is_in_low_processor_usage_mode();
}

Vector2i AvatarRuntime::get_avatar_viewport_size() const {
    return avatar_viewport_ != nullptr ? avatar_viewport_->get_size() : Vector2i();
}

bool AvatarRuntime::is_avatar_viewport_transparent() const {
    return avatar_viewport_ != nullptr && avatar_viewport_->has_transparent_background();
}

int64_t AvatarRuntime::get_avatar_viewport_update_mode() const {
    return avatar_viewport_ != nullptr ? static_cast<int64_t>(avatar_viewport_->get_update_mode())
                                       : -1;
}

Transform3D AvatarRuntime::get_presentation_transform() const {
    return presentation_ != nullptr ? presentation_->get_transform() : Transform3D();
}

int64_t AvatarRuntime::get_viewport_render_count() const {
    return static_cast<int64_t>(viewport_render_count_);
}

int64_t AvatarRuntime::get_preview_render_count() const {
    return static_cast<int64_t>(preview_render_count_);
}

bool AvatarRuntime::is_preview_a_copy() const {
    return preview_is_copy_;
}

String AvatarRuntime::get_pacing_mode() const {
    return pacing_mode_name(pacing_mode_);
}

double AvatarRuntime::get_display_refresh_hz() const {
    return display_refresh_hz_;
}

double AvatarRuntime::get_target_interval_ms() const {
    return pacer_.interval_ms();
}

double AvatarRuntime::get_last_frame_time_ms() const {
    return static_cast<double>(last_frame_time_ns_) / 1'000'000.0;
}

double AvatarRuntime::get_last_interval_ms() const {
    return static_cast<double>(last_interval_ns_) / 1'000'000.0;
}

double AvatarRuntime::get_frame_time_p99_ms() const {
    return static_cast<double>(frame_time_histogram_.p99_ns()) / 1'000'000.0;
}

double AvatarRuntime::get_interval_p99_ms() const {
    return static_cast<double>(interval_histogram_.p99_ns()) / 1'000'000.0;
}

double AvatarRuntime::get_jitter_p99_ms() const {
    return static_cast<double>(jitter_histogram_.p99_ns()) / 1'000'000.0;
}

double AvatarRuntime::get_max_interval_ms() const {
    return static_cast<double>(max_interval_ns_) / 1'000'000.0;
}

int64_t AvatarRuntime::get_max_burst_frames() const {
    return static_cast<int64_t>(max_burst_);
}

int64_t AvatarRuntime::get_dropped_frames() const {
    return static_cast<int64_t>(dropped_frames_);
}

int64_t AvatarRuntime::get_pacing_sample_count() const {
    return static_cast<int64_t>(interval_histogram_.samples());
}

void AvatarRuntime::reset_pacing_stats() {
    interval_histogram_.reset();
    jitter_histogram_.reset();
    frame_time_histogram_.reset();
    const TimestampNs now = clock_.now_ns();
    pacer_.reset(now);
    last_hud_update_ns_ = now;
    last_frame_start_ns_ = 0;
    last_interval_ns_ = 0;
    last_frame_time_ns_ = 0;
    max_interval_ns_ = 0;
    current_burst_ = 0;
    max_burst_ = 0;
    dropped_frames_ = 0;
    has_last_frame_ = false;
}

void AvatarRuntime::set_consumer_connected(bool connected) {
    consumer_connected_ = connected;
    if (connected) {
        OS::get_singleton()->set_low_processor_usage_mode(false);
        if (avatar_viewport_ != nullptr) {
            avatar_viewport_->set_update_mode(SubViewport::UPDATE_ALWAYS);
        }
        if (!app_nap_disabled_) {
            avatar::platform::disable_app_nap();
            app_nap_disabled_ = true;
        }
    } else {
        if (app_nap_disabled_) {
            avatar::platform::enable_app_nap();
            app_nap_disabled_ = false;
        }
    }
}

bool AvatarRuntime::is_consumer_connected() const {
    return consumer_connected_;
}

bool AvatarRuntime::is_app_nap_disabled() const {
    return app_nap_disabled_;
}

void AvatarRuntime::set_debug_hud_visible(bool visible) {
    hud_visible_ = visible;
    if (hud_label_ != nullptr) {
        hud_label_->set_visible(visible);
    }
}

bool AvatarRuntime::is_debug_hud_visible() const {
    return hud_visible_ && hud_label_ != nullptr && hud_label_->is_visible();
}

String AvatarRuntime::get_debug_hud_text() const {
    return hud_label_ != nullptr ? hud_label_->get_text() : String();
}

Node* AvatarRuntime::find_scene_node(const String& name) const {
    if (Node* child = get_node_or_null(name)) {
        return child;
    }
    if (Node* parent = get_parent()) {
        return parent->get_node_or_null(name);
    }
    return nullptr;
}

void AvatarRuntime::configure_engine() {
    // Engine.max_fps is pinned for every display mode; the deadline scheduler
    // only corrects drift inside that cap.
    Engine::get_singleton()->set_max_fps(kTargetFps);
    OS::get_singleton()->set_low_processor_usage_mode(false);
    OS::get_singleton()->set_low_processor_usage_mode_sleep_usec(0);

    const RenderingServer* rendering_server = RenderingServer::get_singleton();
    renderer_name_ =
        rendering_server != nullptr ? rendering_server->get_current_rendering_method() : String();
    forward_plus_ = renderer_name_ == String("forward_plus");
    if (!forward_plus_) {
        UtilityFunctions::push_error(
            String("GPU-002: Avatar Studio requires the Forward+ renderer, got '") +
            renderer_name_ + String("'"));
    }
}

void AvatarRuntime::configure_scene() {
    avatar_viewport_ = Object::cast_to<SubViewport>(find_scene_node("SubViewport"));
    preview_ = Object::cast_to<TextureRect>(find_scene_node("Preview"));
    presentation_ =
        avatar_viewport_ != nullptr
            ? Object::cast_to<Node3D>(avatar_viewport_->get_node_or_null("Presentation"))
            : nullptr;
}

void AvatarRuntime::configure_viewport() {
    if (avatar_viewport_ == nullptr) {
        return;
    }
    avatar_viewport_->set_size(Vector2i(kViewportWidth, kViewportHeight));
    avatar_viewport_->set_transparent_background(true);
    avatar_viewport_->set_use_own_world_3d(true);
    avatar_viewport_->set_update_mode(SubViewport::UPDATE_ALWAYS);
    avatar_viewport_->set_clear_mode(SubViewport::CLEAR_MODE_ALWAYS);
}

void AvatarRuntime::configure_preview() {
    if (avatar_viewport_ == nullptr || preview_ == nullptr) {
        return;
    }
    Ref<ViewportTexture> viewport_texture = avatar_viewport_->get_texture();
    preview_->set_texture(viewport_texture);
    preview_->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
    preview_->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
    preview_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);

    const Ref<Texture2D> preview_texture = preview_->get_texture();
    preview_is_copy_ = viewport_texture.is_valid() && preview_texture.is_valid() &&
                       preview_texture->get_rid() == viewport_texture->get_rid();
}

void AvatarRuntime::configure_pacing() {
    DisplayServer* display_server = DisplayServer::get_singleton();
    double refresh_hz = 0.0;
    if (display_server != nullptr && display_server->get_name() != String("headless") &&
        display_server->get_screen_count() > 0) {
        refresh_hz =
            display_server->screen_get_refresh_rate(display_server->window_get_current_screen());
    }
    display_refresh_hz_ = refresh_hz;
    pacing_mode_ =
        avatar::core::FramePacer::select_mode(refresh_hz, static_cast<double>(kTargetFps));

    if (display_server != nullptr && display_server->get_name() != String("headless")) {
        display_server->window_set_vsync_mode(pacing_mode_ == PacingMode::vsync
                                                  ? DisplayServer::VSYNC_ENABLED
                                                  : DisplayServer::VSYNC_DISABLED);
    }
}

void AvatarRuntime::create_debug_hud() {
    hud_layer_ = memnew(CanvasLayer);
    hud_layer_->set_name("DebugHudLayer");
    hud_layer_->set_layer(100);
    add_child(hud_layer_);

    hud_label_ = memnew(Label);
    hud_label_->set_name("DebugHud");
    hud_label_->set_position(Vector2(12.0F, 12.0F));
    hud_label_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    hud_label_->add_theme_font_size_override("font_size", 14);
    hud_label_->add_theme_color_override("font_color", Color(1.0F, 1.0F, 1.0F, 1.0F));
    hud_label_->add_theme_color_override("font_outline_color", Color(0.0F, 0.0F, 0.0F, 1.0F));
    hud_label_->add_theme_constant_override("outline_size", 4);
    hud_label_->set_visible(false);
    hud_layer_->add_child(hud_label_);
}

void AvatarRuntime::update_debug_hud() {
    if (hud_label_ == nullptr) {
        return;
    }
    const Vector2i size = get_avatar_viewport_size();
    String text;
    text += String("frame ") + String::num(get_last_frame_time_ms(), 2) + String(" ms");
    text += String("  interval ") + String::num(get_last_interval_ms(), 2) + String(" ms");
    text += String("  jitter ") + String::num(get_jitter_p99_ms(), 2) + String(" ms");
    text += String("  drops ") + String::num_int64(get_dropped_frames());
    text +=
        String("\nviewport ") + String::num_int64(size.x) + String("x") + String::num_int64(size.y);
    text += String("  renderer ") + renderer_name_;
    text += String("  pacing ") + pacing_mode_name(pacing_mode_);
    text += String("  refresh ") + String::num(display_refresh_hz_, 1) + String(" Hz");
    hud_label_->set_text(text);
}

void AvatarRuntime::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_version"), &AvatarRuntime::get_version);
    ClassDB::bind_method(D_METHOD("get_frame_count"), &AvatarRuntime::get_frame_count);
    ClassDB::bind_method(D_METHOD("reset_frame_count"), &AvatarRuntime::reset_frame_count);

    ClassDB::bind_method(D_METHOD("get_renderer_name"), &AvatarRuntime::get_renderer_name);
    ClassDB::bind_method(D_METHOD("is_forward_plus"), &AvatarRuntime::is_forward_plus);
    ClassDB::bind_method(D_METHOD("is_low_processor_mode_disabled"),
                         &AvatarRuntime::is_low_processor_mode_disabled);
    ClassDB::bind_method(D_METHOD("get_avatar_viewport_size"),
                         &AvatarRuntime::get_avatar_viewport_size);
    ClassDB::bind_method(D_METHOD("is_avatar_viewport_transparent"),
                         &AvatarRuntime::is_avatar_viewport_transparent);
    ClassDB::bind_method(D_METHOD("get_avatar_viewport_update_mode"),
                         &AvatarRuntime::get_avatar_viewport_update_mode);
    ClassDB::bind_method(D_METHOD("get_presentation_transform"),
                         &AvatarRuntime::get_presentation_transform);

    ClassDB::bind_method(D_METHOD("get_viewport_render_count"),
                         &AvatarRuntime::get_viewport_render_count);
    ClassDB::bind_method(D_METHOD("get_preview_render_count"),
                         &AvatarRuntime::get_preview_render_count);
    ClassDB::bind_method(D_METHOD("is_preview_a_copy"), &AvatarRuntime::is_preview_a_copy);

    ClassDB::bind_method(D_METHOD("get_pacing_mode"), &AvatarRuntime::get_pacing_mode);
    ClassDB::bind_method(D_METHOD("get_display_refresh_hz"),
                         &AvatarRuntime::get_display_refresh_hz);
    ClassDB::bind_method(D_METHOD("get_target_interval_ms"),
                         &AvatarRuntime::get_target_interval_ms);
    ClassDB::bind_method(D_METHOD("get_last_frame_time_ms"),
                         &AvatarRuntime::get_last_frame_time_ms);
    ClassDB::bind_method(D_METHOD("get_last_interval_ms"), &AvatarRuntime::get_last_interval_ms);
    ClassDB::bind_method(D_METHOD("get_frame_time_p99_ms"), &AvatarRuntime::get_frame_time_p99_ms);
    ClassDB::bind_method(D_METHOD("get_interval_p99_ms"), &AvatarRuntime::get_interval_p99_ms);
    ClassDB::bind_method(D_METHOD("get_jitter_p99_ms"), &AvatarRuntime::get_jitter_p99_ms);
    ClassDB::bind_method(D_METHOD("get_max_interval_ms"), &AvatarRuntime::get_max_interval_ms);
    ClassDB::bind_method(D_METHOD("get_max_burst_frames"), &AvatarRuntime::get_max_burst_frames);
    ClassDB::bind_method(D_METHOD("get_dropped_frames"), &AvatarRuntime::get_dropped_frames);
    ClassDB::bind_method(D_METHOD("get_pacing_sample_count"),
                         &AvatarRuntime::get_pacing_sample_count);
    ClassDB::bind_method(D_METHOD("reset_pacing_stats"), &AvatarRuntime::reset_pacing_stats);

    ClassDB::bind_method(D_METHOD("set_consumer_connected", "connected"),
                         &AvatarRuntime::set_consumer_connected);
    ClassDB::bind_method(D_METHOD("is_consumer_connected"), &AvatarRuntime::is_consumer_connected);
    ClassDB::bind_method(D_METHOD("is_app_nap_disabled"), &AvatarRuntime::is_app_nap_disabled);

    ClassDB::bind_method(D_METHOD("set_debug_hud_visible", "visible"),
                         &AvatarRuntime::set_debug_hud_visible);
    ClassDB::bind_method(D_METHOD("is_debug_hud_visible"), &AvatarRuntime::is_debug_hud_visible);
    ClassDB::bind_method(D_METHOD("get_debug_hud_text"), &AvatarRuntime::get_debug_hud_text);

    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "frame_count", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_READ_ONLY),
        "", "get_frame_count");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "consumer_connected"), "set_consumer_connected",
                 "is_consumer_connected");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_hud_visible"), "set_debug_hud_visible",
                 "is_debug_hud_visible");
}
