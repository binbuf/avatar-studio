#pragma once

#include "avatar/core/diagnostics.h"
#include "avatar/output/frame_convert.h"
#include "avatar/output/output_gate.h"
#include "avatar/output/video_output.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace avatar::output {

using DiagnosticSink = void (*)(void* context, const core::LogRecord& record) noexcept;

/// Owns one transport and the gate that decides what it sees. T23 deliberately
/// keeps the manager single-transport: the gate and the transport boundary are
/// the invariant, while T46 adds fan-out, async readback and per-transport
/// drop-oldest queues.
class OutputManager {
public:
    OutputManager(DiagnosticSink sink = nullptr, void* context = nullptr);
    ~OutputManager();

    OutputManager(const OutputManager&) = delete;
    OutputManager& operator=(const OutputManager&) = delete;

    [[nodiscard]] bool initialize(std::unique_ptr<IVideoOutput> output, const OutputConfig& config);
    void shutdown();

    /// Submit one rendered avatar frame plus the gate signal. When the gate
    /// blanks, the transport receives a blank frame in its own alpha/background
    /// terms. The transport is never shut down while blank.
    [[nodiscard]] GateDecision submit(const OutputFrame& avatar_frame, const GateSignal& signal);

    void reset();

    [[nodiscard]] const OutputGate& gate() const noexcept { return gate_; }
    [[nodiscard]] const OutputStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const OutputConfig& config() const noexcept { return config_; }
    [[nodiscard]] IVideoOutput* output() const noexcept { return output_.get(); }
    [[nodiscard]] bool is_output_alive() const noexcept {
        return output_ != nullptr && output_->is_initialized();
    }
    [[nodiscard]] std::uint32_t sequence() const noexcept { return sequence_; }

private:
    void emit_blank_diagnostics(const GateDecision& decision, const GateSignal& signal,
                                std::uint32_t sequence);
    void record_submit(const OutputFrame& frame, bool accepted);

    std::unique_ptr<IVideoOutput> output_;
    OutputConfig config_{};
    OutputGate gate_{};
    core::Logger logger_;
    OutputStats stats_{};
    std::uint32_t sequence_ = 0;
    std::vector<std::uint8_t> convert_storage_;
    std::vector<std::uint8_t> blank_storage_;
};

} // namespace avatar::output
