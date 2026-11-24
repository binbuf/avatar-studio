#include "backend.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace avatar::tracker_backend {

avatar_tracker_error validate_config(const avatar_tracker_config& c) {
    if (c.abi_version != AVATAR_TRACKER_ABI_VERSION) {
        return AVATAR_TRACKER_ERROR_ABI_MISMATCH;
    }
    if (c.reserved || c.scenario > AVATAR_TRACKER_SCENARIO_LEFT_RIGHT_SWAP || !c.frame_count ||
        !c.frame_interval_ns || !(c.base_confidence >= 0 && c.base_confidence <= 1) ||
        c.simulate_fault > AVATAR_TRACKER_FAULT_POLL ||
        ((c.input_width == 0) != (c.input_height == 0))) {
        return AVATAR_TRACKER_ERROR_INVALID_CONFIG;
    }
    if (c.input_width &&
        (c.input_width != c.input_height || c.input_width < 64 || c.input_width > 1024)) {
        return AVATAR_TRACKER_ERROR_INVALID_CONFIG;
    }
    return AVATAR_TRACKER_OK;
}

avatar_tracker_error validate_frame(const avatar_tracker_frame& f) {
    if (!f.width || !f.height || f.format == AVATAR_TRACKER_PIXEL_FORMAT_MJPEG ||
        f.format > AVATAR_TRACKER_PIXEL_FORMAT_RGBA8) {
        return AVATAR_TRACKER_ERROR_INVALID_ARGUMENT;
    }
    if (f.width > 8192 || f.height > 8192) {
        return AVATAR_TRACKER_ERROR_FRAME_TOO_LARGE;
    }
    const bool nv12 = f.format == AVATAR_TRACKER_PIXEL_FORMAT_NV12;
    const bool yuy2 = f.format == AVATAR_TRACKER_PIXEL_FORMAT_YUY2;
    if ((nv12 && ((f.width | f.height) & 1)) || (yuy2 && (f.width & 1))) {
        return AVATAR_TRACKER_ERROR_INVALID_ARGUMENT;
    }
    const size_t row = static_cast<size_t>(f.width) * (nv12 ? 1 : yuy2 ? 2 : 4);
    if (f.plane_count != (nv12 ? 2u : 1u) || !f.plane0 || f.stride < row || f.stride > 32768 ||
        f.plane0_size < static_cast<size_t>(f.stride) * f.height || f.plane2 || f.plane2_size ||
        (nv12 ? (!f.plane1 || f.plane1_size < static_cast<size_t>(f.stride) * (f.height / 2))
              : (f.plane1 != nullptr || f.plane1_size != 0))) {
        return AVATAR_TRACKER_ERROR_INVALID_ARGUMENT;
    }
    return AVATAR_TRACKER_OK;
}

void OwnedFrame::copy(const avatar_tracker_frame& input) {
    frame = input;
    // Copy only the validated layout, never caller-supplied trailing bytes.
    frame.plane0_size = static_cast<size_t>(frame.stride) * frame.height;
    frame.plane1_size = frame.plane_count == 2 ? frame.plane0_size / 2 : 0;
    bytes.resize(frame.plane0_size + frame.plane1_size);
    std::memcpy(bytes.data(), input.plane0, frame.plane0_size);
    if (frame.plane1_size) {
        std::memcpy(bytes.data() + frame.plane0_size, input.plane1, frame.plane1_size);
    }
    bind();
}

void OwnedFrame::bind() {
    frame.plane0 = bytes.data();
    frame.plane1 = frame.plane1_size ? bytes.data() + frame.plane0_size : nullptr;
}

avatar_tracker_landmark Letterbox::unproject(float x, float y, float z) const {
    return {(x * size - pad_x) / (scale * source_width),
            (y * size - pad_y) / (scale * source_height), z * size / (scale * source_width)};
}

namespace {
std::array<float, 3> pixel(const avatar_tracker_frame& f, int x, int y) {
    x = std::clamp(x, 0, static_cast<int>(f.width) - 1);
    y = std::clamp(y, 0, static_cast<int>(f.height) - 1);
    const auto* row = f.plane0 + static_cast<size_t>(y) * f.stride;
    if (f.format == AVATAR_TRACKER_PIXEL_FORMAT_BGRA8 ||
        f.format == AVATAR_TRACKER_PIXEL_FORMAT_RGBA8) {
        const auto* p = row + x * 4;
        return f.format == AVATAR_TRACKER_PIXEL_FORMAT_BGRA8
                   ? std::array<float, 3>{float(p[2]), float(p[1]), float(p[0])}
                   : std::array<float, 3>{float(p[0]), float(p[1]), float(p[2])};
    }
    float luma, u, v;
    if (f.format == AVATAR_TRACKER_PIXEL_FORMAT_NV12) {
        const auto* uv = f.plane1 + static_cast<size_t>(y / 2) * f.stride + (x & ~1);
        luma = row[x];
        u = uv[0];
        v = uv[1];
    } else {
        const auto* p = row + (x & ~1) * 2;
        luma = p[(x & 1) * 2];
        u = p[1];
        v = p[3];
    }
    // ABI v1 has no colorimetry field: YUV inputs are BT.601 limited.
    // Callers with another colour space must convert to RGBA/BGRA first.
    const float c = 1.164384f * (luma - 16);
    u -= 128;
    v -= 128;
    return {c + 1.596027f * v, c - 0.391762f * u - 0.812968f * v, c + 2.017232f * u};
}
} // namespace

PreparedFrame prepare(const avatar_tracker_frame& f, uint32_t size) {
    PreparedFrame out;
    out.box = {
        std::min(float(size) / f.width, float(size) / f.height), 0, 0, size, f.width, f.height};
    out.box.pad_x = (size - f.width * out.box.scale) * 0.5f;
    out.box.pad_y = (size - f.height * out.box.scale) * 0.5f;
    out.rgb.resize(static_cast<size_t>(size) * size * 3);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            // Sampling centres and clamped source coordinates also replicate edge padding.
            const float sx = std::clamp((x + 0.5f - out.box.pad_x) / out.box.scale - 0.5f, 0.0f,
                                        float(f.width - 1));
            const float sy = std::clamp((y + 0.5f - out.box.pad_y) / out.box.scale - 0.5f, 0.0f,
                                        float(f.height - 1));
            const int ix = static_cast<int>(sx), iy = static_cast<int>(sy);
            const float fx = sx - ix, fy = sy - iy;
            const auto a = pixel(f, ix, iy), b = pixel(f, ix + 1, iy);
            const auto c = pixel(f, ix, iy + 1), d = pixel(f, ix + 1, iy + 1);
            for (size_t k = 0; k < 3; ++k) {
                const float value =
                    (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy;
                out.rgb[(static_cast<size_t>(y) * size + x) * 3 + k] =
                    static_cast<uint8_t>(std::clamp(value, 0.0f, 255.0f) + 0.5f);
            }
        }
    }
    return out;
}

bool features_usable(const avatar_tracker_result& r) {
    if (!r.valid || r.landmark_count != 478 || r.confidence < 0.5f) {
        return false;
    }
    const auto inside = [&](size_t index) {
        const auto& p = r.landmarks[index];
        return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && p.x > 0 &&
               p.x < 1 && p.y > 0 && p.y < 1;
    };
    const auto distance = [&](size_t a, size_t b) {
        return std::hypot(r.landmarks[a].x - r.landmarks[b].x, r.landmarks[a].y - r.landmarks[b].y);
    };
    const bool mouth =
        inside(61) && inside(291) && inside(13) && inside(14) && distance(61, 291) > 0.005f;
    // A profile view hides the far eye. Keep the mouth and either eye; the rig
    // can solve from the visible subset and the decal keeps both eyes painted.
    const bool eye_r = inside(33) && inside(133) && inside(468) && distance(33, 133) > 0.003f;
    const bool eye_l = inside(263) && inside(362) && inside(473) && distance(263, 362) > 0.003f;
    return mouth && (eye_r || eye_l);
}

int64_t next_timestamp_ms(uint64_t source_ns, int64_t previous_ms) {
    const auto source_ms = static_cast<int64_t>(source_ns / 1000000);
    if (previous_ms == std::numeric_limits<int64_t>::max()) {
        throw std::overflow_error("tracker timestamp exhausted");
    }
    return std::max(source_ms, previous_ms + 1);
}

} // namespace avatar::tracker_backend
