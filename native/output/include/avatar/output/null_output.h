#pragma once

#include "avatar/output/video_output.h"

namespace avatar::output {

/// A transport that accepts every valid frame and keeps only counters. It is
/// the cheapest way to exercise the gate and the transport-agnostic invariant.
class NullOutput final : public IVideoOutput {
public:
    bool initialize(const OutputConfig& config) override;
    bool submit(const OutputFrame& frame) override;
    void shutdown() override;

    [[nodiscard]] OutputStats stats() const override { return stats_; }
    [[nodiscard]] OutputConfig config() const override { return config_; }
    [[nodiscard]] bool is_initialized() const override { return initialized_; }
    [[nodiscard]] const char* name() const noexcept override { return "null"; }

    [[nodiscard]] FrameContent last_content() const noexcept { return last_content_; }
    [[nodiscard]] AlphaMode last_alpha() const noexcept { return last_alpha_; }
    [[nodiscard]] ColorSpace last_color_space() const noexcept { return last_color_space_; }
    [[nodiscard]] std::uint32_t last_width() const noexcept { return last_width_; }
    [[nodiscard]] std::uint32_t last_height() const noexcept { return last_height_; }

private:
    OutputConfig config_{};
    OutputStats stats_{};
    bool initialized_ = false;
    FrameContent last_content_ = FrameContent::avatar;
    AlphaMode last_alpha_ = AlphaMode::straight;
    ColorSpace last_color_space_ = ColorSpace::srgb;
    std::uint32_t last_width_ = 0;
    std::uint32_t last_height_ = 0;
};

} // namespace avatar::output
