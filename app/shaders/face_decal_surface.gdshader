// T21: one shared-geometry pass for all three regions. Default depth test is
// LEQUAL policy (Godot 4.7 reversed-depth GEQUAL); never write decal depth.
shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never, cull_disabled;

#include "res://shaders/yuv_convert.gdshaderinc"

uniform sampler2D camera_tex : filter_linear, repeat_disable;
uniform sampler2D camera_y_tex : filter_linear, repeat_disable;
uniform sampler2D camera_uv_tex : filter_linear, repeat_disable;
uniform bool camera_is_yuv = false;
uniform int camera_matrix = 0; // 0 = BT.601, 1 = BT.709
uniform int camera_range = 0;  // 0 = limited, 1 = full
uniform bool camera_bgra = false;
uniform mat3 canonical_to_image = mat3(1.0);
uniform vec3 face_origin = vec3(0.0);
uniform vec3 face_right = vec3(1.0, 0.0, 0.0);
uniform vec3 face_up = vec3(0.0, 1.0, 0.0);
uniform vec3 face_normal = vec3(0.0, 0.0, -1.0);
uniform float face_scale = 1.0;
uniform float depth_bias = 0.000001; // NDC units towards the camera, not a geometry offset.
uniform int region_count = 0;
uniform vec4 region_rect[3];
uniform vec2 region_anchor[3];
// rotation (radians), feather, shape (0 ellipse / 1 rounded rect), corner radius
uniform vec4 region_style[3];
uniform vec3 colour_gain[3];
uniform vec3 colour_lift[3];
uniform float region_opacity[3];

varying vec3 rest_pos;
varying vec3 rest_normal;
varying vec3 posed_pos;
varying vec3 posed_normal;

vec3 srgb_to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 sample_camera(vec2 uv) {
    if (camera_is_yuv) {
        float y = texture(camera_y_tex, uv).r;
        vec2 chroma = texture(camera_uv_tex, uv).rg;
        return clamp(yuv_to_rgb(y, chroma, camera_matrix, camera_range), 0.0, 1.0);
    }
    vec3 rgb = texture(camera_tex, uv).rgb;
    return camera_bgra ? rgb.bgr : rgb;
}

void vertex() {
    rest_pos = CUSTOM0.xyz;
    rest_normal = CUSTOM1.xyz;
    posed_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    posed_normal = normalize(MODEL_NORMAL_MATRIX * NORMAL);
    POSITION = PROJECTION_MATRIX * MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
    // Godot 4.7 uses reverse Z in both Compatibility and RenderingDevice.
    // A positive clip-Z offset is the conventional negative depth bias.
    POSITION.z += depth_bias * POSITION.w;
}

void fragment() {
    // Planar projection must never paint the back hemisphere, even when it
    // turns towards the camera. Posed facing alone cannot enforce this.
    if (dot(rest_normal, face_normal) <= 0.0) { discard; }
    vec2 canonical = vec2(dot(rest_pos - face_origin, face_right),
            dot(rest_pos - face_origin, face_up)) / face_scale;
    vec3 h = canonical_to_image * vec3(canonical, 1.0);
    if (h.z <= 0.0) { discard; }
    vec2 uv = h.xy / h.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { discard; }
    vec3 view = normalize(CAMERA_POSITION_WORLD - posed_pos);
    float facing = smoothstep(0.10, 0.35, dot(normalize(posed_normal), view));
    if (facing <= 0.0) { discard; }
    vec3 rgb = srgb_to_linear(sample_camera(uv));
    vec3 premultiplied = vec3(0.0);
    float alpha = 0.0;
    for (int i = 0; i < 3; i++) {
        if (i >= region_count) { break; }
        vec4 style = region_style[i];
        vec2 d = (canonical - region_rect[i].xy - region_anchor[i]) / (region_rect[i].zw * 0.5);
        float c = cos(style.x);
        float s = sin(style.x);
        d = mat2(vec2(c, -s), vec2(s, c)) * d;
        float sdf = length(d) - 1.0;
        if (style.z > 0.5) {
            vec2 q = abs(d) - vec2(1.0 - style.w);
            sdf = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - style.w;
        }
        float mask = 1.0 - smoothstep(-max(style.y, 0.00001), 0.0, sdf);
        float opacity = region_opacity[i];
        float a = mask * facing * facing * opacity;
        premultiplied = (rgb * colour_gain[i] + colour_lift[i]) * a + premultiplied * (1.0 - a);
        alpha = a + alpha * (1.0 - a);
    }
    if (alpha <= 0.0) { discard; }
    ALBEDO = premultiplied / alpha;
    ALPHA = alpha;
}
