// Setup-window camera preview (T25c).
//
// This is a diagnostic view only. It samples the same T24 ring textures the
// decal samples, with the same NV12/BT.601/709 decode, so the operator sees the
// exact pixels the mapping math is using. It never enters the avatar viewport
// or the OBS output.
shader_type canvas_item;
render_mode unshaded;

#include "res://shaders/yuv_convert.gdshaderinc"

uniform sampler2D camera_tex : filter_linear, repeat_disable;
uniform sampler2D camera_y_tex : filter_linear, repeat_disable;
uniform sampler2D camera_uv_tex : filter_linear, repeat_disable;
uniform bool camera_is_yuv = false;
uniform int camera_matrix = 0; // 0 = BT.601, 1 = BT.709
uniform int camera_range = 0; // 0 = limited, 1 = full
uniform bool camera_bgra = false;
uniform bool has_frame = false;

void fragment() {
    if (!has_frame) {
        COLOR = vec4(0.055, 0.070, 0.095, 1.0);
    } else if (camera_is_yuv) {
        float y = texture(camera_y_tex, UV).r;
        vec2 chroma = texture(camera_uv_tex, UV).rg;
        vec3 rgb = clamp(yuv_to_rgb(y, chroma, camera_matrix, camera_range), 0.0, 1.0);
        COLOR = vec4(rgb, 1.0);
    } else {
        vec3 rgb = texture(camera_tex, UV).rgb;
        COLOR = vec4(camera_bgra ? rgb.bgr : rgb, 1.0);
    }
}
