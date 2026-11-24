# ADR-0041: Robust neutral measurement and yaw-tolerant feature validity

- **Status:** Accepted
- **Date:** 2026-11-23
- **Supersedes:** -
- **Related:** [editor-ux.md](../editor-ux.md), [face-compositing.md](../face-compositing.md),
  [tracking.md](../tracking.md), [ADR-0039](0039-independent-source-and-target-regions.md),
  [ADR-0040](0040-automatic-camera-cutout-fit-and-eye-lock.md)

## Context

ADR-0039 split each eye/mouth region into a camera-side source rect and a model-side target rect.
ADR-0040 added automatic source measurement and the eye lock. Hands-on review of that work found
four remaining quality problems:

1. **The first frame defined neutral.** An empty profile bootstrapped `CanonicalFaceFrame` from the
   first accepted frame, and the first solve defined the rest centroids. A first frame with a smile,
   a glance or a turned head baked that expression into every later anchor. The canonical frame also
   used the irises, which move with gaze.
2. **The mouth window was sized from a smile.** The contour width at a smile can be 50% wider than a
   resting mouth, so the source patch swallowed the cheeks and the painted mouth looked small.
3. **A profile view blanked the output.** `features_valid` required both eyes, both irises, the
   mouth and every one of the 478 landmarks to be visible. At yaw the far-side points drop below
   MediaPipe's visibility threshold, so the gate closed even though the mouth and near eye were
   good. The rig also rejected the whole frame when one region's centroid was unavailable.
4. **The face box and the model defaults were diagnostic-quality.** The box was the full landmark
   bounding box, which shrank at yaw and pumped with expression. The provisional face was 24% of the
   model height, the eye line was high, and the head cut sheared the avocado around its pit.

## Decision

1. **Neutral bootstrap window.** An empty profile buffers the best of up to 12 frames / 500 ms. A
   frontal, relaxed frame (score below 8: yaw, half pitch/roll, a mouth-width penalty above 0.95
   inter-ocular, minus confidence) is used immediately; otherwise the least extreme frame is used
   when the window closes. The canonical frame uses the **eye-corner midpoints**, not the irises.
2. **Stable rest, expressive live anchors.** The rest centroid is the neutral bootstrap frame's
   eye-corner and mouth-corner midpoint. The live centroid still includes the iris and the two lip
   centres when available, so gaze and lip motion move the patch. The corners are required; the
   expression points are optional. A region whose centroid is unavailable holds its previous
   anchor; the other regions keep tracking.
3. **Measure the source window from stable geometry.** Eye centre = outer/inner corner midpoint;
   eye width = corner distance plus margin. Mouth centre = mouth-corner midpoint; mouth width =
   corner distance capped by `1.30 x outer nose width` (a resting-mouth proportion), plus margin.
   Height follows the target aspect. A sample only counts while the face is frontal and relaxed;
   the Setup window takes the median of 24-45 samples over about 1.5 s and holds it.
4. **Feature validity is mouth + at least one visible eye.** Visibility is checked per feature, not
   over all 478 points. A hand over the mouth, or both eyes hidden, still blanks the output; a
   profile view with the mouth and near eye stays live. The rig holds the hidden region.
5. **The face box is a smoothed, yaw-aware face box.** It is measured from the stable outline
   (forehead, chin, temples, jaw, cheeks, eye corners, mouth corners), widened by `1/cos(yaw)`
   (clamped), and low-pass filtered. A large jump resets it.
6. **Model-aware best defaults.** The provisional face is 26% of the model height with the eye line
   30% down the model, the mouth below it, and a head cut low enough that the avocado turns rigidly.
   **Best defaults** restores this plus the canonical camera windows, re-arms the auto-fit and does
   not save; **Revert** restores the last applied/loaded mapping. Both remain operator-overridable.

## Consequences

- A new subject no longer needs a neutral first frame; the fit waits for a good sample and reports
  why it is waiting. A user who never faces the camera still gets the least-extreme frame after
  500 ms, with a visible warning.
- A profile view keeps the avatar visible instead of blanking. The far eye may be predicted by the
  tracker or held at its rest anchor; this is preferable to dropping the whole frame for a stylised
  avatar, and the mouth gate still catches a covered mouth.
- The mouth window is now expression-independent enough for a smile not to inflate it. A resting
  face still produces a slightly tighter window.
- The model defaults are provisional. T32/T34 still own the real auto-placement and gizmo; this ADR
  makes the test avocado presentable, not a substitute for per-avatar calibration.
