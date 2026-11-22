#include "avatar/output/output_frame.h"

namespace avatar::output {

std::size_t OutputFrame::expected_bytes() const noexcept {
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
}

bool OutputFrame::valid() const noexcept {
    return format == PixelFormat::rgba8 && width > 0 && height > 0 &&
           pixels.size() == expected_bytes();
}

const std::uint8_t* OutputFrame::pixel_at(std::uint32_t x, std::uint32_t y) const noexcept {
    if (x >= width || y >= height) {
        return nullptr;
    }
    const std::size_t offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                static_cast<std::size_t>(x)) *
                               4U;
    return pixels.data() + offset;
}

const char* alpha_mode_name(AlphaMode mode) noexcept {
    switch (mode) {
        case AlphaMode::straight:
            return "straight";
        case AlphaMode::premultiplied:
            return "premultiplied";
        case AlphaMode::opaque:
            return "opaque";
    }
    return "unknown";
}

const char* color_space_name(ColorSpace space) noexcept {
    switch (space) {
        case ColorSpace::srgb:
            return "srgb";
        case ColorSpace::linear:
            return "linear";
    }
    return "unknown";
}

const char* frame_content_name(FrameContent content) noexcept {
    switch (content) {
        case FrameContent::avatar:
            return "avatar";
        case FrameContent::blank:
            return "blank";
    }
    return "unknown";
}

} // namespace avatar::output
