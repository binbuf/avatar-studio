#pragma once

// Internal helper shared by the synthetic sources. It is not part of the
// public capture API: sources render into an RGB canvas and then convert once
// into the ring slot they own. The conversion is an explicit source-side step,
// not a hidden frame copy.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "avatar/capture/video_frame.h"

namespace avatar::capture::detail {

struct Rgb8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

[[nodiscard]] inline std::uint8_t clamp_u8(int value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

class RgbCanvas {
public:
    RgbCanvas(std::uint32_t width, std::uint32_t height)
        : width_(width), height_(height),
          pixels_(static_cast<std::size_t>(width) * height * 3u, 0) {}

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

    void fill(Rgb8 colour) noexcept {
        for (std::size_t index = 0; index < pixels_.size(); index += 3) {
            pixels_[index] = colour.r;
            pixels_[index + 1] = colour.g;
            pixels_[index + 2] = colour.b;
        }
    }

    void set(std::uint32_t x, std::uint32_t y, Rgb8 colour) noexcept {
        if (x >= width_ || y >= height_) {
            return;
        }
        const std::size_t index = (static_cast<std::size_t>(y) * width_ + x) * 3u;
        pixels_[index] = colour.r;
        pixels_[index + 1] = colour.g;
        pixels_[index + 2] = colour.b;
    }

    [[nodiscard]] Rgb8 get(std::uint32_t x, std::uint32_t y) const noexcept {
        if (x >= width_ || y >= height_) {
            return {};
        }
        const std::size_t index = (static_cast<std::size_t>(y) * width_ + x) * 3u;
        return Rgb8{pixels_[index], pixels_[index + 1], pixels_[index + 2]};
    }

    [[nodiscard]] const std::uint8_t* data() const noexcept { return pixels_.data(); }

    /// Converts the canvas into the frame's first (and, for NV12, second) plane.
    /// The frame must have been reserved by FrameRing::begin_write and have the
    /// matching dimensions and format.
    [[nodiscard]] bool write_to(VideoFrame& frame) const noexcept {
        if (frame.width != width_ || frame.height != height_) {
            return false;
        }
        switch (frame.format) {
            case PixelFormat::bgra8:
                return write_bgra(frame, false);
            case PixelFormat::rgba8:
                return write_bgra(frame, true);
            case PixelFormat::nv12:
                return write_nv12(frame);
            case PixelFormat::yuy2:
            case PixelFormat::mjpeg:
                return false;
        }
        return false;
    }

private:
    [[nodiscard]] bool write_bgra(VideoFrame& frame, bool rgba) const noexcept {
        if (frame.planes[0] == nullptr) {
            return false;
        }
        std::uint8_t* destination = const_cast<std::uint8_t*>(frame.planes[0]);
        const std::uint32_t stride = frame.strides[0];
        for (std::uint32_t y = 0; y < height_; ++y) {
            std::uint8_t* row = destination + static_cast<std::size_t>(y) * stride;
            const std::uint8_t* source = pixels_.data() + static_cast<std::size_t>(y) * width_ * 3u;
            for (std::uint32_t x = 0; x < width_; ++x) {
                const std::uint8_t r = source[x * 3u];
                const std::uint8_t g = source[x * 3u + 1];
                const std::uint8_t b = source[x * 3u + 2];
                std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 4u;
                if (rgba) {
                    pixel[0] = r;
                    pixel[1] = g;
                    pixel[2] = b;
                } else {
                    pixel[0] = b;
                    pixel[1] = g;
                    pixel[2] = r;
                }
                pixel[3] = 255;
            }
        }
        return true;
    }

    [[nodiscard]] bool write_nv12(VideoFrame& frame) const noexcept {
        if (frame.planes[0] == nullptr || frame.planes[1] == nullptr) {
            return false;
        }
        std::uint8_t* y_plane = const_cast<std::uint8_t*>(frame.planes[0]);
        std::uint8_t* uv_plane = const_cast<std::uint8_t*>(frame.planes[1]);
        const std::uint32_t y_stride = frame.strides[0];
        const std::uint32_t uv_stride = frame.strides[1];

        for (std::uint32_t y = 0; y < height_; ++y) {
            std::uint8_t* row = y_plane + static_cast<std::size_t>(y) * y_stride;
            const std::uint8_t* source = pixels_.data() + static_cast<std::size_t>(y) * width_ * 3u;
            for (std::uint32_t x = 0; x < width_; ++x) {
                const float r = source[x * 3u];
                const float g = source[x * 3u + 1];
                const float b = source[x * 3u + 2];
                const float luma =
                    16.0f + (0.2126f * r + 0.7152f * g + 0.0722f * b) * (219.0f / 255.0f);
                row[x] = clamp_u8(static_cast<int>(luma + 0.5f));
            }
        }

        for (std::uint32_t y = 0; y < height_; y += 2) {
            std::uint8_t* row = uv_plane + static_cast<std::size_t>(y / 2u) * uv_stride;
            for (std::uint32_t x = 0; x < width_; x += 2) {
                float r = 0.0f;
                float g = 0.0f;
                float b = 0.0f;
                for (std::uint32_t dy = 0; dy < 2; ++dy) {
                    const std::uint8_t* source =
                        pixels_.data() + (static_cast<std::size_t>(y + dy) * width_ + x) * 3u;
                    for (std::uint32_t dx = 0; dx < 2; ++dx) {
                        r += source[dx * 3u];
                        g += source[dx * 3u + 1];
                        b += source[dx * 3u + 2];
                    }
                }
                r *= 0.25f;
                g *= 0.25f;
                b *= 0.25f;
                const float u =
                    128.0f + (-0.1146f * r - 0.3854f * g + 0.5000f * b) * (224.0f / 255.0f);
                const float v =
                    128.0f + (0.5000f * r - 0.4542f * g - 0.0458f * b) * (224.0f / 255.0f);
                row[x] = clamp_u8(static_cast<int>(u + 0.5f));
                row[x + 1] = clamp_u8(static_cast<int>(v + 0.5f));
            }
        }
        return true;
    }

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::vector<std::uint8_t> pixels_;
};

} // namespace avatar::capture::detail
