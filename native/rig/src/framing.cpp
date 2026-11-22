#include "avatar/rig/framing.h"

#include <algorithm>
#include <cmath>

namespace avatar::rig {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEpsilon = 1e-12;

Vec3 subtract(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 scale(const Vec3& a, double s) {
    return {a.x * s, a.y * s, a.z * s};
}

double length(const Vec3& a) {
    return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}

std::array<Vec3, 8> corners(const Aabb& bounds) {
    std::array<Vec3, 8> result{};
    std::size_t index = 0;
    for (int z = 0; z < 2; ++z) {
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 2; ++x) {
                result[index++] = {x == 0 ? bounds.min.x : bounds.max.x,
                                   y == 0 ? bounds.min.y : bounds.max.y,
                                   z == 0 ? bounds.min.z : bounds.max.z};
            }
        }
    }
    return result;
}

Plane normalised_plane(double a, double b, double c, double d) {
    const double n = std::sqrt(a * a + b * b + c * c);
    if (!(n > kEpsilon) || !std::isfinite(n)) {
        return {0.0, 0.0, 0.0, -1.0};
    }
    return {a / n, b / n, c / n, d / n};
}

} // namespace

bool is_finite(double value) {
    return std::isfinite(value);
}

bool is_valid(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool is_valid(const Aabb& bounds) {
    return is_valid(bounds.min) && is_valid(bounds.max) && bounds.min.x <= bounds.max.x &&
           bounds.min.y <= bounds.max.y && bounds.min.z <= bounds.max.z;
}

Vec3 aabb_centre(const Aabb& bounds) {
    return {0.5 * (bounds.min.x + bounds.max.x), 0.5 * (bounds.min.y + bounds.max.y),
            0.5 * (bounds.min.z + bounds.max.z)};
}

Vec3 aabb_size(const Aabb& bounds) {
    return {bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y, bounds.max.z - bounds.min.z};
}

Aabb merge(const Aabb& a, const Aabb& b) {
    if (!is_valid(a)) {
        return b;
    }
    if (!is_valid(b)) {
        return a;
    }
    return {{std::min(a.min.x, b.min.x), std::min(a.min.y, b.min.y), std::min(a.min.z, b.min.z)},
            {std::max(a.max.x, b.max.x), std::max(a.max.y, b.max.y), std::max(a.max.z, b.max.z)}};
}

Mat4 identity() {
    return Mat4{};
}

Mat4 translation(const Vec3& offset) {
    Mat4 result;
    result.m[3] = offset.x;
    result.m[7] = offset.y;
    result.m[11] = offset.z;
    return result;
}

Mat4 rotation_x(double radians) {
    Mat4 result;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    result.m[5] = c;
    result.m[6] = -s;
    result.m[9] = s;
    result.m[10] = c;
    return result;
}

Mat4 rotation_y(double radians) {
    Mat4 result;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    result.m[0] = c;
    result.m[2] = s;
    result.m[8] = -s;
    result.m[10] = c;
    return result;
}

Mat4 rotation_z(double radians) {
    Mat4 result;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    result.m[0] = c;
    result.m[1] = -s;
    result.m[4] = s;
    result.m[5] = c;
    return result;
}

Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 result;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            double value = 0.0;
            for (int k = 0; k < 4; ++k) {
                value += a.m[static_cast<std::size_t>(row * 4 + k)] *
                         b.m[static_cast<std::size_t>(k * 4 + column)];
            }
            result.m[static_cast<std::size_t>(row * 4 + column)] = value;
        }
    }
    return result;
}

Vec3 transform_point(const Mat4& matrix, const Vec3& point) {
    const double x =
        matrix.m[0] * point.x + matrix.m[1] * point.y + matrix.m[2] * point.z + matrix.m[3];
    const double y =
        matrix.m[4] * point.x + matrix.m[5] * point.y + matrix.m[6] * point.z + matrix.m[7];
    const double z =
        matrix.m[8] * point.x + matrix.m[9] * point.y + matrix.m[10] * point.z + matrix.m[11];
    const double w =
        matrix.m[12] * point.x + matrix.m[13] * point.y + matrix.m[14] * point.z + matrix.m[15];
    if (std::abs(w) > kEpsilon) {
        return {x / w, y / w, z / w};
    }
    return {x, y, z};
}

Vec3 transform_direction(const Mat4& matrix, const Vec3& direction) {
    return {matrix.m[0] * direction.x + matrix.m[1] * direction.y + matrix.m[2] * direction.z,
            matrix.m[4] * direction.x + matrix.m[5] * direction.y + matrix.m[6] * direction.z,
            matrix.m[8] * direction.x + matrix.m[9] * direction.y + matrix.m[10] * direction.z};
}

Aabb transform_aabb(const Mat4& matrix, const Aabb& bounds) {
    if (!is_valid(bounds)) {
        return bounds;
    }
    const std::array<Vec3, 8> input = corners(bounds);
    Aabb result{transform_point(matrix, input[0]), transform_point(matrix, input[0])};
    for (std::size_t i = 1; i < input.size(); ++i) {
        const Vec3 p = transform_point(matrix, input[i]);
        result.min.x = std::min(result.min.x, p.x);
        result.min.y = std::min(result.min.y, p.y);
        result.min.z = std::min(result.min.z, p.z);
        result.max.x = std::max(result.max.x, p.x);
        result.max.y = std::max(result.max.y, p.y);
        result.max.z = std::max(result.max.z, p.z);
    }
    return result;
}

bool clamp_zoom(double zoom, double& clamped) {
    if (!std::isfinite(zoom)) {
        return false;
    }
    clamped = std::clamp(zoom, kMinZoom, kMaxZoom);
    return true;
}

bool clamp_margin(double margin, double& clamped) {
    if (!std::isfinite(margin)) {
        return false;
    }
    clamped = std::clamp(margin, kMinMargin, kMaxMargin);
    return true;
}

Vec3 aabb_bounding_sphere_centre(const Aabb& bounds) {
    return aabb_centre(bounds);
}

double aabb_bounding_sphere_radius(const Aabb& bounds) {
    if (!is_valid(bounds)) {
        return 0.0;
    }
    const Vec3 size = aabb_size(bounds);
    return 0.5 * std::sqrt(size.x * size.x + size.y * size.y + size.z * size.z);
}

HeadSweep head_sweep(const Aabb& head_bounds, const Vec3& pivot, double sweep_degrees) {
    HeadSweep result;
    result.pivot = pivot;
    result.sweep_degrees = sweep_degrees;
    if (!is_valid(head_bounds) || !is_valid(pivot)) {
        result.bounds = {{0.0, 0.0, 0.0}, {-1.0, -1.0, -1.0}};
        return result;
    }
    double radius = 0.0;
    for (const Vec3& corner : corners(head_bounds)) {
        radius = std::max(radius, length(subtract(corner, pivot)));
    }
    result.radius = radius;
    result.bounds = {{pivot.x - radius, pivot.y - radius, pivot.z - radius},
                     {pivot.x + radius, pivot.y + radius, pivot.z + radius}};
    return result;
}

double framing_distance(double radius, double theta, double margin, double zoom) {
    if (!(radius > 0.0) || !(theta > 0.0) || !(zoom > 0.0) || !std::isfinite(radius) ||
        !std::isfinite(theta) || !std::isfinite(margin) || !std::isfinite(zoom)) {
        return 0.0;
    }
    return (radius / std::sin(theta)) * (1.0 + margin) / zoom;
}

Mat4 perspective(double vertical_fov_radians, double aspect, double near_plane, double far_plane) {
    Mat4 result;
    const double f = 1.0 / std::tan(vertical_fov_radians * 0.5);
    result.m[0] = f / aspect;
    result.m[5] = f;
    result.m[10] = (far_plane + near_plane) / (near_plane - far_plane);
    result.m[11] = (2.0 * far_plane * near_plane) / (near_plane - far_plane);
    result.m[14] = -1.0;
    result.m[15] = 0.0;
    return result;
}

std::array<Plane, 6> frustum_planes_from_clip(const Mat4& clip_from_world) {
    const auto& m = clip_from_world.m;
    const auto row = [&m](int index) {
        return std::array<double, 4>{
            m[static_cast<std::size_t>(index * 4)], m[static_cast<std::size_t>(index * 4 + 1)],
            m[static_cast<std::size_t>(index * 4 + 2)], m[static_cast<std::size_t>(index * 4 + 3)]};
    };
    const std::array<double, 4> r0 = row(0);
    const std::array<double, 4> r1 = row(1);
    const std::array<double, 4> r2 = row(2);
    const std::array<double, 4> r3 = row(3);
    const auto combine = [](const std::array<double, 4>& a, const std::array<double, 4>& b,
                            double sign) {
        return normalised_plane(a[0] + sign * b[0], a[1] + sign * b[1], a[2] + sign * b[2],
                                a[3] + sign * b[3]);
    };
    return {combine(r3, r0, 1.0),  combine(r3, r0, -1.0), combine(r3, r1, 1.0),
            combine(r3, r1, -1.0), combine(r3, r2, 1.0),  combine(r3, r2, -1.0)};
}

bool aabb_inside_frustum(const Aabb& bounds, const std::array<Plane, 6>& planes, double epsilon) {
    if (!is_valid(bounds)) {
        return false;
    }
    for (const Plane& plane : planes) {
        const double x = plane.a >= 0.0 ? bounds.min.x : bounds.max.x;
        const double y = plane.b >= 0.0 ? bounds.min.y : bounds.max.y;
        const double z = plane.c >= 0.0 ? bounds.min.z : bounds.max.z;
        if (plane.a * x + plane.b * y + plane.c * z + plane.d < -epsilon) {
            return false;
        }
    }
    return true;
}

FramingResult make_framing(const FramingInput& input, const FramingSettings& settings) {
    FramingResult result;
    result.settings = settings;
    result.model_bounds = input.model_bounds;
    if (!is_valid(input.model_bounds)) {
        return result;
    }
    if (!std::isfinite(input.aspect) || input.aspect <= 0.0 ||
        !std::isfinite(input.vertical_fov_degrees) || input.vertical_fov_degrees <= 0.0 ||
        input.vertical_fov_degrees >= 180.0) {
        return result;
    }
    if (!clamp_zoom(settings.zoom, result.settings.zoom) ||
        !clamp_margin(settings.margin, result.settings.margin)) {
        return result;
    }
    if (!std::isfinite(result.settings.offset_x) || !std::isfinite(result.settings.offset_y)) {
        return result;
    }
    if (!is_valid(input.head_pivot)) {
        return result;
    }

    result.head_sweep =
        head_sweep(input.has_head_bounds ? input.head_bounds : Aabb{}, input.head_pivot,
                   std::clamp(input.head_sweep_degrees, 0.0, 180.0));
    result.swept_bounds = merge(input.model_bounds, result.head_sweep.bounds);

    result.vertical_fov_degrees = input.vertical_fov_degrees;
    result.aspect = input.aspect;
    const double vertical_radians = input.vertical_fov_degrees * kPi / 180.0;
    result.theta_vertical = vertical_radians * 0.5;
    result.theta_horizontal = std::atan(std::tan(result.theta_vertical) * input.aspect);
    result.theta = std::min(result.theta_vertical, result.theta_horizontal);

    if (result.settings.fit == FramingFit::head) {
        if (!is_valid(input.face_origin) || !std::isfinite(input.face_scale) ||
            input.face_scale <= 0.0) {
            return result;
        }
        result.fitted_centre = input.face_origin;
        result.fitted_radius = 1.5 * input.face_scale;
    } else {
        result.fitted_centre = aabb_centre(result.swept_bounds);
        result.fitted_radius = aabb_bounding_sphere_radius(result.swept_bounds);
    }
    if (!(result.fitted_radius > 0.0) || !std::isfinite(result.fitted_radius)) {
        return result;
    }

    result.distance = framing_distance(result.fitted_radius, result.theta, result.settings.margin,
                                       result.settings.zoom);
    if (!(result.distance > 0.0) || !std::isfinite(result.distance)) {
        return result;
    }

    result.visible_half_height = result.distance * std::tan(result.theta_vertical);
    result.visible_half_width = result.visible_half_height * input.aspect;
    result.offset = {result.settings.offset_x * result.visible_half_width,
                     result.settings.offset_y * result.visible_half_height, 0.0};

    result.view_from_model = translation(result.offset) * rotation_y(kPi) *
                             translation(scale(result.fitted_centre, -1.0));
    result.camera_position = {0.0, 0.0, result.distance};

    // Near/far only have to contain the fitted volume. A fit-head avatar may
    // deliberately crop the body outside this range.
    result.near_plane = std::max(0.001, result.distance - result.fitted_radius * 1.5);
    result.far_plane =
        std::max(result.near_plane + 0.001, result.distance + result.fitted_radius * 1.5 + 0.5);

    const Mat4 clip_from_world =
        perspective(vertical_radians, input.aspect, result.near_plane, result.far_plane) *
        translation({0.0, 0.0, -result.distance});
    result.clip_from_model = clip_from_world * result.view_from_model;
    // Planes are extracted in world space so the editor can draw them beside
    // the presented model without reapplying the presentation transform.
    result.frustum_planes = frustum_planes_from_clip(clip_from_world);

    std::size_t corner_index = 0;
    for (const double plane_distance : {result.near_plane, result.far_plane}) {
        const double half_height = plane_distance * std::tan(result.theta_vertical);
        const double half_width = half_height * result.aspect;
        const double z = result.distance - plane_distance;
        for (const double y : {-half_height, half_height}) {
            for (const double x : {-half_width, half_width}) {
                result.frustum_corners[corner_index++] = {x, y, z};
            }
        }
    }
    result.valid = true;
    return result;
}

double mirror_uv(double u) {
    return 1.0 - u;
}

int mirror_pixel_x(int x, int width) {
    return width - 1 - x;
}

void mirror_rgba8(std::uint8_t* pixels, int width, int height) {
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return;
    }
    for (int y = 0; y < height; ++y) {
        std::uint8_t* row =
            pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width / 2; ++x) {
            std::uint8_t* left = row + static_cast<std::size_t>(x) * 4;
            std::uint8_t* right = row + static_cast<std::size_t>(width - 1 - x) * 4;
            for (int channel = 0; channel < 4; ++channel) {
                std::swap(left[channel], right[channel]);
            }
        }
    }
}

} // namespace avatar::rig
