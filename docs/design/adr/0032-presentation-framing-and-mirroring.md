# ADR-0032: Fixed presentation, swept fit, and display-only preview mirroring

- **Status:** Accepted
- **Date:** 2026-11-22
- **Related:** [face compositing](../face-compositing.md), [editor UX](../editor-ux.md),
  [ADR-0009](0009-head-region-rotation.md), [ADR-0026](0026-fixed-60hz-pacing-and-one-viewport-preview.md)

## Context

T22 must place the whole avatar with a fixed transform, keep it inside the viewport while the head
sweeps ±45°, expose framing evidence to the editor, and mirror the preview without corrupting the
camera sampling or the output. The model faces −Z, the camera looks down −Z from +Z, and the head
rotates in rest model space. A fit that uses only the model AABB can clip the head at the rotation
limits; a fit that rotates the whole model AABB is needlessly conservative and shrinks a
head-and-shoulders avatar. Mirroring the camera image would move the decal relative to the face.

## Decision

`native/rig/` implements framework-free framing math. The presentation is
`view_from_model = T(offset) · R_y(180°) · T(−c)`, with `offset` in fractions of the visible
half-extent at the fitted plane (positive x to the viewer's right). The 180° yaw is not a mirror:
model +X maps to world −X.

The fitted volume for `fit = "model"` is the model AABB unioned with a sphere at the head pivot whose
radius is the farthest corner of the head AABB. The head AABB is the selected node subtree in `node`
mode or the rest AABB of vertices with non-zero cut weight in `cut` mode; when absent, the model
AABB is the conservative fallback. A sphere contains every rotation of those corners, so the
documented ±45° sweep is contained for any axis combination. `c` and `r` are the swept AABB centre
and bounding-sphere radius. `fit = "head"` uses the face-plane origin and `r = 1.5 × inter-ocular
distance`; the body may crop.

The camera is perspective, 35° vertical FOV, `KEEP_HEIGHT`, at `(0, 0, D)` looking at the origin:
`D = (r / sin θ) · (1 + margin) / zoom`, with `θ` the smaller vertical/horizontal half-FOV. Zoom is
clamped to 0.5–2.0 and margin to 0–0.5. Containment is guaranteed for `zoom ≤ 1` and any margin;
`zoom > 1` is a deliberate crop, not a fit failure.

`AvatarRuntime.get_bounds_overlay()` returns the rest/swept AABBs, fitted sphere, six world-space
frustum planes, eight frustum corners, head sweep and containment flags as data only. It never adds
an overlay node to the output `SubViewport`; T36 draws it in the editor.

Preview mirroring (`mirror_preview`, default true) flips the preview `TextureRect` only. Output
mirroring (`mirror_output`, default false) is a final-frame RGBA8 flip after compositing, so the
decal and props flip with the model. The camera image is never mirrored for sampling.

## Alternatives considered

| Option | Why not |
|---|---|
| Rotate the whole model AABB by ±45° | Over-conservative: shoulders and torso inflate the fit and the avatar becomes too small. |
| Sample a few rotated AABBs per axis | Not a guaranteed bound for combined yaw/pitch/roll; a diagonal rotation can exceed every sampled axis case. |
| Fit from the AABB's largest extent | Asymmetric models clip on the narrower axis; the bounding sphere is orientation-independent. |
| Make zoom > 1 still contain the avatar | Removes the zoom-in control's meaning; the editor needs a deliberate crop. |
| Mirror the camera texture or decal UVs | Sampling must use the unmirrored accepted frame; mirroring UVs moves the face against the model. |
| Render a second mirrored preview viewport | Violates one render, one readback; the preview is a copy of the same texture. |
| Draw the bounds overlay inside the avatar viewport | The overlay would enter the output stream; it must stay in the editor window. |

## Consequences

- Positive: the head cannot clip at the ±45° limits; containment is testable at zoom ≤ 1; the
  overlay is honest at any zoom; preview mirroring costs one `TextureRect` flip; output mirroring is
  a single post-composite pass that keeps the decal attached.
- Negative: the swept sphere is conservative, so a long head or a pivot far from the head slightly
  shrinks the model fit. A model whose geometry lies outside its reported AABB can still clip.
  Zoom > 1 crops by design and must be explained in the editor.
- Neutral: the framing result is CPU-only and applied once per framing change, not per frame; the
  head pose never changes the framing.

## Follow-ups

- T36 draws the overlay from `get_bounds_overlay()` in the editor window and adds the framing
  controls.
- T42 persists per-avatar framing and Show overrides; the package fields remain the source of truth.
- T24 wires the live head rotation and camera/decal pipeline; framing stays independent of pose.
- T43's real-model corpus should check whether the swept sphere is too conservative for long-haired
  or full-body models.
