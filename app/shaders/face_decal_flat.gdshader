// Flat face decal pass (T20).
//
// One quad per face region, parented to the head. The quad is placed on the
// avatar's calibrated face plane in head space; the fragment shader projects
// its world position back to canonical face space, masks the region, and
// samples the camera through the canonical-to-image homography.
//
// Contract:
//   - depth test on, depth write off, transparent pass (blend_mix);
//   - discard outside the feathered region mask and outside [0,1] image UV;
//   - the camera texture is sRGB, converted to linear before blending;
//   - the camera sampler is repeat_disable, so sampling never wraps.
// The flat mode is a fallback/debug mode: it cannot foreshorten or self-occlude
// like the model surface. See docs/design/face-compositing.md.

shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never;

uniform sampler2D camera_tex : filter_linear, repeat_disable;

// Homography columns: canonical (u, v, 1) -> image (x, y, w).
uniform mat3 canonical_to_image = mat3(
        vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0));

// Region rectangle in canonical face space: centre (x, y) and full size (w, h).
uniform vec4 region_uv = vec4(0.0, 0.0, 1.0, 1.0);
// Slow landmark anchor offset in canonical units, added to the mask centre.
uniform vec2 region_anchor = vec2(0.0);
uniform float region_rot = 0.0;
uniform float feather = 0.18;
uniform int region_shape = 0; // 0 = ellipse, 1 = rounded rectangle
uniform float corner_radius = 0.35;
uniform vec3 colour_gain = vec3(1.0);
uniform vec3 colour_lift = vec3(0.0);
uniform float opacity = 1.0;

// Calibrated face plane in world space. The quad vertices are authored in head
// space, so the fragment uses world position; this keeps the projection correct
// when the model is framed by a presentation transform.
uniform vec3 face_origin = vec3(0.0);
uniform vec3 face_right = vec3(1.0, 0.0, 0.0);
uniform vec3 face_up = vec3(0.0, 1.0, 0.0);
uniform float face_scale = 1.0;

varying vec3 flat_world_position;
varying vec3 flat_world_normal;

vec3 srgb_to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)),
            step(vec3(0.04045), c));
}

// Signed distance in half-size units: -1 at the centre, 0 on the region edge.
float region_sdf(vec2 d) {
    if (region_shape == 1) {
        vec2 q = abs(d) - vec2(1.0 - corner_radius);
        return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - corner_radius;
    }
    return length(d) - 1.0;
}

void vertex() {
    flat_world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    flat_world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
}

void fragment() {
    vec2 canonical = vec2(
            dot(flat_world_position - face_origin, face_right),
            dot(flat_world_position - face_origin, face_up)) / face_scale;

    vec2 d = (canonical - (region_uv.xy + region_anchor)) / (region_uv.zw * 0.5);
    float c = cos(region_rot);
    float s = sin(region_rot);
    d = mat2(vec2(c, -s), vec2(s, c)) * d;

    float f = max(feather, 0.00001);
    float mask = 1.0 - smoothstep(-f, 0.0, region_sdf(d));
    if (mask <= 0.0) {
        discard;
    }

    vec3 h = canonical_to_image * vec3(canonical, 1.0);
    if (h.z <= 0.0) {
        discard;
    }
    vec2 image_uv = h.xy / h.z;
    if (image_uv.x < 0.0 || image_uv.x > 1.0 ||
            image_uv.y < 0.0 || image_uv.y > 1.0) {
        discard;
    }

    vec3 camera_linear = srgb_to_linear(texture(camera_tex, image_uv).rgb);
    vec3 view = normalize(CAMERA_POSITION_WORLD - flat_world_position);
    float facing = smoothstep(0.10, 0.35, dot(normalize(flat_world_normal), view));

    ALBEDO = camera_linear * colour_gain + colour_lift;
    ALPHA = mask * facing * facing * opacity;
}
