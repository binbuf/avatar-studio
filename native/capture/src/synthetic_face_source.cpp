#include "avatar/capture/synthetic_face_source.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "rgb_canvas.h"

namespace avatar::capture {

namespace {

constexpr float kPi = 3.14159265358979323846f;

float degrees_to_radians(float degrees) noexcept {
    return degrees * (kPi / 180.0f);
}

float smoothstep01(float value) noexcept {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return clamped * clamped * (3.0f - 2.0f * clamped);
}

float lerp(float a, float b, float t) noexcept {
    return a + (b - a) * t;
}

using Mat3 = std::array<float, 9>;

Mat3 multiply(const Mat3& a, const Mat3& b) noexcept {
    Mat3 result{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) {
                sum += a[static_cast<std::size_t>(row) * 3u + static_cast<std::size_t>(k)] *
                       b[static_cast<std::size_t>(k) * 3u + static_cast<std::size_t>(column)];
            }
            result[static_cast<std::size_t>(row) * 3u + static_cast<std::size_t>(column)] = sum;
        }
    }
    return result;
}

bool invert(const Mat3& matrix, Mat3& inverse) noexcept {
    const float a = matrix[0];
    const float b = matrix[1];
    const float c = matrix[2];
    const float d = matrix[3];
    const float e = matrix[4];
    const float f = matrix[5];
    const float g = matrix[6];
    const float h = matrix[7];
    const float i = matrix[8];
    const float determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(determinant) < 1e-12f) {
        return false;
    }
    const float inv = 1.0f / determinant;
    inverse = {inv * (e * i - f * h), inv * (c * h - b * i), inv * (b * f - c * e),
               inv * (f * g - d * i), inv * (a * i - c * g), inv * (c * d - a * f),
               inv * (d * h - e * g), inv * (b * g - a * h), inv * (a * e - b * d)};
    return true;
}

std::array<float, 3> apply(const Mat3& matrix, float x, float y, float w) noexcept {
    return {matrix[0] * x + matrix[1] * y + matrix[2] * w,
            matrix[3] * x + matrix[4] * y + matrix[5] * w,
            matrix[6] * x + matrix[7] * y + matrix[8] * w};
}

bool inside_ellipse(float u, float v, float cx, float cy, float rx, float ry) noexcept {
    const float du = (u - cx) / rx;
    const float dv = (v - cy) / ry;
    return du * du + dv * dv <= 1.0f;
}

bool inside_circle(float u, float v, float cx, float cy, float radius) noexcept {
    const float du = u - cx;
    const float dv = v - cy;
    return du * du + dv * dv <= radius * radius;
}

detail::Rgb8 scale(detail::Rgb8 colour, float factor) noexcept {
    return detail::Rgb8{detail::clamp_u8(static_cast<int>(static_cast<float>(colour.r) * factor + 0.5f)),
                        detail::clamp_u8(static_cast<int>(static_cast<float>(colour.g) * factor + 0.5f)),
                        detail::clamp_u8(static_cast<int>(static_cast<float>(colour.b) * factor + 0.5f))};
}

constexpr detail::Rgb8 kBackground{30, 30, 30};
constexpr detail::Rgb8 kSkin{214, 172, 142};
constexpr detail::Rgb8 kSkinShade{190, 145, 120};
constexpr detail::Rgb8 kBrow{80, 55, 40};
constexpr detail::Rgb8 kSclera{245, 245, 245};
constexpr detail::Rgb8 kIris{70, 110, 150};
constexpr detail::Rgb8 kPupil{20, 20, 20};
constexpr detail::Rgb8 kLips{120, 40, 45};
constexpr detail::Rgb8 kMouthInner{40, 10, 15};
constexpr detail::Rgb8 kMarker{0, 255, 0};

detail::Rgb8 sample_face(float u, float v, float mouth_open, float blink, bool draw_marker) noexcept {
    if (!inside_ellipse(u, v, 0.0f, 0.30f, 0.95f, 1.25f)) {
        return kBackground;
    }

    const float shade = 1.0f - 0.08f * std::clamp((v - 0.30f) / 1.25f, -1.0f, 1.0f);
    detail::Rgb8 colour = scale(kSkin, shade);

    if (inside_ellipse(u, v, 0.50f, 0.20f, 0.22f, 0.045f) ||
        inside_ellipse(u, v, -0.50f, 0.20f, 0.22f, 0.045f)) {
        colour = kBrow;
    }

    const bool eyelids_closed = blink >= 0.5f;
    for (const float eye : {0.50f, -0.50f}) {
        if (eyelids_closed) {
            if (inside_ellipse(u, v, eye, 0.0f, 0.20f, 0.11f)) {
                colour = kSkinShade;
            }
            continue;
        }
        if (inside_ellipse(u, v, eye, 0.0f, 0.20f, 0.11f)) {
            colour = kSclera;
        }
        if (inside_circle(u, v, eye, 0.0f, 0.075f)) {
            colour = kIris;
        }
        if (inside_circle(u, v, eye, 0.0f, 0.035f)) {
            colour = kPupil;
        }
    }

    if (inside_ellipse(u, v, 0.0f, -0.35f, 0.10f, 0.20f)) {
        colour = kSkinShade;
    }

    if (inside_ellipse(u, v, 0.0f, -0.78f, 0.30f, 0.10f + 0.10f * mouth_open)) {
        colour = kLips;
    }
    if (mouth_open > 0.05f && inside_ellipse(u, v, 0.0f, -0.78f, 0.20f, 0.04f + 0.08f * mouth_open)) {
        colour = kMouthInner;
    }

    if (draw_marker && inside_circle(u, v, 0.62f, -0.25f, 0.06f)) {
        colour = kMarker;
    }
    return colour;
}

void undistort_point(float distorted_x,
                     float distorted_y,
                     float k1,
                     float aspect,
                     float& ideal_x,
                     float& ideal_y) noexcept {
    const float dx = (distorted_x - 0.5f) * aspect;
    const float dy = distorted_y - 0.5f;
    float x = dx;
    float y = dy;
    for (int iteration = 0; iteration < 6; ++iteration) {
        const float r2 = x * x + y * y;
        const float factor = 1.0f + k1 * r2;
        x = dx / factor;
        y = dy / factor;
    }
    ideal_x = 0.5f + x / aspect;
    ideal_y = 0.5f + y;
}

struct CanonicalPoint {
    GroundTruthLandmark id;
    float u;
    float v;
};

constexpr std::array<CanonicalPoint, kGroundTruthLandmarkCount> kCanonicalPoints{{
    {GroundTruthLandmark::eye_outer_r, 0.70f, 0.00f},
    {GroundTruthLandmark::eye_inner_r, 0.30f, 0.00f},
    {GroundTruthLandmark::iris_center_r, 0.50f, 0.00f},
    {GroundTruthLandmark::eye_outer_l, -0.70f, 0.00f},
    {GroundTruthLandmark::eye_inner_l, -0.30f, 0.00f},
    {GroundTruthLandmark::iris_center_l, -0.50f, 0.00f},
    {GroundTruthLandmark::nose_bridge, 0.00f, 0.05f},
    {GroundTruthLandmark::nose_tip, 0.00f, -0.32f},
    {GroundTruthLandmark::nose_left, -0.10f, -0.32f},
    {GroundTruthLandmark::nose_right, 0.10f, -0.32f},
    {GroundTruthLandmark::mouth_corner_r, 0.30f, -0.78f},
    {GroundTruthLandmark::mouth_corner_l, -0.30f, -0.78f},
    {GroundTruthLandmark::mouth_upper_center, 0.00f, -0.70f},
    {GroundTruthLandmark::mouth_lower_center, 0.00f, -0.86f},
    {GroundTruthLandmark::chin, 0.00f, -1.05f},
    {GroundTruthLandmark::jaw_left, -0.75f, -0.75f},
    {GroundTruthLandmark::jaw_right, 0.75f, -0.75f},
    {GroundTruthLandmark::temple_l, -0.80f, 0.55f},
    {GroundTruthLandmark::temple_r, 0.80f, 0.55f},
    {GroundTruthLandmark::cheek_l, -0.65f, -0.15f},
    {GroundTruthLandmark::cheek_r, 0.65f, -0.15f},
    {GroundTruthLandmark::forehead_center, 0.00f, 1.05f},
    {GroundTruthLandmark::marker_right_cheek, 0.62f, -0.25f},
}};

void render_face(const SyntheticFaceConfig& config,
                 const FacePoseSample& pose,
                 detail::RgbCanvas& canvas) noexcept {
    canvas.fill(kBackground);

    Mat3 inverse{};
    if (!invert(pose.canonical_to_image, inverse)) {
        return;
    }
    const float aspect = static_cast<float>(config.width) / static_cast<float>(config.height);
    const float k1 = config.barrel_k1;

    // Project the canonical face extent to find the pixels worth sampling.
    float min_x = 1e9f;
    float max_x = -1e9f;
    float min_y = 1e9f;
    float max_y = -1e9f;
    const std::array<std::array<float, 2>, 4> corners{{
        {-1.15f, -1.15f},
        {1.15f, -1.15f},
        {-1.15f, 1.75f},
        {1.15f, 1.75f},
    }};
    for (const auto& corner : corners) {
        const auto ideal = SyntheticFaceSource::project_canonical(pose, corner[0], corner[1]);
        const auto distorted = SyntheticFaceSource::distort(ideal, k1, aspect);
        min_x = std::min(min_x, distorted[0]);
        max_x = std::max(max_x, distorted[0]);
        min_y = std::min(min_y, distorted[1]);
        max_y = std::max(max_y, distorted[1]);
    }

    const auto clamp_x = [width = config.width, margin = 2 + static_cast<int>(k1 * 40.0f)](float value) {
        const int pixel = static_cast<int>(std::floor(value * static_cast<float>(width))) - margin;
        return std::clamp(pixel, 0, static_cast<int>(width));
    };
    const auto clamp_y = [height = config.height, margin = 2 + static_cast<int>(k1 * 40.0f)](float value) {
        const int pixel = static_cast<int>(std::floor(value * static_cast<float>(height))) - margin;
        return std::clamp(pixel, 0, static_cast<int>(height));
    };
    const std::uint32_t x0 = static_cast<std::uint32_t>(clamp_x(min_x));
    const std::uint32_t y0 = static_cast<std::uint32_t>(clamp_y(min_y));
    const std::uint32_t x1 = static_cast<std::uint32_t>(std::clamp(
        static_cast<int>(std::ceil(max_x * static_cast<float>(config.width))) + 2 +
            static_cast<int>(k1 * 40.0f),
        0,
        static_cast<int>(config.width)));
    const std::uint32_t y1 = static_cast<std::uint32_t>(std::clamp(
        static_cast<int>(std::ceil(max_y * static_cast<float>(config.height))) + 2 +
            static_cast<int>(k1 * 40.0f),
        0,
        static_cast<int>(config.height)));

    for (std::uint32_t py = y0; py < y1; ++py) {
        const float distorted_y = (static_cast<float>(py) + 0.5f) / static_cast<float>(config.height);
        for (std::uint32_t px = x0; px < x1; ++px) {
            const float distorted_x = (static_cast<float>(px) + 0.5f) / static_cast<float>(config.width);
            float ideal_x = distorted_x;
            float ideal_y = distorted_y;
            if (k1 > 0.0f) {
                undistort_point(distorted_x, distorted_y, k1, aspect, ideal_x, ideal_y);
            }
            const auto canonical = apply(inverse, ideal_x, ideal_y, 1.0f);
            if (std::fabs(canonical[2]) < 1e-9f) {
                continue;
            }
            const float u = canonical[0] / canonical[2];
            const float v = canonical[1] / canonical[2];
            if (u < -1.15f || u > 1.15f || v < -1.15f || v > 1.75f) {
                continue;
            }
            canvas.set(px,
                       py,
                       sample_face(u, v, pose.mouth_open, pose.blink, config.draw_marker));
        }
    }
}

} // namespace

std::vector<FaceMotionKeyframe> default_face_motion_path() {
    return {
        FaceMotionKeyframe{0ull, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f},
        FaceMotionKeyframe{1'000'000'000ull,
                           {0.010f, 0.004f, 0.000f},
                           {20.0f, 8.0f, 4.0f},
                           0.15f,
                           0.0f},
        FaceMotionKeyframe{2'000'000'000ull,
                           {-0.012f, -0.005f, 0.010f},
                           {-18.0f, -10.0f, -5.0f},
                           0.55f,
                           0.0f},
        FaceMotionKeyframe{3'000'000'000ull, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f},
    };
}

SyntheticFaceSource::SyntheticFaceSource(core::FrameClock& clock, SyntheticFaceConfig config)
    : clock_(clock), config_(std::move(config)), ring_(config_.width, config_.height, config_.format) {
    if (config_.path.empty()) {
        config_.path = default_face_motion_path();
    }
}

core::Status SyntheticFaceSource::start() {
    std::array<PlaneLayout, kMaxFramePlanes> layout{};
    if (!compute_plane_layout(config_.format, config_.width, config_.height, layout)) {
        return core::Status::error(core::StatusCode::unsupported,
                                   "synthetic face format or dimensions are not supported");
    }
    if (config_.barrel_k1 < 0.0f) {
        return core::Status::error(core::StatusCode::invalid_argument,
                                   "synthetic face barrel_k1 must be non-negative");
    }
    if (config_.vfov_deg <= 0.0f || config_.vfov_deg >= 180.0f || config_.interocular_m <= 0.0f ||
        config_.base_distance_m <= 0.0f) {
        return core::Status::error(core::StatusCode::invalid_argument,
                                   "synthetic face camera parameters are invalid");
    }
    ring_.reset();
    started_ = true;
    has_last_ = false;
    last_ns_ = 0;
    start_ns_ = clock_.now_ns();
    last_pose_ = evaluate_pose(config_, 0);
    ground_truth_.reset();
    return core::Status::success();
}

void SyntheticFaceSource::stop() {
    started_ = false;
}

FrameRing& SyntheticFaceSource::ring() noexcept {
    return ring_;
}

SourceInfo SyntheticFaceSource::info() const noexcept {
    return SourceInfo{"synthetic_face",
                      config_.width,
                      config_.height,
                      config_.format,
                      pixel_format_has_alpha(config_.format),
                      true,
                      true};
}

std::optional<FaceGroundTruth> SyntheticFaceSource::latest_ground_truth() const {
    return ground_truth_;
}

FacePoseSample SyntheticFaceSource::evaluate_pose(const SyntheticFaceConfig& config,
                                                  TimestampNs elapsed_ns) {
    FacePoseSample pose;
    if (config.path.empty()) {
        return pose;
    }

    const FaceMotionKeyframe* first = &config.path.front();
    const FaceMotionKeyframe* second = &config.path.front();
    float blend = 0.0f;

    if (elapsed_ns <= config.path.front().time_ns) {
        first = &config.path.front();
        second = first;
    } else if (elapsed_ns >= config.path.back().time_ns) {
        first = &config.path.back();
        second = first;
    } else {
        for (std::size_t index = 0; index + 1 < config.path.size(); ++index) {
            if (elapsed_ns >= config.path[index].time_ns && elapsed_ns <= config.path[index + 1].time_ns) {
                first = &config.path[index];
                second = &config.path[index + 1];
                const double span = static_cast<double>(second->time_ns) -
                                    static_cast<double>(first->time_ns);
                const double offset = static_cast<double>(elapsed_ns) -
                                      static_cast<double>(first->time_ns);
                blend = span > 0.0 ? smoothstep01(static_cast<float>(offset / span)) : 0.0f;
                break;
            }
        }
    }

    const float yaw = degrees_to_radians(lerp(first->euler_deg[0], second->euler_deg[0], blend));
    const float pitch = degrees_to_radians(lerp(first->euler_deg[1], second->euler_deg[1], blend));
    const float roll = degrees_to_radians(lerp(first->euler_deg[2], second->euler_deg[2], blend));
    pose.mouth_open = lerp(first->mouth_open, second->mouth_open, blend);
    pose.blink = lerp(first->blink, second->blink, blend);

    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float cr = std::cos(roll);
    const float sr = std::sin(roll);

    // R = Rz(roll) * Ry(yaw) * Rx(pitch).
    const Mat3 rx{1.0f, 0.0f, 0.0f, 0.0f, cp, -sp, 0.0f, sp, cp};
    const Mat3 ry{cy, 0.0f, sy, 0.0f, 1.0f, 0.0f, -sy, 0.0f, cy};
    const Mat3 rz{cr, -sr, 0.0f, sr, cr, 0.0f, 0.0f, 0.0f, 1.0f};
    const Mat3 rotation = multiply(multiply(rz, ry), rx);

    const float tx = lerp(first->translation_m[0], second->translation_m[0], blend);
    const float ty = lerp(first->translation_m[1], second->translation_m[1], blend);
    const float tz = -config.base_distance_m +
                     lerp(first->translation_m[2], second->translation_m[2], blend);

    pose.camera_from_head = {rotation[0], rotation[1], rotation[2], tx,
                             rotation[3], rotation[4], rotation[5], ty,
                             rotation[6], rotation[7], rotation[8], tz,
                             0.0f, 0.0f, 0.0f, 1.0f};

    const float fy = 0.5f / std::tan(degrees_to_radians(config.vfov_deg) * 0.5f);
    const float fx = fy * static_cast<float>(config.height) / static_cast<float>(config.width);
    const float iod = config.interocular_m;

    // Plane-induced homography for head point p = (-u*IOD, v*IOD, 0):
    //   q = R p + t, x = 0.5 + fx*q.x/(-q.z), y = 0.5 - fy*q.y/(-q.z).
    const Mat3 b{-iod * rotation[0], iod * rotation[1], tx,
                 -iod * rotation[3], iod * rotation[4], ty,
                 -iod * rotation[6], iod * rotation[7], tz};
    const Mat3 a{fx, 0.0f, -0.5f, 0.0f, -fy, -0.5f, 0.0f, 0.0f, -1.0f};
    pose.canonical_to_image = multiply(a, b);
    return pose;
}

std::array<float, 2> SyntheticFaceSource::project_canonical(const FacePoseSample& pose,
                                                            float u,
                                                            float v) noexcept {
    const auto point = apply(pose.canonical_to_image, u, v, 1.0f);
    if (std::fabs(point[2]) < 1e-9f) {
        return {0.5f, 0.5f};
    }
    return {point[0] / point[2], point[1] / point[2]};
}

std::array<float, 2> SyntheticFaceSource::distort(std::array<float, 2> ideal,
                                                  float k1,
                                                  float aspect) noexcept {
    if (k1 == 0.0f) {
        return ideal;
    }
    const float dx = (ideal[0] - 0.5f) * aspect;
    const float dy = ideal[1] - 0.5f;
    const float r2 = dx * dx + dy * dy;
    const float factor = 1.0f + k1 * r2;
    return {0.5f + (dx * factor) / aspect, 0.5f + dy * factor};
}

bool SyntheticFaceSource::produce() {
    if (!started_) {
        return false;
    }
    const TimestampNs now = clock_.now_ns();
    if (has_last_ && now <= last_ns_) {
        return false;
    }
    if (!has_last_) {
        start_ns_ = now;
    }
    const TimestampNs elapsed = now - start_ns_;
    const FacePoseSample pose = evaluate_pose(config_, elapsed);

    auto write = ring_.begin_write();
    if (!write) {
        return false;
    }

    detail::RgbCanvas canvas(config_.width, config_.height);
    render_face(config_, pose, canvas);
    if (!canvas.write_to(write->frame())) {
        return false;
    }
    if (!ring_.publish(*write, now, now, false)) {
        return false;
    }

    const float aspect = static_cast<float>(config_.width) / static_cast<float>(config_.height);
    FaceGroundTruth truth;
    truth.timestamp_ns = now;
    truth.sequence = ring_.latest_sequence();
    truth.camera_from_head = pose.camera_from_head;
    truth.features_valid = pose.blink < 0.9f;
    truth.barrel_k1 = config_.barrel_k1;
    truth.correspondence_count = kGroundTruthLandmarkCount;
    for (std::size_t index = 0; index < kGroundTruthLandmarkCount; ++index) {
        const CanonicalPoint& point = kCanonicalPoints[index];
        const auto ideal = project_canonical(pose, point.u, point.v);
        const auto rendered = distort(ideal, config_.barrel_k1, aspect);
        CanonicalCorrespondence& correspondence = truth.correspondences[index];
        correspondence.id = point.id;
        correspondence.name = ground_truth_landmark_name(point.id);
        correspondence.u = point.u;
        correspondence.v = point.v;
        correspondence.x = rendered[0];
        correspondence.y = rendered[1];
        correspondence.ideal_x = ideal[0];
        correspondence.ideal_y = ideal[1];
    }

    last_pose_ = pose;
    ground_truth_ = truth;
    last_ns_ = now;
    has_last_ = true;
    return true;
}

} // namespace avatar::capture
