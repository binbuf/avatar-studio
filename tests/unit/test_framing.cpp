#include "test_registry.h"

#include "avatar/rig/framing.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using avatar::rig::Aabb;
using avatar::rig::FramingFit;
using avatar::rig::FramingInput;
using avatar::rig::FramingResult;
using avatar::rig::FramingSettings;
using avatar::rig::Mat4;
using avatar::rig::Plane;
using avatar::rig::Vec3;

constexpr double kPi = 3.14159265358979323846;

bool close(double actual, double expected, double epsilon = 1e-9) {
    return std::abs(actual - expected) <= epsilon;
}

bool point_inside(const Vec3& point, const std::array<Plane, 6>& planes, double epsilon = 1e-6) {
    for (const Plane& plane : planes) {
        if (plane.a * point.x + plane.b * point.y + plane.c * point.z + plane.d < -epsilon) {
            return false;
        }
    }
    return true;
}

Vec3 rotate_about(const Mat4& rotation, const Vec3& pivot, const Vec3& point) {
    const Vec3 local{point.x - pivot.x, point.y - pivot.y, point.z - pivot.z};
    const Vec3 turned = avatar::rig::transform_direction(rotation, local);
    return {pivot.x + turned.x, pivot.y + turned.y, pivot.z + turned.z};
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

FramingInput sample_input() {
    FramingInput input;
    input.model_bounds = {{-0.40, 0.00, -0.22}, {0.40, 1.60, 0.22}};
    input.has_head_bounds = true;
    input.head_bounds = {{-0.24, 1.08, -0.16}, {0.24, 1.55, 0.16}};
    input.head_pivot = {0.0, 1.02, 0.05};
    input.face_origin = {0.0, 1.32, -0.15};
    input.face_scale = 0.08;
    input.aspect = 16.0 / 9.0;
    return input;
}

} // namespace

TEST_CASE(framing_distance_matches_the_documented_formula) {
    const double theta = 17.5 * kPi / 180.0;
    const double expected = (1.0 / std::sin(theta)) * 1.1;
    CHECK(close(avatar::rig::framing_distance(1.0, theta, 0.10, 1.0), expected, 1e-12));

    const double base = avatar::rig::framing_distance(1.0, theta, 0.10, 1.0);
    CHECK(close(avatar::rig::framing_distance(1.0, theta, 0.10, 2.0), base * 0.5, 1e-12));
    CHECK(close(avatar::rig::framing_distance(1.0, theta, 0.0, 1.0), base / 1.1, 1e-12));
    CHECK(close(avatar::rig::framing_distance(2.0, theta, 0.10, 1.0), base * 2.0, 1e-12));
    CHECK(close(avatar::rig::framing_distance(1.0, theta, 0.0, 0.0), 0.0));
}

TEST_CASE(framing_clamps_zoom_and_margin_to_documented_ranges) {
    double value = 0.0;
    CHECK(avatar::rig::clamp_zoom(0.1, value));
    CHECK(close(value, avatar::rig::kMinZoom));
    CHECK(avatar::rig::clamp_zoom(9.0, value));
    CHECK(close(value, avatar::rig::kMaxZoom));
    CHECK(avatar::rig::clamp_zoom(1.25, value));
    CHECK(close(value, 1.25));
    CHECK(!avatar::rig::clamp_zoom(std::nan(""), value));
    CHECK(!avatar::rig::clamp_zoom(INFINITY, value));

    CHECK(avatar::rig::clamp_margin(-1.0, value));
    CHECK(close(value, avatar::rig::kMinMargin));
    CHECK(avatar::rig::clamp_margin(2.0, value));
    CHECK(close(value, avatar::rig::kMaxMargin));
    CHECK(avatar::rig::clamp_margin(0.10, value));
    CHECK(close(value, 0.10));
    CHECK(!avatar::rig::clamp_margin(std::nan(""), value));
}

TEST_CASE(view_from_model_places_the_fitted_centre_and_keeps_the_180_degree_yaw) {
    FramingInput input = sample_input();
    FramingSettings settings;
    const FramingResult result = avatar::rig::make_framing(input, settings);
    CHECK(result.valid);

    const Vec3 centre = avatar::rig::transform_point(result.view_from_model, result.fitted_centre);
    CHECK(close(centre.x, 0.0, 1e-12));
    CHECK(close(centre.y, 0.0, 1e-12));
    CHECK(close(centre.z, 0.0, 1e-12));

    // 180 degrees about Y: the model's right (+X) stays on the viewer's left.
    const Vec3 right = avatar::rig::transform_direction(result.view_from_model, {1.0, 0.0, 0.0});
    const Vec3 forward = avatar::rig::transform_direction(result.view_from_model, {0.0, 0.0, 1.0});
    CHECK(close(right.x, -1.0, 1e-12));
    CHECK(close(right.y, 0.0, 1e-12));
    CHECK(close(forward.z, -1.0, 1e-12));

    // Positive offset x moves the avatar to the viewer's right; positive y up.
    FramingSettings nudged = settings;
    nudged.offset_x = 0.25;
    nudged.offset_y = -0.50;
    const FramingResult moved = avatar::rig::make_framing(input, nudged);
    CHECK(moved.valid);
    const Vec3 moved_centre =
        avatar::rig::transform_point(moved.view_from_model, moved.fitted_centre);
    CHECK(close(moved_centre.x, 0.25 * moved.visible_half_width, 1e-12));
    CHECK(close(moved_centre.y, -0.50 * moved.visible_half_height, 1e-12));
    CHECK(moved_centre.x > 0.0);
    CHECK(moved_centre.y < 0.0);

    CHECK(close(result.camera_position.z, result.distance, 1e-12));
    CHECK(result.distance > result.fitted_radius);
}

TEST_CASE(head_sweep_contains_every_corner_at_the_45_degree_limits) {
    const Aabb head = {{-0.24, 1.08, -0.16}, {0.24, 1.55, 0.16}};
    const Vec3 pivot{0.0, 1.02, 0.05};
    const avatar::rig::HeadSweep sweep = avatar::rig::head_sweep(head, pivot, 45.0);
    CHECK(sweep.radius > 0.0);
    CHECK(avatar::rig::is_valid(sweep.bounds));

    const double limit = 45.0 * kPi / 180.0;
    for (const double yaw : {-limit, 0.0, limit}) {
        for (const double pitch : {-limit, 0.0, limit}) {
            for (const double roll : {-limit, 0.0, limit}) {
                const Mat4 rotation = avatar::rig::rotation_y(yaw) *
                                      avatar::rig::rotation_x(pitch) *
                                      avatar::rig::rotation_z(roll);
                for (const Vec3& corner : corners(head)) {
                    const Vec3 turned = rotate_about(rotation, pivot, corner);
                    CHECK(turned.x >= sweep.bounds.min.x - 1e-9);
                    CHECK(turned.x <= sweep.bounds.max.x + 1e-9);
                    CHECK(turned.y >= sweep.bounds.min.y - 1e-9);
                    CHECK(turned.y <= sweep.bounds.max.y + 1e-9);
                    CHECK(turned.z >= sweep.bounds.min.z - 1e-9);
                    CHECK(turned.z <= sweep.bounds.max.z + 1e-9);
                }
            }
        }
    }
}

TEST_CASE(model_fit_keeps_the_swept_aabb_inside_the_frustum) {
    const FramingInput input = sample_input();
    FramingSettings settings;
    settings.margin = 0.0;
    const FramingResult result = avatar::rig::make_framing(input, settings);
    CHECK(result.valid);
    CHECK(avatar::rig::aabb_inside_frustum(
        avatar::rig::transform_aabb(result.view_from_model, result.swept_bounds),
        result.frustum_planes));
    CHECK(avatar::rig::aabb_inside_frustum(
        avatar::rig::transform_aabb(result.view_from_model, input.model_bounds),
        result.frustum_planes));

    // The fitted sphere touches the narrower half-FOV exactly at margin 0.
    CHECK(close(std::asin(result.fitted_radius / result.distance), result.theta, 1e-9));

    const double limit = 45.0 * kPi / 180.0;
    for (const double yaw : {-limit, 0.0, limit}) {
        for (const double pitch : {-limit, 0.0, limit}) {
            for (const double roll : {-limit, 0.0, limit}) {
                const Mat4 rotation = avatar::rig::rotation_y(yaw) *
                                      avatar::rig::rotation_x(pitch) *
                                      avatar::rig::rotation_z(roll);
                for (const Vec3& corner : corners(input.head_bounds)) {
                    const Vec3 turned = rotate_about(rotation, input.head_pivot, corner);
                    CHECK(point_inside(avatar::rig::transform_point(result.view_from_model, turned),
                                       result.frustum_planes));
                }
            }
        }
    }

    for (const Vec3& corner : corners(input.model_bounds)) {
        CHECK(point_inside(avatar::rig::transform_point(result.view_from_model, corner),
                           result.frustum_planes));
    }
}

TEST_CASE(head_fit_uses_the_face_sphere_and_keeps_it_inside) {
    FramingInput input = sample_input();
    FramingSettings settings;
    settings.fit = FramingFit::head;
    settings.margin = 0.0;
    const FramingResult result = avatar::rig::make_framing(input, settings);
    CHECK(result.valid);
    CHECK(close(result.fitted_centre.x, input.face_origin.x));
    CHECK(close(result.fitted_centre.y, input.face_origin.y));
    CHECK(close(result.fitted_centre.z, input.face_origin.z));
    CHECK(close(result.fitted_radius, 1.5 * input.face_scale, 1e-12));
    CHECK(close(std::asin(result.fitted_radius / result.distance), result.theta, 1e-9));

    const Vec3 centre = avatar::rig::transform_point(result.view_from_model, result.fitted_centre);
    CHECK(close(centre.x, 0.0, 1e-12));
    CHECK(close(centre.y, 0.0, 1e-12));
    CHECK(close(centre.z, 0.0, 1e-12));

    // The body may crop; the fitted head sphere may not.
    for (const Vec3& axis : {Vec3{1, 0, 0}, Vec3{-1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, -1, 0},
                             Vec3{0, 0, 1}, Vec3{0, 0, -1}}) {
        const Vec3 point{result.fitted_centre.x + axis.x * result.fitted_radius,
                         result.fitted_centre.y + axis.y * result.fitted_radius,
                         result.fitted_centre.z + axis.z * result.fitted_radius};
        CHECK(point_inside(avatar::rig::transform_point(result.view_from_model, point),
                           result.frustum_planes));
    }
}

TEST_CASE(extreme_zoom_and_margin_keep_the_fit_and_crop_only_above_one) {
    const FramingInput input = sample_input();
    for (const double zoom : {avatar::rig::kMinZoom, 1.0}) {
        for (const double margin : {avatar::rig::kMinMargin, 0.10, avatar::rig::kMaxMargin}) {
            FramingSettings settings;
            settings.zoom = zoom;
            settings.margin = margin;
            const FramingResult result = avatar::rig::make_framing(input, settings);
            CHECK(result.valid);
            CHECK(close(result.settings.zoom, zoom));
            CHECK(close(result.settings.margin, margin));
            CHECK(avatar::rig::aabb_inside_frustum(
                avatar::rig::transform_aabb(result.view_from_model, result.swept_bounds),
                result.frustum_planes));
        }
    }

    // Zoom > 1 is a deliberate crop, not a fit failure. The documented
    // containment guarantee is for zoom <= 1.
    FramingSettings cropped;
    cropped.zoom = avatar::rig::kMaxZoom;
    const FramingResult result = avatar::rig::make_framing(input, cropped);
    CHECK(result.valid);
    CHECK(result.fitted_radius / result.distance > std::sin(result.theta));
    CHECK(!avatar::rig::aabb_inside_frustum(
        avatar::rig::transform_aabb(result.view_from_model, result.swept_bounds),
        result.frustum_planes));

    FramingSettings out_of_range;
    out_of_range.zoom = 0.1;
    out_of_range.margin = 3.0;
    const FramingResult clamped = avatar::rig::make_framing(input, out_of_range);
    CHECK(clamped.valid);
    CHECK(close(clamped.settings.zoom, avatar::rig::kMinZoom));
    CHECK(close(clamped.settings.margin, avatar::rig::kMaxMargin));
}

TEST_CASE(frustum_corners_and_planes_agree_and_a_point_outside_is_rejected) {
    const FramingInput input = sample_input();
    const FramingResult result = avatar::rig::make_framing(input, FramingSettings{});
    CHECK(result.valid);
    for (const Vec3& corner : result.frustum_corners) {
        CHECK(point_inside(corner, result.frustum_planes, 1e-6));
    }
    CHECK(!point_inside({result.visible_half_width * 2.0, 0.0, 0.0}, result.frustum_planes));
    CHECK(!point_inside({0.0, result.visible_half_height * 2.0, 0.0}, result.frustum_planes));
    CHECK(!point_inside({0.0, 0.0, result.distance + 1.0}, result.frustum_planes));
}

TEST_CASE(mirroring_is_a_final_frame_flip_and_an_involution) {
    constexpr int width = 4;
    constexpr int height = 2;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width * height * 4), 0);
    const auto set = [&pixels](int x, int y, std::uint8_t value) {
        const std::size_t index = static_cast<std::size_t>((y * width + x) * 4);
        pixels[index] = value;
        pixels[index + 3] = 255;
    };
    set(0, 0, 10);
    set(1, 0, 20);
    set(3, 1, 30);

    avatar::rig::mirror_rgba8(pixels.data(), width, height);
    CHECK_EQ(pixels[static_cast<std::size_t>((0 * 4 + 0) * 4)], std::uint8_t{0});
    CHECK_EQ(pixels[static_cast<std::size_t>((0 * 4 + 2) * 4)], std::uint8_t{20});
    CHECK_EQ(pixels[static_cast<std::size_t>((0 * 4 + 3) * 4)], std::uint8_t{10});
    CHECK_EQ(pixels[static_cast<std::size_t>((1 * 4 + 0) * 4)], std::uint8_t{30});

    avatar::rig::mirror_rgba8(pixels.data(), width, height);
    CHECK_EQ(pixels[static_cast<std::size_t>((0 * 4 + 0) * 4)], std::uint8_t{10});
    CHECK_EQ(pixels[static_cast<std::size_t>((0 * 4 + 1) * 4)], std::uint8_t{20});
    CHECK_EQ(pixels[static_cast<std::size_t>((1 * 4 + 3) * 4)], std::uint8_t{30});

    CHECK(close(avatar::rig::mirror_uv(0.25), 0.75));
    CHECK_EQ(avatar::rig::mirror_pixel_x(1, 4), 2);
    CHECK_EQ(avatar::rig::mirror_pixel_x(2, 4), 1);
    CHECK_EQ(avatar::rig::mirror_pixel_x(0, 4), 3);
}

TEST_CASE(invalid_framing_input_is_rejected_without_nan) {
    FramingInput input = sample_input();
    input.model_bounds.min.x = 1.0;
    input.model_bounds.max.x = -1.0;
    CHECK(!avatar::rig::make_framing(input, FramingSettings{}).valid);

    input = sample_input();
    input.aspect = 0.0;
    CHECK(!avatar::rig::make_framing(input, FramingSettings{}).valid);

    input = sample_input();
    input.face_scale = 0.0;
    FramingSettings head;
    head.fit = FramingFit::head;
    CHECK(!avatar::rig::make_framing(input, head).valid);

    input = sample_input();
    FramingSettings bad_zoom;
    bad_zoom.zoom = std::nan("");
    CHECK(!avatar::rig::make_framing(input, bad_zoom).valid);

    input = sample_input();
    FramingSettings bad_offset;
    bad_offset.offset_x = INFINITY;
    CHECK(!avatar::rig::make_framing(input, bad_offset).valid);

    input = sample_input();
    input.head_pivot = {std::nan(""), 0.0, 0.0};
    CHECK(!avatar::rig::make_framing(input, FramingSettings{}).valid);
}
