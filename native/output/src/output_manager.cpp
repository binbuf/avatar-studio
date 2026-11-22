#include "avatar/output/output_manager.h"

#include <array>
#include <charconv>
#include <string>

namespace avatar::output {
namespace {

[[nodiscard]] std::string sequence_text(std::uint32_t sequence) {
    std::array<char, 16> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), sequence);
    return std::string(buffer.data(), result.ptr);
}

} // namespace

OutputManager::OutputManager(DiagnosticSink sink, void* context) : logger_(sink, context) {}

OutputManager::~OutputManager() = default;

bool OutputManager::initialize(std::unique_ptr<IVideoOutput> output, const OutputConfig& config) {
    if (!output || !config.valid()) {
        return false;
    }
    if (!output->initialize(config)) {
        return false;
    }
    output_ = std::move(output);
    config_ = output_->config();
    gate_.reset();
    stats_ = {};
    sequence_ = 0;
    convert_storage_.clear();
    blank_storage_.clear();
    return true;
}

void OutputManager::shutdown() {
    if (output_) {
        output_->shutdown();
    }
    output_.reset();
    config_ = {};
    gate_.reset();
    stats_ = {};
    sequence_ = 0;
    convert_storage_.clear();
    blank_storage_.clear();
}

void OutputManager::reset() {
    gate_.reset();
    stats_ = {};
    sequence_ = 0;
}

GateDecision OutputManager::submit(const OutputFrame& avatar_frame, const GateSignal& signal) {
    if (!output_ || !output_->is_initialized()) {
        GateSignal effective = signal;
        effective.frame_available = false;
        return gate_.evaluate(effective);
    }

    const bool frame_ok = avatar_frame.valid() && avatar_frame.width == config_.width &&
                          avatar_frame.height == config_.height;
    GateSignal effective = signal;
    effective.frame_available = signal.frame_available && frame_ok;
    GateDecision decision = gate_.evaluate(effective);

    if (decision.state == GateState::blank) {
        const std::uint32_t sequence = ++sequence_;
        OutputFrame blank = make_blank_frame(config_, signal.source_timestamp_ns,
                                             signal.render_time_ns, sequence, blank_storage_);
        const bool accepted = output_->submit(blank);
        record_submit(blank, accepted);
        if (decision.transitioned) {
            emit_blank_diagnostics(decision, signal, sequence);
        }
        return decision;
    }

    OutputFrame converted;
    std::string error;
    if (!convert_to_output(avatar_frame, config_, convert_storage_, converted, &error)) {
        // Defensive: a frame that cannot be converted must never reach the
        // transport. Submit a blank and report the drop without inventing a
        // diagnostic transition the gate did not produce.
        const std::uint32_t sequence = ++sequence_;
        OutputFrame blank = make_blank_frame(config_, signal.source_timestamp_ns,
                                             signal.render_time_ns, sequence, blank_storage_);
        const bool accepted = output_->submit(blank);
        record_submit(blank, accepted);
        GateDecision fallback = decision;
        fallback.state = GateState::blank;
        fallback.cause = BlankCause::no_frame;
        fallback.blanked = true;
        fallback.transitioned = false;
        fallback.diagnostic = DiagnosticCode::unknown;
        return fallback;
    }

    converted.sequence = ++sequence_;
    converted.source_timestamp_ns = avatar_frame.source_timestamp_ns;
    converted.render_ns = avatar_frame.render_ns;
    converted.content = FrameContent::avatar;
    const bool accepted = output_->submit(converted);
    record_submit(converted, accepted);
    if (decision.state == GateState::held) {
        ++stats_.held_frames;
    }
    return decision;
}

void OutputManager::emit_blank_diagnostics(const GateDecision& decision, const GateSignal& signal,
                                           std::uint32_t sequence) {
    const std::string sequence_value = sequence_text(sequence);
    const std::string_view cause = blank_cause_name(decision.cause);
    const std::string_view state = gate_state_name(decision.state);
    if (decision.diagnostic != DiagnosticCode::unknown) {
        logger_.log_at(signal.render_time_ns, decision.diagnostic,
                       {{"cause", cause}, {"sequence", sequence_value}, {"state", state}});
    }
    logger_.log_at(signal.render_time_ns, DiagnosticCode::out_007,
                   {{"cause", cause}, {"sequence", sequence_value}, {"state", state}});
}

void OutputManager::record_submit(const OutputFrame& frame, bool accepted) {
    ++stats_.submitted;
    if (accepted) {
        ++stats_.accepted;
    } else {
        ++stats_.dropped;
    }
    stats_.last_sequence = frame.sequence;
    stats_.last_submit_ns = frame.render_ns;
    if (frame.content == FrameContent::blank) {
        ++stats_.blank_frames;
    } else {
        ++stats_.avatar_frames;
    }
}

} // namespace avatar::output
