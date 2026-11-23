// T24 component probe: decodes an NV12 colour-bar image with the shared
// yuv_convert include. The probe is intentionally separate from the decal so
// the test measures the conversion itself, not region masks or homography.
shader_type canvas_item;

#include "res://shaders/yuv_convert.gdshaderinc"

uniform sampler2D y_tex : filter_nearest, repeat_disable;
uniform sampler2D uv_tex : filter_nearest, repeat_disable;
uniform int matrix = 0; // 0 = BT.601, 1 = BT.709
uniform int range = 0;  // 0 = limited, 1 = full

void fragment() {
    float y = texture(y_tex, UV).r;
    vec2 uv = texture(uv_tex, UV).rg;
    COLOR = vec4(clamp(yuv_to_rgb(y, uv, matrix, range), 0.0, 1.0), 1.0);
}
