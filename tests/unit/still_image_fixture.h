#pragma once

// Deterministic RGB fixture for the development StillImageSource test.
//
// The still source takes tightly packed RGB8, so the native test owns the
// bytes instead of decoding a PNG. The Godot setup-window test loads the
// committed `tests/fixtures/images/still_setup.png` fixture through the real
// Image path; both describe the same three-colour test card.

#include <cstdint>
#include <vector>

namespace avatar::test {

inline std::vector<std::uint8_t> make_still_rgb(std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * height * 3u, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 3u;
            // Left third red, middle third green, right third blue, with a
            // bright top row so luma conversion has a known target.
            if (y == 0) {
                rgb[index] = 235;
                rgb[index + 1] = 235;
                rgb[index + 2] = 235;
            } else if (x < width / 3u) {
                rgb[index] = 220;
            } else if (x < (width * 2u) / 3u) {
                rgb[index + 1] = 220;
            } else {
                rgb[index + 2] = 220;
            }
        }
    }
    return rgb;
}

} // namespace avatar::test
