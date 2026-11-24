# ADR-0040: Automatic camera cutouts and the eye symmetry lock

- **Status:** Accepted
- **Date:** 2026-11-23
- **Supersedes:** -
- **Related:** [editor-ux.md](../editor-ux.md), [face-compositing.md](../face-compositing.md),
  [ADR-0039](0039-independent-source-and-target-regions.md), [ADR-0004](0004-face-decal-projection.md)

## Context

ADR-0039 split each eye/mouth region into a camera-side **source** rect and a model-side **target**
rect. The split made the two placements independent, but the source still defaulted to the target's
authored values, so the operator had to move and size three camera cutouts by hand even though the
tracker already reports accurate eye and mouth contours. Setting up a new subject was the slowest
part of the Setup window.

On the model side, placing two eyes meant repeating the same drag and size change twice. The
provisional defaults are symmetric, but any manual adjustment broke that symmetry and had to be
mirrored by hand.

## Decision

1. **Fit the source rects from the tracker.** A new `source_region_autofit.gd` turns the dense
   landmark contours (the 16-point eye lids and the outer lip contour, pinned to the 478-point
   MediaPipe topology) into a canonical-space box per region. The centre is the rig's **rest**
   centroid, not the instantaneous contour centre, because the decal adds the live anchor to the
   authored rect and using the moving centroid would count the gaze/lip offset twice.
2. **Size for a stable, undistorted window.** The width is the contour width plus a small margin;
   the height is the width divided by the target rect's aspect, so the source maps onto the target
   without anisotropic stretch. The width is settled over about 1.5 s and then frozen: eyes average
   the corner-to-corner distance (expression-independent), the mouth takes the smallest plausible
   width so a smile during settling does not inflate the window. A fixed window is what lets the
   smile and gaze move *inside* the patch; a window that resizes with the expression would paint a
   constant-size mouth.
3. **Manual edits win.** Moving, drawing or scaling a camera/mapping cutout turns the auto-fit off
   and keeps the operator's values. Switching the toggle back on re-measures. The fit never rewrites
   the persisted calibration on its own; only **Apply to model** saves.
4. **Eye lock on the model placement.** A toggle mirrors the two eyes about canonical `u = 0` and
   gives both the same size on drag, draw, resize and slider edits. Enabling it mirrors eye R from
   eye L immediately. It constrains only the eyes; the mouth stays independent.
5. **Editor preferences are not calibration.** Both toggles persist in `user://setup_prefs.json`,
   not in `live_calibration.json` or the `.avatar` package: they change how the operator edits, not
   what the decal consumes.

## Consequences

- A new subject needs no manual camera work: the cutouts land on the tracked eyes and mouth, and the
  operator only places the target rects on the model.
- The eye lock halves model-side placement work and guarantees a symmetric pair.
- The fitted mouth window is generous for an extreme smile (the still fixture is a wide smile); a
  resting mouth settles tighter. Re-toggling the auto-fit re-measures.
- The automatic fit is diagnostic-editor behaviour. The T34 gizmo and the package face calibration
  still own production placement; the fit is a starting point, not a substitute for the target.
- A partial fit is never applied: if any contour is missing or the homography is singular, the
  current cutouts are kept.
