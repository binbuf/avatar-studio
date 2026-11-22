#include "avatar/output/output_gate.h"

#include <algorithm>

namespace avatar::output {
namespace {

[[nodiscard]] BlankCause cause_for(const GateSignal& signal) noexcept {
    if (!signal.frame_available) {
        return BlankCause::no_frame;
    }
    if (!signal.result_valid) {
        return BlankCause::face_lost;
    }
    if (!signal.features_valid) {
        return BlankCause::missing_features;
    }
    return BlankCause::invalid_result;
}

} // namespace

GateDecision OutputGate::evaluate(const GateSignal& signal) {
    if (signal.render_time_ns < last_render_ns_) {
        // A non-monotonic clock must never extend the hold.
        last_accepted_render_ns_ = 0;
    }
    last_render_ns_ = std::max(last_render_ns_, signal.render_time_ns);

    GateDecision decision;
    const bool complete = signal.frame_available && signal.result_valid && signal.features_valid;
    if (complete) {
        last_accepted_render_ns_ = signal.render_time_ns;
        last_source_timestamp_ns_ = signal.source_timestamp_ns;
        decision.state = GateState::avatar;
        decision.cause = BlankCause::none;
        decision.transitioned = state_ != GateState::avatar;
        if (decision.transitioned) {
            ++resume_transitions_;
        }
        state_ = GateState::avatar;
        cause_ = BlankCause::none;
        return decision;
    }

    // The hold is permission to submit the current rendered frame, not a
    // stored/frozen copy. It only exists while there is a recent complete
    // sample and a frame to submit. At the deadline the frame is blank.
    const bool have_hold = signal.frame_available && last_accepted_render_ns_ != 0 &&
                           signal.render_time_ns >= last_accepted_render_ns_ &&
                           signal.render_time_ns - last_accepted_render_ns_ < kHoldNs;
    if (have_hold) {
        decision.state = GateState::held;
        decision.cause = cause_for(signal);
        decision.transitioned = state_ != GateState::held;
        state_ = GateState::held;
        cause_ = decision.cause;
        return decision;
    }

    decision.state = GateState::blank;
    decision.cause = cause_for(signal);
    decision.blanked = true;
    decision.diagnostic = blank_cause_diagnostic(decision.cause);
    const bool state_changed = state_ != GateState::blank;
    const bool cause_changed = cause_ != decision.cause;
    decision.transitioned = state_changed || cause_changed;
    if (decision.transitioned) {
        ++blank_transitions_;
    }
    state_ = GateState::blank;
    cause_ = decision.cause;
    return decision;
}

void OutputGate::reset() noexcept {
    state_ = GateState::blank;
    cause_ = BlankCause::none;
    last_accepted_render_ns_ = 0;
    last_source_timestamp_ns_ = 0;
    last_render_ns_ = 0;
    blank_transitions_ = 0;
    resume_transitions_ = 0;
}

const char* gate_state_name(GateState state) noexcept {
    switch (state) {
        case GateState::blank:
            return "blank";
        case GateState::avatar:
            return "avatar";
        case GateState::held:
            return "held";
    }
    return "unknown";
}

const char* blank_cause_name(BlankCause cause) noexcept {
    switch (cause) {
        case BlankCause::none:
            return "none";
        case BlankCause::missing_features:
            return "missing_features";
        case BlankCause::face_lost:
            return "face_lost";
        case BlankCause::invalid_result:
            return "invalid_result";
        case BlankCause::no_frame:
            return "no_frame";
    }
    return "unknown";
}

DiagnosticCode blank_cause_diagnostic(BlankCause cause) noexcept {
    switch (cause) {
        case BlankCause::missing_features:
        case BlankCause::invalid_result:
            return DiagnosticCode::trk_008;
        case BlankCause::face_lost:
            return DiagnosticCode::trk_002;
        case BlankCause::no_frame:
        case BlankCause::none:
            return DiagnosticCode::unknown;
    }
    return DiagnosticCode::unknown;
}

} // namespace avatar::output
