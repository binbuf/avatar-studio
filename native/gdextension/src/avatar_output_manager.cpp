#include "avatar_output_manager.h"

#include "window_output.h"

#include "avatar/output/file_output.h"
#include "avatar/output/null_output.h"
#include "avatar/output/output_frame.h"
#include "avatar/output/output_gate.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <utility>

namespace {

[[nodiscard]] std::uint8_t unit_to_byte(float value) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

[[nodiscard]] std::filesystem::path utf8_path(const godot::String& value) {
    const godot::CharString utf8 = value.utf8();
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.get_data())));
}

[[nodiscard]] bool parse_alpha(const godot::String& value, avatar::output::AlphaMode& mode) {
    if (value == godot::String("straight")) {
        mode = avatar::output::AlphaMode::straight;
        return true;
    }
    if (value == godot::String("premultiplied")) {
        mode = avatar::output::AlphaMode::premultiplied;
        return true;
    }
    if (value == godot::String("opaque")) {
        mode = avatar::output::AlphaMode::opaque;
        return true;
    }
    return false;
}

[[nodiscard]] bool parse_color_space(const godot::String& value,
                                     avatar::output::ColorSpace& space) {
    if (value == godot::String("srgb")) {
        space = avatar::output::ColorSpace::srgb;
        return true;
    }
    if (value == godot::String("linear")) {
        space = avatar::output::ColorSpace::linear;
        return true;
    }
    return false;
}

[[nodiscard]] godot::Dictionary stats_dict(const avatar::output::OutputStats& stats) {
    godot::Dictionary out;
    out["submitted"] = static_cast<int64_t>(stats.submitted);
    out["accepted"] = static_cast<int64_t>(stats.accepted);
    out["dropped"] = static_cast<int64_t>(stats.dropped);
    out["avatar_frames"] = static_cast<int64_t>(stats.avatar_frames);
    out["held_frames"] = static_cast<int64_t>(stats.held_frames);
    out["blank_frames"] = static_cast<int64_t>(stats.blank_frames);
    out["last_sequence"] = static_cast<int64_t>(stats.last_sequence);
    out["last_submit_ns"] = static_cast<int64_t>(stats.last_submit_ns);
    return out;
}

} // namespace

AvatarOutputManager::AvatarOutputManager() = default;

AvatarOutputManager::~AvatarOutputManager() {
    shutdown();
}

void AvatarOutputManager::_bind_methods() {
    godot::ClassDB::bind_method(godot::D_METHOD("initialize", "config"),
                                &AvatarOutputManager::initialize);
    godot::ClassDB::bind_method(godot::D_METHOD("submit_frame", "frame", "signal"),
                                &AvatarOutputManager::submit_frame);
    godot::ClassDB::bind_method(godot::D_METHOD("set_panic", "active", "render_time_ns"),
                                &AvatarOutputManager::set_panic, DEFVAL(0));
    godot::ClassDB::bind_method(godot::D_METHOD("is_panic"), &AvatarOutputManager::is_panic);
    godot::ClassDB::bind_method(godot::D_METHOD("get_gate_state"),
                                &AvatarOutputManager::get_gate_state);
    godot::ClassDB::bind_method(godot::D_METHOD("get_stats"), &AvatarOutputManager::get_stats);
    godot::ClassDB::bind_method(godot::D_METHOD("get_config"), &AvatarOutputManager::get_config);
    godot::ClassDB::bind_method(godot::D_METHOD("get_diagnostics"),
                                &AvatarOutputManager::get_diagnostics);
    godot::ClassDB::bind_method(godot::D_METHOD("clear_diagnostics"),
                                &AvatarOutputManager::clear_diagnostics);
    godot::ClassDB::bind_method(godot::D_METHOD("shutdown"), &AvatarOutputManager::shutdown);
    godot::ClassDB::bind_method(godot::D_METHOD("get_last_frame"),
                                &AvatarOutputManager::get_last_frame);
    godot::ClassDB::bind_method(godot::D_METHOD("is_initialized"),
                                &AvatarOutputManager::is_initialized);
    godot::ClassDB::bind_method(godot::D_METHOD("get_output_name"),
                                &AvatarOutputManager::get_output_name);
    godot::ClassDB::bind_method(godot::D_METHOD("get_last_error"),
                                &AvatarOutputManager::get_last_error);
}

bool AvatarOutputManager::initialize(const godot::Dictionary& config) {
    shutdown();

    avatar::output::OutputConfig output_config;
    const int64_t width_value = static_cast<int64_t>(config.get("width", static_cast<int64_t>(0)));
    const int64_t height_value =
        static_cast<int64_t>(config.get("height", static_cast<int64_t>(0)));
    if (width_value <= 0 || height_value <= 0 ||
        width_value > static_cast<int64_t>(std::numeric_limits<std::uint32_t>::max()) ||
        height_value > static_cast<int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        set_error("output width and height must be positive");
        return false;
    }
    output_config.width = static_cast<std::uint32_t>(width_value);
    output_config.height = static_cast<std::uint32_t>(height_value);
    if (!output_config.valid()) {
        set_error("output width and height must be positive");
        return false;
    }

    const godot::String alpha_text = config.get("alpha", godot::String("straight"));
    if (!parse_alpha(alpha_text, output_config.alpha)) {
        set_error("alpha must be straight, premultiplied or opaque");
        return false;
    }
    const godot::String color_text = config.get("color_space", godot::String("srgb"));
    if (!parse_color_space(color_text, output_config.color_space)) {
        set_error("color_space must be srgb or linear");
        return false;
    }
    const godot::Color background = config.get("background", godot::Color(0.0F, 0.0F, 0.0F, 1.0F));
    output_config.background = {unit_to_byte(background.r), unit_to_byte(background.g),
                                unit_to_byte(background.b), 255U};

    const godot::String kind = config.get("kind", godot::String("null"));
    std::unique_ptr<avatar::output::IVideoOutput> output;
    if (kind == godot::String("null")) {
        output = std::make_unique<avatar::output::NullOutput>();
    } else if (kind == godot::String("file")) {
        avatar::output::FileOutputOptions options;
        options.directory = utf8_path(config.get("directory", godot::String("")));
        const godot::String prefix = config.get("prefix", godot::String("frame"));
        options.prefix = prefix.utf8().get_data();
        output = std::make_unique<avatar::output::FileOutput>(std::move(options));
    } else if (kind == godot::String("window")) {
        godot::TextureRect* target = nullptr;
        const godot::Variant target_value = config.get("target", godot::Variant());
        if (target_value.get_type() == godot::Variant::OBJECT) {
            target = godot::Object::cast_to<godot::TextureRect>(
                static_cast<godot::Object*>(target_value));
        }
        auto window = std::make_unique<avatar::output::WindowOutput>(target);
        window_output_ = window.get();
        output = std::move(window);
    } else {
        set_error("kind must be null, file or window");
        return false;
    }

    const godot::String source_alpha_text = config.get("source_alpha", godot::String("straight"));
    if (!parse_alpha(source_alpha_text, source_alpha_)) {
        set_error("source_alpha must be straight, premultiplied or opaque");
        return false;
    }
    const godot::String source_color_text = config.get("source_color_space", godot::String("srgb"));
    if (!parse_color_space(source_color_text, source_color_space_)) {
        set_error("source_color_space must be srgb or linear");
        return false;
    }

    diagnostic_limit_ = static_cast<std::size_t>(std::max<int64_t>(
        0, static_cast<int64_t>(config.get("diagnostic_limit", static_cast<int64_t>(64)))));
    diagnostics_.clear();
    last_error_ = godot::String();

    manager_ = std::make_unique<avatar::output::OutputManager>(
        &AvatarOutputManager::diagnostic_sink, this);
    if (!manager_->initialize(std::move(output), output_config)) {
        manager_.reset();
        window_output_ = nullptr;
        set_error("output initialization failed");
        return false;
    }
    return true;
}

godot::Dictionary AvatarOutputManager::submit_frame(const godot::Ref<godot::Image>& frame,
                                                    const godot::Dictionary& signal) {
    if (!manager_) {
        set_error("output manager is not initialized");
        return {};
    }
    if (frame.is_null() || frame->is_empty()) {
        set_error("frame is empty");
        return {};
    }

    godot::Ref<godot::Image> image = frame;
    if (image->get_format() != godot::Image::FORMAT_RGBA8) {
        image = image->duplicate();
        image->convert(godot::Image::FORMAT_RGBA8);
    }
    const godot::PackedByteArray bytes = image->get_data();

    avatar::output::OutputFrame output_frame;
    output_frame.source_timestamp_ns = static_cast<std::uint64_t>(
        static_cast<int64_t>(signal.get("source_timestamp_ns", static_cast<int64_t>(0))));
    output_frame.render_ns = static_cast<std::uint64_t>(
        static_cast<int64_t>(signal.get("render_time_ns", static_cast<int64_t>(0))));
    output_frame.width = static_cast<std::uint32_t>(image->get_width());
    output_frame.height = static_cast<std::uint32_t>(image->get_height());
    output_frame.format = avatar::output::PixelFormat::rgba8;
    output_frame.alpha = source_alpha_;
    output_frame.color_space = source_color_space_;
    output_frame.content = avatar::output::FrameContent::avatar;
    output_frame.pixels =
        std::span<const std::uint8_t>(bytes.ptr(), static_cast<std::size_t>(bytes.size()));

    avatar::output::GateSignal gate_signal;
    gate_signal.result_valid = static_cast<bool>(signal.get("result_valid", true));
    gate_signal.features_valid = static_cast<bool>(signal.get("features_valid", true));
    gate_signal.frame_available = static_cast<bool>(signal.get("frame_available", true));
    gate_signal.source_timestamp_ns = output_frame.source_timestamp_ns;
    gate_signal.render_time_ns = output_frame.render_ns;

    const avatar::output::GateDecision decision = manager_->submit(output_frame, gate_signal);
    godot::Dictionary out;
    out["state"] = godot::String(avatar::output::gate_state_name(decision.state));
    out["cause"] = godot::String(avatar::output::blank_cause_name(decision.cause));
    out["blanked"] = decision.blanked;
    out["transitioned"] = decision.transitioned;
    out["diagnostic"] =
        decision.diagnostic == avatar::core::DiagnosticCode::unknown
            ? godot::String()
            : godot::String(avatar::core::format_diagnostic_code(decision.diagnostic).c_str());
    out["sequence"] = static_cast<int64_t>(manager_->stats().last_sequence);
    out["stats"] = stats_dict(manager_->stats());
    return out;
}

godot::Dictionary AvatarOutputManager::set_panic(bool active, int64_t render_time_ns) {
    godot::Dictionary out;
    if (!manager_) {
        set_error("output manager is not initialized");
        out["state"] = godot::String("blank");
        out["cause"] = godot::String("panic");
        out["panic"] = false;
        return out;
    }
    const avatar::output::GateDecision decision = manager_->set_panic(
        active, static_cast<avatar::core::TimestampNs>(std::max<int64_t>(0, render_time_ns)));
    out["state"] = godot::String(avatar::output::gate_state_name(decision.state));
    out["cause"] = godot::String(avatar::output::blank_cause_name(decision.cause));
    out["blanked"] = decision.blanked;
    out["transitioned"] = decision.transitioned;
    out["panic"] = manager_->gate().is_panic();
    out["sequence"] = static_cast<int64_t>(manager_->stats().last_sequence);
    out["stats"] = stats_dict(manager_->stats());
    return out;
}

bool AvatarOutputManager::is_panic() const {
    return manager_ != nullptr && manager_->gate().is_panic();
}

godot::Dictionary AvatarOutputManager::get_gate_state() const {
    godot::Dictionary out;
    if (!manager_) {
        out["state"] = godot::String("blank");
        out["cause"] = godot::String("none");
        return out;
    }
    const avatar::output::OutputGate& gate = manager_->gate();
    out["state"] = godot::String(avatar::output::gate_state_name(gate.state()));
    out["cause"] = godot::String(avatar::output::blank_cause_name(gate.cause()));
    out["panic"] = gate.is_panic();
    out["last_accepted_render_ns"] = static_cast<int64_t>(gate.last_accepted_render_ns());
    out["blank_transitions"] = static_cast<int64_t>(gate.blank_transitions());
    out["resume_transitions"] = static_cast<int64_t>(gate.resume_transitions());
    return out;
}

godot::Dictionary AvatarOutputManager::get_stats() const {
    return manager_ ? stats_dict(manager_->stats()) : godot::Dictionary{};
}

godot::Dictionary AvatarOutputManager::get_config() const {
    godot::Dictionary out;
    if (!manager_) {
        return out;
    }
    const avatar::output::OutputConfig& config = manager_->config();
    out["width"] = static_cast<int64_t>(config.width);
    out["height"] = static_cast<int64_t>(config.height);
    out["alpha"] = godot::String(avatar::output::alpha_mode_name(config.alpha));
    out["color_space"] = godot::String(avatar::output::color_space_name(config.color_space));
    out["background"] = godot::Color(static_cast<float>(config.background[0]) / 255.0F,
                                     static_cast<float>(config.background[1]) / 255.0F,
                                     static_cast<float>(config.background[2]) / 255.0F, 1.0F);
    return out;
}

godot::Array AvatarOutputManager::get_diagnostics() const {
    godot::Array out;
    for (const DiagnosticEvent& event : diagnostics_) {
        godot::Dictionary entry;
        entry["code"] = godot::String(event.code.c_str());
        entry["severity"] = godot::String(event.severity.c_str());
        entry["cause"] = godot::String(event.cause.c_str());
        entry["timestamp_ns"] = static_cast<int64_t>(event.timestamp_ns);
        entry["sequence"] = static_cast<int64_t>(event.sequence);
        out.push_back(entry);
    }
    return out;
}

void AvatarOutputManager::clear_diagnostics() {
    diagnostics_.clear();
}

void AvatarOutputManager::shutdown() {
    manager_.reset();
    window_output_ = nullptr;
    diagnostics_.clear();
    last_error_ = godot::String();
    source_alpha_ = avatar::output::AlphaMode::straight;
    source_color_space_ = avatar::output::ColorSpace::srgb;
}

godot::Ref<godot::Image> AvatarOutputManager::get_last_frame() const {
    return window_output_ != nullptr ? window_output_->last_frame() : godot::Ref<godot::Image>();
}

bool AvatarOutputManager::is_initialized() const {
    return manager_ != nullptr && manager_->is_output_alive();
}

godot::String AvatarOutputManager::get_output_name() const {
    return manager_ != nullptr && manager_->output() != nullptr
               ? godot::String(manager_->output()->name())
               : godot::String();
}

godot::String AvatarOutputManager::get_last_error() const {
    return last_error_;
}

void AvatarOutputManager::diagnostic_sink(void* context,
                                          const avatar::core::LogRecord& record) noexcept {
    if (context != nullptr) {
        static_cast<AvatarOutputManager*>(context)->add_diagnostic(record);
    }
}

void AvatarOutputManager::add_diagnostic(const avatar::core::LogRecord& record) noexcept {
    if (diagnostic_limit_ == 0) {
        return;
    }
    try {
        DiagnosticEvent event;
        event.code = avatar::core::format_diagnostic_code(record.code);
        event.severity = std::string(avatar::core::severity_name(record.severity));
        event.timestamp_ns = record.timestamp_ns;
        for (std::uint8_t i = 0; i < record.field_count; ++i) {
            const avatar::core::LogField& field = record.fields[i];
            if (field.key == "cause") {
                event.cause = std::string(field.value);
            } else if (field.key == "sequence") {
                std::uint32_t sequence = 0;
                const auto result = std::from_chars(
                    field.value.data(), field.value.data() + field.value.size(), sequence);
                if (result.ec == std::errc()) {
                    event.sequence = sequence;
                }
            }
        }
        if (diagnostics_.size() >= diagnostic_limit_) {
            diagnostics_.erase(diagnostics_.begin());
        }
        diagnostics_.push_back(std::move(event));
    } catch (...) {
        // Diagnostics must never take down the output path.
    }
}

void AvatarOutputManager::set_error(const godot::String& message) {
    last_error_ = message;
}
