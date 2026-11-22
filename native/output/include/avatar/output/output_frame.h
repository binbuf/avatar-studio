#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace avatar::output {

/// The one pixel format in 1.0. RGBA8 is tightly packed, row-major, with no
/// padding between rows.
enum class PixelFormat : std::uint8_t {
    rgba8 = 0,
};

/// What the RGB values mean relative to alpha. `straight` is the usual image
/// representation; `premultiplied` is what OBS-style transports expect;
/// `opaque` means alpha is ignored and the frame is composited over a
/// configured background before submission.
enum class AlphaMode : std::uint8_t {
    straight = 0,
    premultiplied,
    opaque,
};

/// The encoded RGB space. Camera pixels are decoded to linear before blending;
/// the captured avatar viewport is linear under the Compatibility renderer and
/// sRGB under Forward+. The output boundary is the one place that normalises
/// this difference.
enum class ColorSpace : std::uint8_t {
    srgb = 0,
    linear,
};

/// Whether a frame is the rendered avatar or the gate's blank frame. The
/// distinction is metadata for stats, diagnostics and tests; the pixels alone
/// are authoritative for what a transport shows.
enum class FrameContent : std::uint8_t {
    avatar = 0,
    blank,
};

/// One immutable frame handed to a transport. The pixel span is owned by the
/// caller and is valid only for the duration of `IVideoOutput::submit`.
///
/// This mirrors docs/design/output-and-virtual-camera.md. `content` and the
/// explicit `alpha`/`color_space` fields were added by T23 so the gate and the
/// output boundary can be tested without inferring state from pixels.
struct OutputFrame {
    std::uint64_t source_timestamp_ns = 0;
    std::uint64_t render_ns = 0;
    std::uint32_t sequence = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PixelFormat format = PixelFormat::rgba8;
    AlphaMode alpha = AlphaMode::straight;
    ColorSpace color_space = ColorSpace::srgb;
    FrameContent content = FrameContent::avatar;
    std::span<const std::uint8_t> pixels{};
    void* native_handle = nullptr;

    /// Bytes required for a tightly packed RGBA8 frame.
    [[nodiscard]] std::size_t expected_bytes() const noexcept;

    /// True when the dimensions, format and pixel span are consistent.
    [[nodiscard]] bool valid() const noexcept;

    /// Pointer to the first byte of one pixel, or nullptr when out of range.
    [[nodiscard]] const std::uint8_t* pixel_at(std::uint32_t x, std::uint32_t y) const noexcept;
};

[[nodiscard]] const char* alpha_mode_name(AlphaMode mode) noexcept;
[[nodiscard]] const char* color_space_name(ColorSpace space) noexcept;
[[nodiscard]] const char* frame_content_name(FrameContent content) noexcept;

} // namespace avatar::output
