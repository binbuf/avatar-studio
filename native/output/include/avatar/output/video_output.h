#pragma once

#include "avatar/output/output_frame.h"

#include <array>
#include <cstdint>

namespace avatar::output {

/// Target configuration for one transport. The gate builds a blank frame in
/// these terms: transparent for non-opaque alpha, the configured background for
/// an opaque output.
struct OutputConfig {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    AlphaMode alpha = AlphaMode::straight;
    ColorSpace color_space = ColorSpace::srgb;
    /// Background RGB used by `AlphaMode::opaque`. The alpha byte is ignored;
    /// an opaque output always writes 255.
    std::array<std::uint8_t, 4> background{0, 0, 0, 255};

    [[nodiscard]] bool valid() const noexcept { return width > 0 && height > 0; }
};

struct OutputStats {
    std::uint64_t submitted = 0;
    std::uint64_t accepted = 0;
    std::uint64_t dropped = 0;
    std::uint64_t avatar_frames = 0;
    std::uint64_t held_frames = 0;
    std::uint64_t blank_frames = 0;
    std::uint32_t last_sequence = 0;
    std::uint64_t last_submit_ns = 0;
};

/// The transport seam. Implementations must not block the render thread; the
/// T23 Null/File/Window outputs copy or write immediately, and T46 adds the
/// drop-oldest queues behind this interface.
class IVideoOutput {
public:
    virtual ~IVideoOutput() = default;

    virtual bool initialize(const OutputConfig& config) = 0;

    /// Non-blocking. May drop a frame; `false` records the drop.
    virtual bool submit(const OutputFrame& frame) = 0;

    virtual void shutdown() = 0;

    [[nodiscard]] virtual OutputStats stats() const = 0;
    [[nodiscard]] virtual OutputConfig config() const = 0;
    [[nodiscard]] virtual bool is_initialized() const = 0;
    [[nodiscard]] virtual const char* name() const noexcept = 0;
};

} // namespace avatar::output
