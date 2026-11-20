# ADR-0010: The output is the avatar or nothing

- **Status:** Proposed
- **Date:** 2026-11-20
- **Related:** [`../face-compositing.md`](../face-compositing.md), [`../security-privacy.md`](../security-privacy.md), [`../output-and-virtual-camera.md`](../output-and-virtual-camera.md)

## Context

The output is a stream overlay or a virtual camera used in calls. Two failure modes are unacceptable:

1. **A camera passthrough.** If the compositor falls back to the raw camera, the real face appears on
   stream. Nothing in the product should make that possible, even after a bug.
2. **A frozen or neutral face.** Holding the last pose, or decaying to a neutral pose, shows a face
   that is not live. On a call or a stream that is worse than showing nothing, because it lies about
   the performer's state.

A third, smaller problem: the only camera pixels that legitimately reach the output are the decal's
eye and mouth samples. At a region edge, hair, a wall, or a window can be sampled along with the
face. A matte is needed even though the rest of the frame is synthetic.

## Decision

**The output has exactly two states: the rendered avatar, or a blank frame.**

- `Avatar` when tracking is valid and both eyes and the mouth are present.
- `Blank` otherwise. Blank means transparent for OBS/Spout and the configured background colour for
  opaque outputs.
- The last pose is held for at most 150 ms to bridge a dropped frame or a blink. Beyond that the
  frame is blank. There is no decay to neutral and no last-frame freeze.
- The gate lives in the output manager, so every transport gets the same state. A transport cannot
  reintroduce a frame.
- The preview may show a "no face" overlay and, in calibration only, an opt-in camera feed. Neither
  exists in the output path.

**A face matte gates the decal.** `auto` mode detects a green or blue screen from the frame border
and uses a soft chroma key with spill suppression; otherwise it uses a low-resolution software
segmentation mask, temporally smoothed. The matte multiplies the decal alpha only. If segmentation is
unavailable, the region mask is used and `TRK-009` is logged once.

## Alternatives considered

| Option | Why not |
|---|---|
| **Hold the last frame until tracking returns** | Shows a frozen face indefinitely; the common "camera froze" failure |
| **Decay to a neutral pose** | Still a face, still not live; it hides the failure from the performer |
| **Fall back to the camera feed** | Leaks the real face; the exact outcome this product exists to avoid |
| **Green screen only** | Most users do not have one; the software path is the default |
| **Software matte only** | A chroma key is cheaper and cleaner when a screen exists; ignoring it wastes the best case |
| **Blank the whole app, not the output** | The performer needs the preview to understand and recover |

## Consequences

**Positive**

- A hard, testable privacy and honesty invariant.
- One state machine for all transports.
- The best-case matte is used automatically, with a graceful fallback.

**Negative**

- A lost face is visible as a blank frame; the preview must explain it.
- The 150 ms hold and the feature gate need per-feature hysteresis to avoid flicker (Q18).
- Software segmentation adds a model and 1–2 ms; the fallback path must be maintained.

**Neutral**

- The matte affects only the decal, not the output background; the avatar's background remains
  synthetic.

## Follow-ups

1. Implement the no-passthrough test: diff output frames against a black camera and assert every
   changed pixel is inside a region mask.
2. Measure flicker on blinks and partial occlusion; tune the hold and hysteresis (Q18).
3. Measure the software matte at 1080p and decide whether the tracker runtime or a separate provider
   owns it (Q17).
