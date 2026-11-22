#include "avatar/output/frame_convert.h"

#include <algorithm>
#include <cmath>

namespace avatar::output {
namespace {

[[nodiscard]] std::uint8_t to_byte(float value) noexcept {
    const float clamped = std::clamp(value, 0.0F, 1.0F);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0F));
}

[[nodiscard]] float to_unit(std::uint8_t value) noexcept {
    return static_cast<float>(value) / 255.0F;
}

[[nodiscard]] float srgb_to_linear_float(float value) noexcept {
    const float clamped = std::clamp(value, 0.0F, 1.0F);
    if (clamped <= 0.04045F) {
        return clamped / 12.92F;
    }
    return std::pow((clamped + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] float linear_to_srgb_float(float value) noexcept {
    const float clamped = std::clamp(value, 0.0F, 1.0F);
    if (clamped <= 0.0031308F) {
        return clamped * 12.92F;
    }
    return 1.055F * std::pow(clamped, 1.0F / 2.4F) - 0.055F;
}

} // namespace

float srgb8_to_linear(std::uint8_t srgb) noexcept {
    return srgb_to_linear_float(to_unit(srgb));
}

std::uint8_t linear_to_srgb8(float linear) noexcept {
    return to_byte(linear_to_srgb_float(linear));
}

bool convert_to_output(const OutputFrame& source, const OutputConfig& config,
                       std::vector<std::uint8_t>& storage, OutputFrame& out, std::string* error) {
    if (!source.valid()) {
        if (error != nullptr) {
            *error = "source frame is invalid";
        }
        return false;
    }
    if (!config.valid()) {
        if (error != nullptr) {
            *error = "output config is invalid";
        }
        return false;
    }
    if (source.width != config.width || source.height != config.height) {
        if (error != nullptr) {
            *error = "source and output sizes differ";
        }
        return false;
    }

    out = source;
    out.sequence = 0;
    out.alpha = config.alpha;
    out.color_space = config.color_space;
    out.native_handle = nullptr;

    if (source.alpha == config.alpha && source.color_space == config.color_space) {
        return true;
    }

    const std::size_t count = static_cast<std::size_t>(config.width) * config.height;
    storage.resize(count * 4U);
    const float background_srgb[3] = {to_unit(config.background[0]), to_unit(config.background[1]),
                                      to_unit(config.background[2])};
    float background_linear[3] = {background_srgb[0], background_srgb[1], background_srgb[2]};
    if (config.alpha == AlphaMode::opaque && config.color_space == ColorSpace::srgb) {
        background_linear[0] = srgb_to_linear_float(background_srgb[0]);
        background_linear[1] = srgb_to_linear_float(background_srgb[1]);
        background_linear[2] = srgb_to_linear_float(background_srgb[2]);
    }

    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* in = source.pixels.data() + i * 4U;
        float r = to_unit(in[0]);
        float g = to_unit(in[1]);
        float b = to_unit(in[2]);
        float a = source.alpha == AlphaMode::opaque ? 1.0F : to_unit(in[3]);

        if (source.alpha == AlphaMode::premultiplied) {
            if (a > 0.0F) {
                r /= a;
                g /= a;
                b /= a;
            } else {
                r = 0.0F;
                g = 0.0F;
                b = 0.0F;
            }
        }

        if (config.alpha == AlphaMode::opaque) {
            // Composite in linear light, then encode to the target space.
            if (source.color_space == ColorSpace::srgb) {
                r = srgb_to_linear_float(r);
                g = srgb_to_linear_float(g);
                b = srgb_to_linear_float(b);
            }
            r = r * a + background_linear[0] * (1.0F - a);
            g = g * a + background_linear[1] * (1.0F - a);
            b = b * a + background_linear[2] * (1.0F - a);
            if (config.color_space == ColorSpace::srgb) {
                r = linear_to_srgb_float(r);
                g = linear_to_srgb_float(g);
                b = linear_to_srgb_float(b);
            }
            a = 1.0F;
        } else {
            if (source.color_space != config.color_space) {
                if (config.color_space == ColorSpace::linear) {
                    r = srgb_to_linear_float(r);
                    g = srgb_to_linear_float(g);
                    b = srgb_to_linear_float(b);
                } else {
                    r = linear_to_srgb_float(r);
                    g = linear_to_srgb_float(g);
                    b = linear_to_srgb_float(b);
                }
            }
            if (config.alpha == AlphaMode::premultiplied) {
                r *= a;
                g *= a;
                b *= a;
            }
        }

        std::uint8_t* pixel = storage.data() + i * 4U;
        pixel[0] = to_byte(r);
        pixel[1] = to_byte(g);
        pixel[2] = to_byte(b);
        pixel[3] = to_byte(a);
    }

    out.pixels = std::span<const std::uint8_t>(storage.data(), storage.size());
    return true;
}

OutputFrame make_blank_frame(const OutputConfig& config, std::uint64_t source_timestamp_ns,
                             std::uint64_t render_ns, std::uint32_t sequence,
                             std::vector<std::uint8_t>& storage) {
    OutputFrame frame;
    frame.source_timestamp_ns = source_timestamp_ns;
    frame.render_ns = render_ns;
    frame.sequence = sequence;
    frame.width = config.width;
    frame.height = config.height;
    frame.alpha = config.alpha;
    frame.color_space = config.color_space;
    frame.content = FrameContent::blank;
    if (!config.valid()) {
        return frame;
    }

    const std::size_t bytes = static_cast<std::size_t>(config.width) * config.height * 4U;
    storage.assign(bytes, 0U);
    if (config.alpha == AlphaMode::opaque) {
        for (std::size_t i = 0; i < bytes; i += 4U) {
            storage[i] = config.background[0];
            storage[i + 1] = config.background[1];
            storage[i + 2] = config.background[2];
            storage[i + 3] = 255U;
        }
    }
    frame.pixels = std::span<const std::uint8_t>(storage.data(), storage.size());
    return frame;
}

} // namespace avatar::output
