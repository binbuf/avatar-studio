#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace avatar::rig {
using HeadPoint = std::array<float, 3>;
inline float head_weight(const HeadPoint& p, const HeadPoint& origin, const HeadPoint& normal,
                         float falloff) {
    if (!(falloff > 0))
        return 0;
    float d = 0;
    for (size_t i = 0; i < 3; ++i)
        d += (p[i] - origin[i]) * normal[i];
    const float t = std::clamp(d / falloff, 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
inline HeadPoint head_pivot(const HeadPoint& origin, const HeadPoint& normal, float scale) {
    HeadPoint result{};
    for (size_t i = 0; i < 3; ++i)
        result[i] = origin[i] - normal[i] * (0.75f * scale);
    return result;
}
// Axis/angle is canonicalised to the shortest quaternion arc once per frame.
// Rodrigues with angle*w is exactly slerp(identity, q, w), including at pi.
struct HeadRotation {
    HeadPoint axis{0, 1, 0};
    float angle = 0;
    explicit HeadRotation(std::array<float, 4> q) {
        float length = 0;
        for (float c : q)
            length += c * c;
        if (!(length > 0) || !std::isfinite(length))
            return;
        const float sign = q[3] < 0 ? -1.0f : 1.0f;
        for (float& c : q)
            c *= sign / std::sqrt(length);
        const float s = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
        if (s < 1e-8f)
            return;
        for (size_t i = 0; i < 3; ++i)
            axis[i] = q[i] / s;
        angle = 2 * std::atan2(s, q[3]);
    }
    HeadPoint rotate(const HeadPoint& p, float sine, float cosine) const {
        const float dot = axis[0] * p[0] + axis[1] * p[1] + axis[2] * p[2];
        const HeadPoint cross{axis[1] * p[2] - axis[2] * p[1], axis[2] * p[0] - axis[0] * p[2],
                              axis[0] * p[1] - axis[1] * p[0]};
        HeadPoint result{};
        for (size_t i = 0; i < 3; ++i)
            result[i] = p[i] * cosine + cross[i] * sine + axis[i] * dot * (1 - cosine);
        return result;
    }
};
} // namespace avatar::rig
