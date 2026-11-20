# ADR-0004: Face decals rendered on the model's own surface in canonical face space

- **Status:** Proposed
- **Date:** 2026-11-20
- **Related:** [`../face-compositing.md`](../face-compositing.md)

## Context

The effect is webcam eyes and mouth attached to a rigid model. Three approaches are possible, and
the source brief mentions two of them ("2D overlay" and "3D surface projection") without resolving
which to build.

What the effect actually requires:

1. the face must not slide relative to the model during head motion,
2. the face must foreshorten and be occluded exactly like the model's surface,
3. the sampling position must be computable per fragment from a small amount of per-frame state,
4. it must work on an ordinary static mesh, with no rig, no UV layout and no special geometry,
5. it must be tunable by a non-technical user in an editor.

## Decision

Render the decal **on the model's own geometry**, in a second pass over the same mesh, sampling the
camera texture through a **canonical face space** shared by the user and the avatar:

- a per-user `FaceProfile` maps landmarks to canonical `(u,v)` (inter-ocular distance = 1),
- a per-avatar calibration maps canonical `(u,v)` onto the model's face surface,
- a per-frame homography `H: canonical → image` comes from the landmarks,
- the fragment shader composes `head-space → canonical → image` and samples the camera texture,
- sampling uses the camera frame whose timestamp matches the pose's timestamp.

Three quality modes (`flat`, `surface`, `shaded surface`) share the same math and differ only in the
geometry that carries the decal.

## Alternatives considered

| Option | Why not |
|---|---|
| **Flat quad overlay in front of the model** | Simplest, and shipped as the `flat` mode. Fails at yaw: no foreshortening, no self-occlusion, reads as a sticker. Not acceptable as the default. |
| **Projected decal volumes** (box + depth test) | Requires depth-texture reads per fragment, produces halos at depth-bias boundaries, and cannot handle the model occluding itself. Rendering the model's own surface is strictly better when the decal lives on that surface. |
| **UV-mapped face texture per model** (bake a face texture, animate it) | Requires per-model UV authoring, loses the real-pixel quality, and contradicts "any `.glb` works". |
| **Full 3D head reconstruction (3DDFA/mesh fitting)** | Higher fidelity in principle, but adds a second mesh to align with the model, plus a retopology problem. Enormous complexity for a decal. |
| **Screen-space overlay (2D only)** | Identical failure to the flat quad, worse: no relation to model rotation at all. |
| **Copying Snap's Face Inset model** | Not available as a format, and depending on a proprietary runtime defeats the purpose. |

## Consequences

**Positive**

- Occlusion and foreshortening are correct by construction — the decal is on the surface.
- No per-model UV work; only a 6-DOF face-plane calibration.
- One extra draw and a handful of ALU per fragment; no extra render targets, no depth reads.
- The same math serves all three modes, so quality is a setting, not a fork.
- Separating `FaceProfile` from avatar calibration makes both reusable.

**Negative**

- The second pass costs a full model draw if the user does not tag a face submesh (~0.7 ms at 20k
  tris, acceptable but not free).
- Planar projection is an approximation on strongly curved characters; a cylindrical mode is needed,
  which is extra editor surface area.
- Correctness depends on the timestamp pairing; without it, the best shader still slides.

**Neutral**

- The model's material does not affect the decal unless `shaded` mode is on, which is a deliberate
  simplification (the face is not a PBR material).

## Follow-ups

1. M1: validate planar projection on three archetypes; decide whether cylinder ships in 1.0 (Q2 in
   [`../roadmap.md`](../roadmap.md#consolidated-open-questions)).
2. M1: measure whether timestamp pairing removes sliding, with and without it, on recorded motion.
3. M2: confirm the calibration gizmo can express everything the shader needs, and nothing more.
4. If a future feature needs decals on multiple objects (reflections, props), revisit the
   projected-volume approach for that case only.
