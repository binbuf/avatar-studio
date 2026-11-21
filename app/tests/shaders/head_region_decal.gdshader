shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never;
#include "res://shaders/head_decal.gdshaderinc"
uniform vec3 face_origin = vec3(0.0, 1.5, -0.15);
uniform vec3 face_right = vec3(1.0, 0.0, 0.0);
uniform vec3 face_up = vec3(0.0, 1.0, 0.0);
uniform float face_scale = 0.3;
void vertex() {
    head_rest_position = CUSTOM0.xyz;
}
// Diagnostic rest-space target. T20 replaces only this projection/colour stage.
void fragment() {
    vec3 relative = (head_rest_position - face_origin) / face_scale;
    vec2 uv = vec2(dot(relative, face_right), dot(relative, face_up));
    if (abs(uv.x) > 0.5 || abs(uv.y) > 0.5) discard;
    ALBEDO = vec3(1.0, 0.1, 0.6);
    ALPHA = 0.85;
}
