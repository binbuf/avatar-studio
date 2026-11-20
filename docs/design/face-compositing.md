# Face Compositing

**Status:** proposal · **Last updated:** 2026-11-20
**Decision record:** [ADR-0004](adr/0004-face-decal-projection.md)

This document specifies how real webcam eyes and mouth are attached to a rigid model. It is the
part of the system that determines whether the product works. Everything else is plumbing.

---

## Goal, stated as a failure mode

A successful frame is one where a viewer cannot tell where the model ends and the human begins.
The three ways this fails, in order of how often they occur:

1. **The face slides.** The decal drifts across the model during head motion, or lags behind it.
   Cause: pose and pixels from different instants, or an unstable face frame.
2. **The face floats.** The decal reads as a sticker: flat, evenly lit, with a hard edge, ignoring
   the model's curvature and occlusion. Cause: 2D overlay rendering, no depth, no shading.
3. **The face is a different photograph.** Colour temperature, exposure or sharpness does not match
   the model, or the model's lighting does not affect the face. Cause: no colour management, no
   lighting integration.

The design below addresses each in that order. The math exists to make (1) impossible; the render
modes exist to make (2) cheap to avoid; the shader exists to make (3) tunable.

---

## Coordinate spaces

| Space | Units | Definition |
|---|---|---|
| **Image** | normalised `[0,1]²` | Camera frame, origin top-left, +v down. Undistorted; no mirroring applied. |
| **Camera** | metres | Right-handed, −Z forward, matching the tracker's pose output. |
| **Head** | metres | The avatar's root space after pose application. Origin at the model's pivot. |
| **Canonical face** | unitless `(u,v)` | The face plane. Origin at the midpoint between the eye centres. `+u` toward the user's right eye, `+v` toward the forehead. **Inter-ocular distance = 1.0.** `z` is the plane normal, unused for planar projection. |

Canonical face space is the contract between the user's face and the avatar's face. It is
deliberately not measured in millimetres: normalising by inter-ocular distance makes the mapping
scale-invariant, so a webcam at any distance and a model at any scale behave identically.

Two independent calibrations meet in this space:

```
        FaceProfile (per user, reusable)          AvatarFaceCalibration (per avatar)
        ───────────────────────────────          ──────────────────────────────────
        landmark ─▶ canonical (u,v)              canonical (u,v) ─▶ head space
        measured once, neutral pose              face plane placement, region shapes
```

Separating them is what lets a user calibrate once and then try twenty avatars without redoing it,
and lets an avatar be shared between users.

### Face frame estimation

Per frame, from `TrackingResult.landmarks`:

1. **Select stable correspondences.** Eye corners, nose bridge and tip, mouth corners, chin, temple
   and cheek points. Eyelid and inner-lip points are excluded: they move with expression and would
   make the frame breathe. Selection is expressed through a `LandmarkMap` (anatomical names →
   backend indices), never hard-coded indices, so a different tracker can be dropped in.
2. **Build correspondences** `(u_i, v_i) ↔ (x_i, y_i)` using the stored `FaceProfile`. If no profile
   exists yet (first run), it is created from the current frame and the frame is defined as neutral.
3. **Estimate the homography** `H : (u,v) → (x,y)` by normalised DLT over the correspondences, then
   refine with a few Gauss–Newton iterations on the reprojection error.
4. **Reject outliers** with RANSAC (threshold ~0.008 in normalised image units). Blinks, a hand
   near the face, or a mis-detected jaw all produce outliers; a single bad correspondence is enough
   to visibly warp the face.
5. **Temporal guard.** If the solve fails or the inlier count drops below threshold, reuse the
   previous `H` scaled by elapsed time (damped extrapolation) and lower confidence. Never emit a
   jump.

`H` is a 3×3 matrix. It is exact for a planar face and cheap to apply per fragment. It is also the
only thing the compositor needs in order to know where any canonical point appears in the image.

**Why not use the tracker's transformation matrix?** MediaPipe supplies a 4×4 metric pose, which is
the right source for *rotating the avatar* but a poor source for *sampling pixels*: it carries
absolute scale, depends on the tracker's canonical head model, and is noisier than the 2D fit
because it must resolve depth. The design uses the 3D pose for rotation and the 2D homography for
sampling. They are complementary, and both are available from the same result.

### Region anchors

Eye and mouth regions must follow the *content*, not just the head. When the user looks left, the
iris moves within the eye socket; if the crop window is fixed to the head, the iris slides to the
edge of the patch and the illusion breaks.

For each region (`eye_l`, `eye_r`, `mouth`):

- a **centroid** is computed from that region's landmarks (eye corners + iris centre for eyes;
  mouth corners + upper/lower lip centre for the mouth),
- the centroid is converted into canonical space through `H⁻¹`,
- the result is low-pass filtered with a slow time constant (~150 ms) and clamped to a small
  envelope around its rest position (±0.12 in canonical units).

The slow filter is deliberate: it tracks gaze and lip shape without inheriting landmark jitter, and
the clamp prevents a bad frame from throwing the patch across the face.

---

## Projection modes

Three modes, selectable per avatar, all sharing the same sampling math. They differ only in *what
geometry carries the decal*.

### Mode 1 — `flat`

A quad per region, parented to the head, offset slightly along the face normal. Drawn in the
transparent pass with depth test enabled but depth write disabled.

- Cost: three quads.
- Works: stylised, low-curvature models; near-frontal viewing.
- Fails: at yaw beyond ~25° the quad does not foreshorten like the model and reads as a sticker;
  the model cannot occlude it (a nose never covers the far eye).

### Mode 2 — `surface` (default)

The **head mesh is drawn a second time** with the decal shader. No extra geometry, no projected
boxes, no depth tricks:

- Pass A: the avatar renders normally (opaque).
- Pass B: the same mesh renders again with `depth_test = LEQUAL`, `depth_write = false`,
  `blend_mix`, and a fragment shader that discards any fragment outside the region mask.

Occlusion is then correct by construction: the decal only ever appears on the model's own surface,
so a turning nose occludes the far eye exactly as it occludes the model. Foreshortening is correct
because the geometry is the model's geometry.

`LEQUAL` (not `EQUAL`) is used because identical transforms can still produce a one-ULP depth
difference across drivers. A small negative depth bias is applied as belt and braces.

If the model is a single mesh, pass B costs a second draw of the whole model. On a 20k-triangle
avatar at 1080p that is well under a millisecond; if it ever matters, the editor lets the user tag
a face submesh and pass B is restricted to it.

### Mode 3 — `shaded surface`

Mode 2 plus lighting integration:

- the sampled face is multiplied by a cheap diffuse term from the scene's key light,
- specular response is suppressed (skin should not mirror the model's metal highlights),
- a per-region colour transform (gain/lift/saturation) is applied,
- edges fade with view angle and mask distance.

This is the quality target. It costs a handful of ALU operations per fragment and no extra passes.

### Rejected: projected-decal volumes

The textbook technique — draw a box, discard outside `[0,1]³` projector space, compare against the
depth buffer — was considered and rejected. It requires a depth texture read per fragment, produces
halos where the depth bias is wrong, and cannot handle the model occluding itself cleanly. Rendering
the model's own surface is strictly better when the decal lives on that surface. It remains the
right answer only if a future feature needs to project onto *multiple* objects at once (for example
a face reflected in a held prop), which is not planned.

---

## Sampling math

Per fragment of the decal pass:

```
p_head   = (head_from_world · p_world).xyz          // model surface point, head space
(u, v)   = face_surface_project(p_head)             // head space → canonical face plane
mask     = region_mask(u, v, anchor)                // feathered ellipse/rounded rect
img_uv   = H · (u, v, 1)                            // canonical → camera image
rgb      = sample_camera_linear(img_uv)             // YUV→RGB, sRGB→linear
facing   = smoothstep(0.10, 0.35, dot(N, V))        // fade at grazing angles
ALBEDO   = rgb * colour_gain + colour_lift
ALPHA    = mask * facing * opacity
```

Two details that matter:

**`face_surface_project` is not always planar.** A peanut or a sphere has significant curvature
across the eye line. The default is a plane (cheap, exact for flat faces), with a **cylindrical**
option for round characters: unwrap `(u,v)` from the cylinder axis. The editor's calibration gizmo
selects and positions it. This is a per-avatar property, stored in the package
([`avatar-package-format.md`](avatar-package-format.md#field-reference)).

**Sampling is clamped, never wrapped.** `img_uv` outside `[0,1]` means the face has left the camera
frame; the fragment is discarded rather than smeared.

### Shader sketch

```glsl
// app/shaders/face_decal.gdshader (spatial, unshaded, blend_mix)
uniform sampler2D camera_tex : filter_linear, repeat_disable;
uniform mat3  canonical_to_image;
uniform mat4  head_from_world;
uniform vec4  region_uv;        // centre (x, y) + full size (w, h) in canonical space
uniform vec2  region_anchor;    // filtered centroid offset
uniform float region_rot;       // radians, from rotation_deg
uniform float feather;
uniform vec3  colour_gain;
uniform vec3  colour_lift;
uniform float lighting_influence;

varying vec3 world_pos;
varying vec3 world_normal;

void fragment() {
    vec3  p_head = (head_from_world * vec4(world_pos, 1.0)).xyz;
    vec2  uv     = face_surface_project(p_head);

    vec2  d      = (uv - (region_uv.xy + region_anchor)) / (region_uv.zw * 0.5);
    d = mat2(cos(region_rot), -sin(region_rot),
             sin(region_rot),  cos(region_rot)) * d;
    float mask   = 1.0 - smoothstep(1.0 - feather, 1.0, length(d));
    if (mask <= 0.0) discard;

    vec3  h    = canonical_to_image * vec3(uv, 1.0);
    vec2  iuv  = h.xy / h.z;
    if (any(lessThan(iuv, vec2(0.0))) || any(greaterThan(iuv, vec2(1.0)))) discard;

    vec3  cam  = srgb_to_linear(texture(camera_tex, iuv).rgb);
    vec3  view = normalize(CAMERA_POSITION_WORLD - world_pos);
    float facing = smoothstep(0.10, 0.35, dot(normalize(world_normal), view));

    ALBEDO = cam * colour_gain + colour_lift;
    ALPHA  = mask * facing * facing;
}
```

`ALPHA` is squared on `facing` so the silhouette fades faster than the interior; a linear fade
leaves a visible rim.

---

## Colour and light matching

The face comes from a consumer camera with auto-exposure, auto-white-balance and unknown gamma; the
model is lit by the scene. Three mechanisms, in order of importance:

1. **Linear pipeline.** Camera sRGB → linear before any blending. Skipping this is the most common
   cause of a washed-out, pasted-on face.
2. **Per-region gain/lift.** Four sliders (exposure, temperature, saturation, contrast) with a
   one-click "match to model" that samples the model's albedo in a ring around the region and
   solves for a gain that equalises mean luminance. Deterministic, no ML.
3. **Lighting influence.** Multiply by a fraction of the scene's key-light diffuse term. Default
   0.35: enough that turning the model's virtual light visibly moves the face, low enough that a
   dark scene does not black out the eyes.

Auto-white-balance flicker is the sneaky failure: the camera changes colour mid-stream and the face
pops. Mitigation: smooth the gain over ~500 ms, and recommend locking camera exposure/WB in the
settings panel when the driver exposes it.

---

## Known artifacts and their causes

| Artifact | Cause | Mitigation |
|---|---|---|
| Face slides across the model | Pose and pixels from different frames | Timestamp-paired camera ring ([`architecture.md`](architecture.md#frame-lifecycle)) |
| Face lags head rotation | Filter latency too high | One Euro filter, tuned per-axis; predict to render time |
| Jitter at rest | Landmark noise | Deadband + adaptive smoothing; never smooth the camera pixels themselves |
| Sticker look at yaw | `flat` mode | Switch to `surface`; document in the model guide |
| Hard oval edge | Mask feather too tight | Feather 0.15–0.25 canonical units, plus view-angle fade |
| Black halo over gameplay | Alpha premultiplication mismatch | Per-transport alpha setting + checkerboard test pattern |
| Face too bright / wrong hue | sRGB blended as linear, or no gain match | Linear pipeline; gain calibration |
| Pupils stuck centre | Region anchored to head, not landmarks | Per-region centroid anchors |
| Eyes look dead | Tracking at 30 Hz while render at 60 Hz | Accept (matches real cameras) or prefer 720p60 capture |
| Jaw opens but chin does not | Rigid model, real mouth | Modelling guidance: neutral-open mouth or soft chin; `shaded` mode hides most of it |
| Face sharp, model soft (or vice versa) | Resolution/mip mismatch | Sample with the camera texture's native mip; optional slight blur on the decal |

---

## Modelling guidance (shipped as documentation)

Users will import arbitrary models, so the app must state what works. The bundled guide says:

- Leave the face area smooth; do not model eyes or a mouth.
- Keep the eye/mouth area within roughly ±30° of a plane (or use cylindrical unwrap).
- Avoid strong specular or metallic material on the face area.
- Put the pivot near the head's centre of rotation, not at the feet.
- Front of the model faces −Z.
- One mesh per character is fine; a separate face submesh is an optimisation, not a requirement.

The editor checks these where it can (pivot far from geometry, non-−Z forward, extreme face
curvature) and warns rather than blocks.

---

## Performance

| Item | Budget at 1080p |
|---|---|
| Extra draw (pass B, whole model, 20k tris) | < 0.7 ms |
| Fragment cost (3 regions, ~8% of screen) | < 0.5 ms |
| Uniform updates per frame | negligible (one mat3, one mat4, a few floats) |
| CPU work (homography + anchors) | < 0.1 ms |

The compositor never copies pixels. The camera texture is sampled directly by the GPU; only the
final composed frame is read back for output, once, regardless of how many destinations consume it.

---

## Open questions

| Question | Why it matters | Resolution path |
|---|---|---|
| Is a planar face frame sufficient for strongly spherical characters, or is cylinder unwrap required in practice? | Determines whether the extra mode ships in 1.0 | Build both in M1; compare on three model archetypes |
| How much does 30 Hz face content hurt next to 60 Hz head rotation? | May force 720p60 capture preference | A/B test in M2 with recorded clips |
| Does per-region gain calibration converge reliably across skin tones and lighting? | Quality equity; a mismatch is most visible on darker skin | Test matrix in M2 across lighting conditions |
| Can the decal pass reuse the model's depth without a second draw (e.g. stencil tag in pass A)? | 0.7 ms at 1080p on low-end GPUs | Profile in M3; only pursue if the budget is tight |
