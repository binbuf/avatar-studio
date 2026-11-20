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
| **Image** | normalised `[0,1]²` | Camera frame, origin top-left, +v down. Assumed undistorted in 1.0 (lens distortion is accepted, see [below](#lens-distortion)); no mirroring applied. |
| **Camera** | metres | Right-handed, −Z forward, matching the tracker's pose output. |
| **Head** | metres | Rest model space: the avatar before the head rotation is applied. The rotation centre is `model.pivot`, or derived from the face plane. Calibration and the decal live here, so a turning head cannot slide the face. |
| **Canonical face** | unitless `(u,v)` | The face plane. Origin at the midpoint between the eye centres. `+u` toward the **subject's right eye**, `+v` toward the forehead. **Inter-ocular distance = 1.0.** `z` is the plane normal, unused for planar projection. |

`_l` and `_r` are always the **subject's** left and right, never the viewer's. In an unmirrored
camera image the subject's right eye appears on the viewer's left, so canonical `+u` maps to the
image's left side. This is the convention used by `LandmarkMap`
([`tracking.md`](tracking.md#landmark-map)), the region ids, and the `FaceProfile`.

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

`FaceProfile` is specified in [`user-data.md`](user-data.md#faceprofile); the avatar side is in
[`avatar-package-format.md`](avatar-package-format.md). Separating them is what lets a user calibrate
once and then try twenty avatars without redoing it, and lets an avatar be shared between users.

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

## Head motion and placement

### Rotation scope

1.0 is **rotation-only**, and the rotation applies to the **head region**, not the whole avatar. The
head turns; the shoulders and torso stay put. A bust whose shoulders swing with a nod reads as a
puppet, and a fixed body keeps the overlay stable when the user leans
([ADR-0009](adr/0009-head-region-rotation.md)).

The head region is authored per avatar (`model.head_region`) and picked in the editor:

| Mode | When | How it deforms |
|---|---|---|
| `node` | The GLB already has a separate head node or subtree | The node is rotated; the seam is the model's own |
| `cut` | A single mesh, the normal case for a downloaded character | Vertices above a neck plane rotate; a falloff band blends to zero below it |

For `cut`, the weight of a rest-space vertex is:

```
d    = dot(p_rest − cut.origin, cut.normal)
w    = smoothstep(0, cut.falloff, d)
R_w  = slerp(identity, R_head, w)
p'   = pivot + R_w · (p_rest − pivot)
```

`cut.origin` and `cut.normal` are in rest model space. `cut.normal` points toward the top of the
head and the plane sits at the neck, so the head, ears and hair rotate and the neck blends. The pivot
is `model.pivot`, or the derived rotation centre; a `node` rotates about its own origin. Vertices
below the band are unchanged.

Both the model pass and the decal pass evaluate the same deformation. The decal pass carries the
**rest position** as a varying, so the fragment shader projects the undeformed surface point and the
face cannot slide relative to the head. Normals are rotated by `R_w` and renormalised.

If `model.head_region` is absent, the importer uses a node named `head` when exactly one exists;
otherwise it derives a cut plane from the face surface and raises `RIG-006` so the user confirms it.
An invalid region (cut plane above the face origin, or a node that is not an ancestor of the face) is
a validation error, `AVT-117`.

### Presentation

The avatar as a whole is placed by a fixed transform. Head rotation is applied first, in rest model
space, so the body never moves:

```
view_from_model = T(offset) · R_y(180°) · T(−c)
```

- `c` is the centre of the fitted volume in rest model space. The fit uses the model AABB expanded by
  the head's swept range (±45° about the pivot), so the head cannot clip or separate at the rotation
  limits.
- The 180° yaw exists because the model's authored forward is −Z while the viewer camera looks down
  −Z from +Z. The model faces the camera after the rotation; the head pose is applied in head space
  before it. The rotation is not a mirror: the model's right side stays on the viewer's left, matching
  an unmirrored camera image.
- `offset` is the user's framing nudge, in fractions of the visible half-extent.

Fit is chosen per avatar:

| `fit` | Meaning |
|---|---|
| `model` | Fit the whole model's AABB. The default for head-shaped avatars. |
| `head` | Fit a sphere at the face plane origin with radius `1.5 × inter-ocular distance`. Useful for full-body models where the body should crop. |

The camera is perspective, 35° vertical FOV, `KEEP_HEIGHT`. With `r` the fitted bounding-sphere
radius and `θ` the smaller of the vertical and horizontal half-FOV:

```
D = (r / sin θ) · (1 + margin) / zoom
```

The camera sits at `(0, 0, D)` looking at the origin; `margin` defaults to 0.10 and `zoom` to 1.0.
The formula is a fit, not a guarantee: a very asymmetric model may still clip, and the editor shows
the bounds while framing.

**Mirroring.** The camera image is never mirrored for sampling. The preview is mirrored by default
(selfie view) as a display-only transform. The output is unmirrored by default; `mirror_output`
flips the final composited frame, including props, and the UI warns that asymmetric props and text
will flip. Flipping after compositing keeps the decal attached.

**Deferred:** a translation gain (0 in 1.0) that would let leaning move the avatar slightly. It needs
a clamp, a follow-camera or wider framing, and its own product decision. It is not in 1.0.

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
p_rest   = v_rest_pos                                // rest model space, from the vertex pass
(u, v)   = face_surface_project(p_rest)              // rest space → canonical face plane
mask     = region_mask(u, v, anchor)                 // feathered ellipse/rounded rect
img_uv   = H · (u, v, 1)                             // canonical → camera image
rgb      = sample_camera_linear(img_uv)              // YUV→RGB, sRGB→linear
facing   = smoothstep(0.10, 0.35, dot(N, V))         // fade at grazing angles
ALBEDO   = rgb * colour_gain + colour_lift
ALPHA    = mask * facing * opacity * matte
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
uniform sampler2D matte_tex;         // chroma or segmentation matte, 1x or 2x downscaled
uniform mat3  canonical_to_image;
uniform vec4  region_uv;        // centre (x, y) + full size (w, h) in canonical space
uniform vec2  region_anchor;    // filtered centroid offset
uniform float region_rot;       // radians, from rotation_deg
uniform float feather;
uniform vec3  colour_gain;
uniform vec3  colour_lift;
uniform float lighting_influence;

varying vec3 rest_pos;          // undeformed model-space position, for sampling
varying vec3 rest_normal;
varying vec3 posed_pos;         // after head deformation, for facing
varying vec3 posed_normal;

void fragment() {
    vec2  uv     = face_surface_project(rest_pos);

    vec2  d      = (uv - (region_uv.xy + region_anchor)) / (region_uv.zw * 0.5);
    d = mat2(cos(region_rot), -sin(region_rot),
             sin(region_rot),  cos(region_rot)) * d;
    float mask   = 1.0 - smoothstep(1.0 - feather, 1.0, length(d));
    if (mask <= 0.0) discard;

    vec3  h    = canonical_to_image * vec3(uv, 1.0);
    vec2  iuv  = h.xy / h.z;
    if (any(lessThan(iuv, vec2(0.0))) || any(greaterThan(iuv, vec2(1.0)))) discard;

    vec3  cam  = srgb_to_linear(texture(camera_tex, iuv).rgb);
    vec3  view = normalize(CAMERA_POSITION_WORLD - posed_pos);
    float facing = smoothstep(0.10, 0.35, dot(normalize(posed_normal), view));
    float m    = texture(matte_tex, iuv).r;

    ALBEDO = cam * colour_gain + colour_lift;
    ALPHA  = mask * facing * facing * m;
}
```

`ALPHA` is squared on `facing` so the silhouette fades faster than the interior; a linear fade
leaves a visible rim.

---

## Face matte

The output never contains a camera passthrough. The only camera pixels that reach the output are the
ones the decal samples, inside the region masks. A matte still matters: a region edge that overlaps
hair, a wall, or a window would otherwise carry that background into the avatar's face.

Two mattes, selected automatically:

| Mode | Input | Use |
|---|---|---|
| `chroma` | A green or blue screen | Best quality; soft key with spill suppression |
| `software` | Any background | Best effort; low-resolution person/face segmentation, temporally smoothed |

Detection is cheap and runs about once a second:

1. Sample the frame border and the area outside the face bounding box.
2. If one hue (green or blue) dominates a configurable share of those pixels, classify the frame as a
   chroma set; the median colour becomes the key.
3. Otherwise use `software`.

The mode is `auto` by default, with `chroma`, `software` and `off` available in the Show. A manual key
colour overrides detection; the editor has a *Test matte* button that shows the result.

The software matte is a segmentation mask from the tracker's runtime (MediaPipe Selfie Segmentation,
or an equivalent provider behind `ISegmentationProvider`), computed at 256×256 and upscaled with a
luma-guided filter so the region edge does not halo. It uses the same frame as the landmarks and is
temporally smoothed with the same One Euro philosophy: fast enough not to lag a blink, slow enough
not to flicker on hair.

`matte` multiplies the decal alpha. It never changes the output background and is never applied
outside the region mask. If segmentation is unavailable, the region mask alone is used and `TRK-009`
is logged once.

---

## Output contract: the avatar or nothing

The output stream has exactly two visible states ([ADR-0010](adr/0010-output-is-avatar-or-nothing.md)):

| State | Frame | When |
|---|---|---|
| **Avatar** | The rendered model with the decal | Tracking is valid, and both eyes and the mouth are present |
| **Blank** | Transparent for OBS/Spout; the configured background colour for opaque outputs | No face, missing eyes or mouth, or tracking lost beyond the hold window |

There is no passthrough, no last-frame freeze, and no fallback to the camera. A held frame only
bridges a dropped frame or a single blink, and never for longer than 150 ms.

This is deliberate. A stream that suddenly shows a frozen face, or the real camera, is worse than a
stream that momentarily shows nothing, and the performer can see the state in the preview. The
preview may show a "no face" overlay and, in calibration only, an opt-in camera feed; neither exists
in the output path.

The gate is enforced in the output manager, not in a shader: a `blank` state submits an empty frame
with the correct alpha or background, so every transport behaves the same. The cause code (`TRK-002`
for a lost face, `TRK-008` for missing features) is emitted on the transition, once, and `OUT-007`
records the gate itself.

---

## Lens distortion

1.0 does not undistort. The homography is estimated from, and samples, the same camera frame, so
landmarks and pixels share the same distortion; the face patch is smoothly warped rather than
misaligned. At typical webcam fields of view and a centred face the error is small. At wide FOV or
close range the face can stretch slightly toward the edges.

No camera intrinsics are used in 1.0. The assumption is recorded in the modelling guide and tested
with a synthetic barrel-distorted source. Optional per-camera radial calibration is `Deferred`; it
re-enters only if M2 quality tests show a visible error.

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

The key light comes from the avatar's `appearance.environment` preset plus the `appearance.key_light`
overrides ([`avatar-package-format.md`](avatar-package-format.md#appearance-and-lighting)). The decal
uses the same direction and intensity as the model, so moving the virtual light moves both. Material
overrides do not apply to the decal; its colour comes from the camera and the per-region transform.

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
| Jaw opens but chin does not | Rigid model, real mouth | Authoring guidance: neutral-open mouth or soft chin; `shaded` mode hides most of it |
| Face sharp, model soft (or vice versa) | Resolution/mip mismatch | Sample with the camera texture's native mip; optional slight blur on the decal |
| Face patch stretches at close range | Lens distortion not modelled | Accepted in 1.0; keep the face centred; optional calibration is `Deferred` |
| Avatar clips at the frame edge | Fit or zoom too tight | Increase margin or decrease zoom; the editor shows the bounds |
| Output is unexpectedly mirrored | `mirror_output` is on | Preview mirroring is display-only; check Settings → Output |
| Shoulders rotate with the head | Head region missing, or `node` mode selected the root | Set a neck cut, or pick the head node |
| Neck stretches or gaps at large yaw | Falloff too tight or the cut plane too high | Widen the falloff, lower the plane, or use a head node |
| Output blanks on a blink | Feature gate too strict, or the hold too short | Widen the hold or add per-feature hysteresis |
| Green tint on the face edge | Spill suppression too weak | Raise spill suppression, or key a cleaner screen |
| Face edge carries background | Matte off, or the region is larger than the face | Enable `auto` matte; tighten the feather |

---

## Model compatibility and authoring guidance

The model needs no rig, UVs, blendshapes or naming convention; the full list is in
[`avatar-package-format.md`](avatar-package-format.md#model-requirements). What it does need is a
face region the surface mapping can follow. The bundled guide says:

- **Required:** keep the eye/mouth area within roughly ±30° of a plane, or use a cylinder for a
  rounder head.
- **Recommended:** leave the face smooth. A face without modelled eyes or a mouth gives the cleanest
  result, but a painted face is supported as long as its features fit inside a region.
- **Recommended:** matte, non-metallic material on the face area.
- **Recommended:** a head-and-shoulders model, or one whose head is a separate node. For a one-piece
  mesh the editor cuts at the neck and blends; a clean neck seam gives the best result. A full-body
  model still crops with `fit = "head"`.
- **Recommended:** face the model toward −Z and keep the face near the frame centre.
- One mesh per character is fine; a separate face submesh is an optimisation, not a requirement.

Existing downloaded models are the normal case, not the exception. A rigged character in a T-pose is
rendered in bind pose with its animation ignored; a painted anime face works if the regions cover its
eyes and mouth. The editor checks what it can (head region, rotation centre, non-−Z forward, extreme
face curvature, no face-like region) and warns rather than blocks.

---

## Performance

| Item | Budget at 1080p |
|---|---|
| Extra draw (pass B, whole model, 20k tris) | < 0.7 ms |
| Fragment cost (3 regions, ~8% of screen) | < 0.5 ms |
| Head-region deformation (per-vertex blend, 20k tris) | < 0.15 ms |
| Face matte (256² segmentation + luma-guided upsample) | 1–2 ms |
| Uniform updates per frame | negligible (one mat3, a quaternion, a few floats) |
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
| Does the neck-cut deformation hold at ±30° on one-piece models? | Determines whether a head node becomes effectively required | Real-model corpus in M1 (Q16) |
| Is the software matte good enough without a green screen? | Determines whether a green screen is a soft requirement | M2 blind review at the region edge (Q17) |
| Can the decal pass reuse the model's depth without a second draw (e.g. stencil tag in pass A)? | 0.7 ms at 1080p on low-end GPUs | Profile in M3; only pursue if the budget is tight |
| Does lens distortion visibly stretch the face at typical webcam distance? | Determines whether optional calibration is worth the setup step | M2 quality tests with a barrel-distorted synthetic source |
| Is `fit = "model"` sufficient, or do users need per-avatar framing presets? | Full-body models may need `fit = "head"` plus offsets | M2 usability run with three model archetypes |
