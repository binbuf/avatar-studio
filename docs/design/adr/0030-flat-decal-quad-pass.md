# ADR-0030: Flat decal as a head-parented quad pass with clamped sampling

- **Status:** Accepted
- **Date:** 2026-11-22
- **Supersedes:** -
- **Related:** [`../face-compositing.md`](../face-compositing.md),
  [ADR-0004](0004-face-decal-projection.md), [ADR-0029](0029-shared-cpu-head-deformation.md)

## Context

[ADR-0004](0004-face-decal-projection.md) makes the model's own surface the quality target. The
flat mode still has to ship: it is the cheapest end-to-end path, the fallback for models whose face
cannot carry the surface pass, and a debugging view for calibration. It must use the same
canonical-face homography as the surface and shaded modes so a calibration cannot work in one mode
and fail in another.

T20 must prove the sampling contract before T21 adds the model surface: correct masks, linear
camera colour, clamped (never wrapped) sampling, and the output invariant that no camera pixel
reaches the frame outside a region mask. The CPU cut deformation from T19 deforms the model mesh,
so a quad pass cannot inherit the deformed surface; it must sit on the calibrated face plane and
follow the head as a rigid attachment.

## Decision

Implement flat mode as three quads, one per region, parented to the head and placed on the
calibrated face plane in head space. Each quad is sized to `region.size × face.scale`, rotated by
`rotation_deg`, and offset along the outward face normal by 1% of the face scale. The material is
unshaded, `blend_mix`, depth-test on, depth-write off, and draws after the opaque model.

The fragment shader projects the quad's **world** position onto the face plane using world-space
`face_origin`/`face_right`/`face_up`/`face_scale` uniforms, applies the feathered region mask
(ellipse or rounded rectangle) with `region_uv`, `region_anchor`, `region_rot`, `feather`,
`region_shape` and `corner_radius`, and samples the camera through `canonical_to_image`. It
discards outside the mask, when the homography's `w <= 0`, and when the image UV leaves `[0,1]`.
The camera sampler is `repeat_disable`; sampling is clamped by discard and never wraps. Camera
sRGB is converted to linear with the IEC curve before `ALBEDO = linear × colour_gain +
colour_lift`; `ALPHA = mask × facing² × opacity`.

The quad is only as large as its region. Anchor movement beyond the region rectangle clips until
the authoring/gizmo work (T35) sizes the rectangle; the anchor is bound but not silently expanded.
The CPU cut deformation does not move the quads; T24 must apply the same head rotation to the
decal anchor. Surface remains the default mode and the flat yaw limitation (~25°) is not hidden.

## Alternatives considered

| Option | Why not |
|---|---|
| Screen-space overlay | No relation to head space or the model; slides immediately and cannot use the canonical homography. |
| Wait for the surface pass only | Leaves no fallback/debug mode and delays validating the shared sampling contract. |
| Projected decal volume with depth reads | Rejected in ADR-0004: halos, depth-buffer reads, poor self-occlusion. |
| CPU-warped texture per frame | Copies pixels, breaks the "never copy the camera" rule, and adds latency. |
| Enlarge every quad by the full anchor envelope | Overlaps adjacent regions and hides authoring mistakes; the region rectangle should be authored for the anchor range. |

## Consequences

**Positive**

- Three draws, one shared shader, no render targets and no depth reads.
- The canonical homography, mask, linear colour and discard contract are validated before the
  surface pass depends on them.
- The black-camera diff makes the output invariant testable without a real camera.

**Negative**

- At yaw beyond roughly 25° the quad does not foreshorten like the model and reads as a sticker.
- The quad cannot be occluded by the model's own nose, glasses or hair.
- Flat mode has no matte and no lighting integration; those are T40 and T39.
- Anchor movement can clip at the region edge until T35 sizes the rectangle.

**Neutral**

- The quad is a rigid head attachment, so the CPU cut deformation is not inherited; T24 owns the
  rotation coupling.
- The shader is unshaded; the model's material does not affect the decal, deliberately.

## Follow-ups

1. T21: port the shared sampling/mask/colour code to the model surface and keep flat as the
   fallback; compare both on the same calibration.
2. T24: apply the head rotation to the flat decal anchor and timestamp-pair the homography.
3. T35: size region rectangles for the ±0.12 canonical anchor envelope.
4. T39/T40: add lighting influence and the matte without changing the mask/clamp contract.
5. T43: measure the flat yaw limit on the real-model corpus; keep surface as the default.
