# ADR-0039: Independent camera source and model target regions

- **Status:** Accepted
- **Date:** 2026-11-23
- **Supersedes:** -
- **Related:** [face-compositing.md](../face-compositing.md), [editor-ux.md](../editor-ux.md),
  [ADR-0004](0004-face-decal-projection.md), [ADR-0030](0030-flat-decal-quad-pass.md),
  [ADR-0031](0031-surface-decal-shared-pass.md)

## Context
T25c introduced one canonical region rect per eye/mouth and used it for both the decal mask and the
camera sample. That coupled two different operator intents: the camera pane had to show the exact
area sampled from the camera, while the model pane had to place the cutout on the avatar's face
plane. Moving one moved the other, and the operator could not use the tracker's accurate camera
cutout as the source while independently placing it on the model.

## Decision
1. **Two rects per region.** Each region carries a **source** rect (centre, size, rotation) and a
   **target** rect (centre, size, rotation). The target drives the mask; the source drives the
   sample. For a fragment at target-local coordinate `d`, the shader samples at
   `source_centre + anchor + R(source_rot) * (d * source_half)`.
2. **Independent editing.** The camera and mapping panes edit the source; the model pane edits the
   target. Selecting a cutout selects the region in all panes, but moving one rect never moves the
   other.
3. **The FaceRig anchor applies to both.** The live centroid delta still shifts the sample and the
   mask so gaze and lip motion follow. The clamp and filter are unchanged.
4. **Calibration version 2.** The persisted JSON adds `source_regions`. Version 1 files are accepted
   and read with source = target, preserving existing calibrations and the single-rect behaviour.
5. **Backward-compatible shaders.** When source equals target the flat and surface shaders reduce to
   sampling at the fragment's canonical point, which is the pre-split behaviour.

## Consequences
- The operator can take the tracker's accurate camera cutout and place it on the model without the
  two moving each other.
- The camera pane is WYSIWYG for the sampled area; the model pane is WYSIWYG for the painted area.
- The Setup control strip now has separate camera-cutout and model-placement slider groups, and the
  mapping pane is bound to the camera selection.
- The T34 gizmo and package face calibration must carry both rects; the T25c JSON remains a
  diagnostic editor for them.
- Auto-deriving the source rect size from the landmark extent is still open (tracking-quality work);
  the source defaults to the model placement and follows the tracker anchor today.
