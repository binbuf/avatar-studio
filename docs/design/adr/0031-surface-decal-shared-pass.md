# ADR-0031: One shared-geometry surface decal pass

- **Status:** Accepted
- **Date:** 2026-11-22
- **Related:** [face compositing](../face-compositing.md), [ADR-0004](0004-face-decal-projection.md),
  [ADR-0029](0029-shared-cpu-head-deformation.md)

## Context

Surface is the default projection mode. Flat quads cannot reproduce nose occlusion or surface
foreshortening. T19 already retains shared live ArrayMeshes and immutable rest positions. A
planar projection also reaches the rear hemisphere unless rest-space orientation rejects it;
posed facing alone permits rear painting after a half turn.

## Decision

`FaceDecalSurface` binds one material through `AvatarModel.create_decal_pass`. All three region
masks are evaluated in that draw, using fixed-size uniform arrays; overlapping regions compose
in input order. Authored `face_submesh` selects the mesh/node, otherwise all model meshes draw
again. Base and decal reference the same live geometry and transforms. Reserve CUSTOM0/1 for
immutable realised rest model position/normal; transform rest normals by inverse transpose at
import. Carry both to fragments. Reject rest normals facing away from the calibrated face normal,
project rest position onto the face plane, then apply the T20 mask/clamp/linear-camera contract,
posed-normal facing squared, opacity, gain and lift. The pass is unshaded, `blend_mix`, depth-test
on, depth-write off, without depth textures or projected volumes.

Retain the LEQUAL depth policy using Godot's default reversed-depth GEQUAL comparison. On pinned
Godot 4.7.2 **both Compatibility and Forward+ use reverse Z**: add `1e-6 * clip.w` to clip Z for
the conventional negative bias towards the camera. Do not offset the sampling position or mesh.
Render references are separate because Compatibility captures linear RGB while Forward+ captures
sRGB; the CPU oracle blends in linear space in both cases.

## Consequences

- One additional draw per selected mesh, independent of region count; no pose-time rest-attribute
  updates. CUSTOM1 adds 12 bytes per imported vertex. Existing T19 reference meshes must preserve
  both custom-attribute format flags.
- Forty-four independent CPU-reference goldens cover avocado, flat/spherical/sculpted archetypes,
  yaw ±30°, rear rejection, cut/node deformation and altered sampling in both real renderers.
- The 20k-triangle, 1080p Forward+ benchmark uses GPU viewport timestamps in six alternating
  on/off blocks, 30 warmup + 60 samples each. It records the signed median delta and a conservative
  p95-on minus p05-off spread; the latter is a noise check, not a confidence interval. Both must
  stay below 0.7 ms. Verified local measurement: 0.012 ms delta, 0.021 ms spread on RTX 4080;
  this is local feasibility evidence, not Minimum/Reference-tier certification.
- Planar projection remains the implemented calibration. Cylindrical calibration, tracker/frame
  timestamp pairing, package/editor wiring, shading and matte remain their later tasks. T21's
  synthetic fixtures do not replace real-model corpus review.
