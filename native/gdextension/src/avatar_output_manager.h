#pragma once

#include "avatar/core/diagnostics.h"
#include "avatar/output/output_manager.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace avatar::output {
class WindowOutput;
}

/// GDScript-facing owner of one output transport plus the T23 gate. It is a
/// thin adapter: parsing, frame copying and diagnostic buffering only. The
/// invariant lives in avatar::output::OutputManager.
class AvatarOutputManager : public godot::RefCounted {
    GDCLASS(AvatarOutputManager, godot::RefCounted)

public:
    AvatarOutputManager();
    ~AvatarOutputManager() override;

    bool initialize(const godot::Dictionary& config);
    godot::Dictionary submit_frame(const godot::Ref<godot::Image>& frame,
                                   const godot::Dictionary& signal);
    godot::Dictionary get_gate_state() const;
    godot::Dictionary get_stats() const;
    godot::Dictionary get_config() const;
    godot::Array get_diagnostics() const;
    void clear_diagnostics();
    void shutdown();
    godot::Ref<godot::Image> get_last_frame() const;
    bool is_initialized() const;
    godot::String get_output_name() const;
    godot::String get_last_error() const;

protected:
    static void _bind_methods();

private:
    struct DiagnosticEvent {
        std::string code;
        std::string severity;
        std::string cause;
        std::uint64_t timestamp_ns = 0;
        std::uint32_t sequence = 0;
    };

    static void diagnostic_sink(void* context, const avatar::core::LogRecord& record) noexcept;
    void add_diagnostic(const avatar::core::LogRecord& record) noexcept;
    void set_error(const godot::String& message);

    std::unique_ptr<avatar::output::OutputManager> manager_;
    avatar::output::WindowOutput* window_output_ = nullptr;
    avatar::output::AlphaMode source_alpha_ = avatar::output::AlphaMode::straight;
    avatar::output::ColorSpace source_color_space_ = avatar::output::ColorSpace::srgb;
    std::vector<DiagnosticEvent> diagnostics_;
    std::size_t diagnostic_limit_ = 64;
    godot::String last_error_;
};
