# ADR-0009: Head-region rotation instead of whole-model rotation

- **Status:** Accepted
- **Date:** 2026-11-20
- **Related:** [`../face-compositing.md`](../face-compositing.md), [`../avatar-package-format.md`](../avatar-package-format.md), [`../editor-ux.md`](../editor-ux.md)

## Context

1.0 rotates the avatar with the head and does not translate it. The first version of this design
applied that rotation to the model root, which is correct for a floating head and wrong for anything
with shoulders: a bust whose shoulders swing with a nod reads as a puppet. The target input is a
downloaded, one-piece character, so requiring a rig or a split mesh would contradict the product's
"no special model" promise.

## Decision

Apply head rotation to a **head region**, selected in the editor and stored in the package as
`model.head_region`:

- `mode: "node"` — rotate a named node subtree. Clean, zero cost, used when the model already has a
  separate head.
- `mode: "cut"` — deform vertices above a plane at the neck, with a falloff band that blends rotation
  to zero below it. Works on a single mesh.

```
d   = dot(p_rest − cut.origin, cut.normal)
w   = smoothstep(0, cut.falloff, d)
R_w = slerp(identity, R_head, w)
p'  = pivot + R_w · (p_rest − pivot)
```

The decal pass carries the rest position as a varying and projects that, so the face is attached to
the undeformed surface and cannot slide relative to the head. The presentation transform places the
whole model; only the head region moves.

When the field is absent, the importer uses a single node named `head`, otherwise derives a cut
plane from the face surface and warns (`RIG-006`).

## Alternatives considered

| Option | Why not |
|---|---|
| **Rotate the model root** | Shoulders and torso swing; unusable for a bust with visible body |
| **Require a rig or a head bone** | Contradicts "no rig, bones, or blendshape requirement" and excludes most downloaded models |
| **Require a split head mesh** | Same problem; a user cannot split a mesh in the app, and asking them to open Blender contradicts the product |
| **Full vertex-weight painting** | More control, but a new editor mode and a new class of calibration failure for a ±30° rotation |
| **No head rotation at all** | Removes the defining motion |

T19 keeps this decision. The measured rendering mechanism and explicit CPU budget overrun are
recorded separately in [ADR-0029](0029-shared-cpu-head-deformation.md).

## Consequences

**Positive**

- A one-piece model works without a rig or an export round-trip.
- The body stays stable, which is what a stream overlay needs.
- The same mechanism handles both a clean node seam and a blended neck cut.

**Negative**

- A blended cut can stretch the neck at large angles; the falloff band needs tuning per model.
- The deformation is per-vertex, so the decal shader carries a rest-position varying rather than a
  single uniform matrix.
- A cut plane is a new calibration control and another thing auto-placement can get wrong.

**Neutral**

- The head node name is a convention, not a requirement; the editor can pick any node.

## Follow-ups

1. Test the neck cut on the real-model corpus at ±30° yaw; if it fails, require a node or add a split
   helper (Q16).
2. Validate that the rest-position interpolation does not produce a seam on low-poly necks.
3. Confirm the plane is always below the face origin, or refuse with `AVT-117`.
