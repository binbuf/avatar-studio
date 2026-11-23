// Shared NV12/BT.601/BT.709 decode (T24).
//
// Inputs are the normalised samples from the R8 luma texture and the RG8
// chroma texture. `matrix` is 0 for BT.601 and 1 for BT.709. `range` is 0 for
// limited (16..235 luma, 16..240 chroma) and 1 for full range. The result is
// sRGB-encoded RGB; the caller converts it to linear exactly once.
//
// The constants are the standard inverse matrices:
//   BT.601: R = Y + 1.402 Cr, G = Y - 0.344136 Cb - 0.714136 Cr, B = Y + 1.772 Cb
//   BT.709: R = Y + 1.5748 Cr, G = Y - 0.187324 Cb - 0.468124 Cr, B = Y + 1.8556 Cb

vec3 yuv_to_rgb(float y, vec2 uv, int matrix, int range) {
    float luma = y;
    vec2 chroma = uv;
    if (range == 0) {
        luma = (y - 16.0 / 255.0) * (255.0 / 219.0);
        chroma = (uv - vec2(128.0 / 255.0)) * (255.0 / 224.0);
    } else {
        chroma = uv - vec2(0.5);
    }
    if (matrix == 1) {
        return vec3(luma + 1.5748 * chroma.y,
                luma - 0.187324 * chroma.x - 0.468124 * chroma.y,
                luma + 1.8556 * chroma.x);
    }
    return vec3(luma + 1.402 * chroma.y,
            luma - 0.344136 * chroma.x - 0.714136 * chroma.y,
            luma + 1.772 * chroma.x);
}
