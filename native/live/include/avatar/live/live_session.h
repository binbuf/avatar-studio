#pragma once

#include "avatar/core/clock.h"
#include "avatar/core/diagnostics.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace avatar::live {

using core::DiagnosticCode;
using core::Severity;
using core::TimestampNs;

enum class CameraState : std::uint8_t {
    ok = 0,
    lost,
    busy,
    denied,
    none,
};

enum class FaceState : std::uint8_t {
    tracked = 0,
    not_visible,
    features_hidden,
    degraded,
    unavailable,
};

enum class PanicReason : std::uint8_t {
    none = 0,
    button,
    hotkey,
    global_hotkey,
    external,
};

enum class OutputState : std::uint8_t {
    placeholder = 0,
    live,
    blanked,
    hidden,
    stopped,
};

enum class EditKind : std::uint8_t {
    avatar = 0,
    calibration,
    camera,
    resolution,
    transport,
    outfit,
    appearance,
};

enum class Hotkey : std::uint8_t {
    panic = 0,
};

/// Pipeline state the Live surface is allowed to see. The adapter sets this
/// from capture, tracker and output callbacks; the session never reads a
/// camera or tracker itself.
struct Inputs {
    CameraState camera = CameraState::ok;
    FaceState face = FaceState::tracked;
    bool tracker_available = true;
    float confidence = 1.0F;
};

/// The one honest status the preview shows. `blocks_output` is true when the
/// output must be blank for this state; `degraded` tracking is reported but
/// does not blank a still-valid avatar.
struct Status {
    DiagnosticCode code = DiagnosticCode::unknown;
    Severity severity = Severity::info;
    bool live = false;
    bool healthy = true;
    bool blocks_output = false;
    FaceState face = FaceState::tracked;
    OutputState output = OutputState::placeholder;
    std::string_view sentence{};
    std::string_view action{};
};

struct Metrics {
    bool live = false;
    bool panic = false;
    double elapsed_s = 0.0;
    double fps = 0.0;
    double frame_time_ms = 0.0;
    float confidence = 0.0F;
    FaceState face = FaceState::tracked;
    OutputState output = OutputState::placeholder;
};

struct EditDecision {
    bool allowed = true;
    bool requires_stop = false;
    std::string sentence{};
    std::string action{};
};

/// Live-mode state machine and copy. Framework-free: no Godot, no OS calls and
/// no per-frame allocation. The output gate owns pixels; this owns the words
/// and the edit guard.
class LiveSession {
public:
    static constexpr std::size_t kFrameWindow = 120;

    void start(TimestampNs now_ns);
    void stop(TimestampNs now_ns);
    [[nodiscard]] bool is_live() const noexcept { return live_; }

    void tick(TimestampNs now_ns);
    void report_frame(TimestampNs now_ns, double frame_time_ms);

    void set_inputs(const Inputs& inputs) noexcept { inputs_ = inputs; }
    [[nodiscard]] const Inputs& inputs() const noexcept { return inputs_; }
    void set_camera(CameraState state) noexcept { inputs_.camera = state; }
    void set_face(FaceState state, float confidence) noexcept;
    void set_tracker_available(bool available) noexcept { inputs_.tracker_available = available; }
    void set_output_state(OutputState state) noexcept;

    void set_panic(bool active, PanicReason reason) noexcept;
    [[nodiscard]] bool toggle_panic(PanicReason reason) noexcept;
    [[nodiscard]] bool is_panic() const noexcept { return panic_; }
    [[nodiscard]] PanicReason panic_reason() const noexcept { return panic_reason_; }

    [[nodiscard]] Status status() const noexcept;
    [[nodiscard]] Metrics metrics(TimestampNs now_ns) const noexcept;
    [[nodiscard]] double elapsed_seconds(TimestampNs now_ns) const noexcept;
    [[nodiscard]] std::string hud_text(TimestampNs now_ns) const;

    [[nodiscard]] EditDecision request_edit(EditKind kind) const;
    [[nodiscard]] bool handle_hotkey(Hotkey hotkey) noexcept;

    [[nodiscard]] static std::string_view camera_state_name(CameraState state) noexcept;
    [[nodiscard]] static std::string_view face_state_name(FaceState state) noexcept;
    [[nodiscard]] static std::string_view output_state_name(OutputState state) noexcept;
    [[nodiscard]] static std::string_view panic_reason_name(PanicReason reason) noexcept;
    [[nodiscard]] static std::string_view edit_kind_name(EditKind kind) noexcept;

private:
    void clear_frames() noexcept;

    Inputs inputs_{};
    bool live_ = false;
    bool panic_ = false;
    PanicReason panic_reason_ = PanicReason::none;
    OutputState output_ = OutputState::placeholder;
    TimestampNs start_ns_ = 0;
    TimestampNs last_tick_ns_ = 0;
    TimestampNs last_frame_ns_ = 0;
    double frame_time_ms_ = 0.0;
    std::array<TimestampNs, kFrameWindow> frame_times_{};
    std::size_t frame_head_ = 0;
    std::size_t frame_count_ = 0;
};

} // namespace avatar::live
