#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace avatar::rig {

// Framework-free presentation and framing math (T22).
//
// The realised contract is documented in docs/design/face-compositing.md and
// ADR-0032. All values are plain CPU data; matrices are row-major, multiply
// column vectors and use metres. No Eigen or engine ABI crosses this header.
//
// Coordinate spaces:
//   model   - rest model space, the imported bind pose before presentation.
//   view    - the presentation/world frame of the avatar SubViewport. The
//             camera sits at (0, 0, distance) looking down -Z at the origin.
//   clip    - homogeneous clip space of that camera.

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct Aabb {
    Vec3 min;
    Vec3 max;
};

// 4x4 row-major matrix; p' = M * p for a column vector p.
struct Mat4 {
    std::array<double, 16> m{1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                             0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
};

// Inward-facing half-space: a*x + b*y + c*z + d >= 0 is inside.
struct Plane {
    double a = 0.0;
    double b = 0.0;
    double c = 0.0;
    double d = 0.0;
};

enum class FramingFit { model, head };

struct FramingSettings {
    FramingFit fit = FramingFit::model;
    double zoom = 1.0;
    double margin = 0.10;
    double offset_x = 0.0; // fractions of the visible half-extent
    double offset_y = 0.0;
};

struct FramingInput {
    Aabb model_bounds;
    bool has_head_bounds = false;
    Aabb head_bounds;
    Vec3 head_pivot;
    Vec3 face_origin;
    double face_scale = 0.0; // inter-ocular distance in metres
    double aspect = 16.0 / 9.0;
    double vertical_fov_degrees = 35.0;
    double head_sweep_degrees = 45.0;
};

struct HeadSweep {
    Vec3 pivot;
    double radius = 0.0;
    double sweep_degrees = 45.0;
    Aabb bounds; // pivot +/- radius; contains every head point under the sweep
};

struct FramingResult {
    bool valid = false;
    FramingSettings settings;

    Aabb model_bounds;
    Aabb swept_bounds;
    HeadSweep head_sweep;
    Vec3 fitted_centre;
    double fitted_radius = 0.0;

    double vertical_fov_degrees = 35.0;
    double aspect = 16.0 / 9.0;
    double theta_vertical = 0.0;
    double theta_horizontal = 0.0;
    double theta = 0.0; // the smaller half-FOV, used by the distance formula
    double distance = 0.0;
    double near_plane = 0.0;
    double far_plane = 0.0;

    double visible_half_width = 0.0;
    double visible_half_height = 0.0;
    Vec3 offset; // world-space offset implied by the fractional nudge

    Mat4 view_from_model;
    Mat4 clip_from_model;
    Vec3 camera_position;
    std::array<Plane, 6> frustum_planes;
    std::array<Vec3, 8> frustum_corners; // world space
};

// Documented ranges. zoom 1.0 is the computed fit; zoom > 1 deliberately crops.
constexpr double kDefaultMargin = 0.10;
constexpr double kMinZoom = 0.5;
constexpr double kMaxZoom = 2.0;
constexpr double kMinMargin = 0.0;
constexpr double kMaxMargin = 0.5;
constexpr double kDefaultVerticalFovDegrees = 35.0;
constexpr double kHeadSweepDegrees = 45.0;

bool is_finite(double value);
bool is_valid(const Vec3& value);
bool is_valid(const Aabb& bounds);
Vec3 aabb_centre(const Aabb& bounds);
Vec3 aabb_size(const Aabb& bounds);
Aabb merge(const Aabb& a, const Aabb& b);

Mat4 identity();
Mat4 translation(const Vec3& offset);
Mat4 rotation_x(double radians);
Mat4 rotation_y(double radians);
Mat4 rotation_z(double radians);
Mat4 operator*(const Mat4& a, const Mat4& b);
Vec3 transform_point(const Mat4& matrix, const Vec3& point);
Vec3 transform_direction(const Mat4& matrix, const Vec3& direction);
Aabb transform_aabb(const Mat4& matrix, const Aabb& bounds);

// Clamping is explicit: non-finite input is invalid, finite input is clamped.
bool clamp_zoom(double zoom, double& clamped);
bool clamp_margin(double margin, double& clamped);

// Bounding sphere of an AABB: centre plus half the diagonal.
Vec3 aabb_bounding_sphere_centre(const Aabb& bounds);
double aabb_bounding_sphere_radius(const Aabb& bounds);

// Conservative swept head bound. Any rotation of the head about `pivot` keeps
// every head point inside a sphere of radius max|corner - pivot|, so the +/-45
// degree sweep is contained for every axis combination.
HeadSweep head_sweep(const Aabb& head_bounds, const Vec3& pivot, double sweep_degrees);

// D = (r / sin(theta)) * (1 + margin) / zoom.
double framing_distance(double radius, double theta, double margin, double zoom);

// Perspective projection for a camera at the origin looking down -Z. The
// resulting matrix maps view z=-near to -1 and view z=-far to +1.
Mat4 perspective(double vertical_fov_radians, double aspect, double near_plane, double far_plane);

// Extract inward-facing world-space planes from a clip-from-world matrix.
std::array<Plane, 6> frustum_planes_from_clip(const Mat4& clip_from_world);
bool aabb_inside_frustum(const Aabb& bounds, const std::array<Plane, 6>& planes,
                         double epsilon = 1e-9);

// Builds the whole realised framing. Invalid or non-finite input returns a
// result with valid=false; no NaN reaches a caller.
FramingResult make_framing(const FramingInput& input, const FramingSettings& settings);

// Display-only preview mapping and final-frame output flip. `mirror_uv` maps a
// normalised horizontal coordinate; `mirror_rgba8` flips a composited RGBA8
// frame in place, so a decal or prop attached to the model moves with it.
double mirror_uv(double u);
int mirror_pixel_x(int x, int width);
void mirror_rgba8(std::uint8_t* pixels, int width, int height);

} // namespace avatar::rig
