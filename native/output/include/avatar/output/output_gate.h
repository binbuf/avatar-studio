#pragma once

#include "avatar/core/clock.h"
#include "avatar/core/diagnostics.h"

#include <cstdint>

namespace avatar::output {

using core::DiagnosticCode;
using core::TimestampNs;

enum class GateState : std::uint8_t {
    blank = 0,
    avatar,
    held,
};

/// Why the gate is not submitting the avatar. The cause is carried in the
/// diagnostic log so TRK-002, TRK-008 and OUT-007 are actionable rather than
/// just "blank".
enum class BlankCause : std::uint8_t {
    none = 0,
    missing_features,
    face_lost,
    invalid_result,
    no_frame,
    panic,
};

/// One frame's gate input. `features_valid` is the tracker/rig contract: both
/// eyes and the mouth are present. `result_valid` distinguishes a lost face
/// from a face with a missing feature. `frame_available` is false when the
/// render/readback produced nothing to hold.
struct GateSignal {
    bool result_valid = true;
    bool features_valid = true;
    bool frame_available = true;
    TimestampNs source_timestamp_ns = 0;
    TimestampNs render_time_ns = 0;
};

struct GateDecision {
    GateState state = GateState::blank;
    BlankCause cause = BlankCause::none;
    bool transitioned = false;
    bool blanked = false;
    /// TRK-002 or TRK-008 when the blank cause has one; unknown for no_frame.
    DiagnosticCode diagnostic = DiagnosticCode::unknown;
};

/// The output gate. It is pure state: it never touches pixels or pose, so it
/// cannot decay to neutral or invent a frame.
///
/// The hold bridges a dropped tracker frame or a blink for less than 150 ms
/// from the last complete accepted sample. At the deadline the gate blanks.
/// Invalid results never extend the deadline. Resuming a valid avatar does not
/// emit a blank diagnostic.
class OutputGate {
public:
    static constexpr TimestampNs kHoldNs = 150'000'000;

    [[nodiscard]] GateDecision evaluate(const GateSignal& signal);

    /// Panic is a deliberate blank. It clears the hold immediately and is
    /// checked before every other cause, so a valid result cannot resume the
    /// avatar until panic is cleared. The returned decision is the transition
    /// (blank, cause panic, no TRK code) that the manager submits at once.
    [[nodiscard]] GateDecision set_panic(bool active, TimestampNs render_time_ns);

    void reset() noexcept;

    [[nodiscard]] bool is_panic() const noexcept { return panic_; }
    [[nodiscard]] GateState state() const noexcept { return state_; }
    [[nodiscard]] BlankCause cause() const noexcept { return cause_; }
    [[nodiscard]] TimestampNs last_accepted_render_ns() const noexcept {
        return last_accepted_render_ns_;
    }
    [[nodiscard]] std::uint32_t blank_transitions() const noexcept { return blank_transitions_; }
    [[nodiscard]] std::uint32_t resume_transitions() const noexcept { return resume_transitions_; }

private:
    GateState state_ = GateState::blank;
    BlankCause cause_ = BlankCause::none;
    TimestampNs last_accepted_render_ns_ = 0;
    TimestampNs last_source_timestamp_ns_ = 0;
    TimestampNs last_render_ns_ = 0;
    std::uint32_t blank_transitions_ = 0;
    std::uint32_t resume_transitions_ = 0;
    bool panic_ = false;
};

[[nodiscard]] const char* gate_state_name(GateState state) noexcept;
[[nodiscard]] const char* blank_cause_name(BlankCause cause) noexcept;
[[nodiscard]] DiagnosticCode blank_cause_diagnostic(BlankCause cause) noexcept;

} // namespace avatar::output
