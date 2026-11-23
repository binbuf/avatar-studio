# ADR-0037 — M1 golden matrix, decal-bbox guard, and the live soak

- **Status:** Accepted
- **Date:** 2026-11-22
- **Supersedes:** none
- **Related:** docs/design/testing-and-quality.md, docs/design/face-compositing.md

## Context

T20/T21 proved the decal pass with a small number of CPU-oracle goldens and a
mean-error threshold. That is enough for a component but not for the M1 claim:
the face reads as attached across the three avatar archetypes, three motions and
two lighting setups, and the app sustains 1080p60 with a measured latency. The
existing comparator (`tests/tools/golden_compare`) is a PPM tool with a global
SSIM and a global max delta. A global SSIM forgives a decal that moved by a few
pixels because the avatar covers most of the frame, and the app had no measured
capture-to-submit number.

## Decision

1. **One reviewed golden matrix** in `tests/golden/m1/forward_plus/` rendered by
   `app/tests/m1_golden.gd`: three avatar archetypes (flat, spherical,
   high-curvature) × three motions (still, yaw sweep, fast nod) × two lighting
   setups (key, key+fill), plus the asymmetric marker, four framing cases, four
   head-region cases (cut/node at ±30° yaw), one synthetic barrel-warp case, and
   three blank-state cases. 28 image baselines. Forward+ only on this host;
   Compatibility baselines are added only when a second renderer is gated.
2. **Comparison is SSIM ≥ 0.995 over 8×8 luma windows plus a max-delta guard
   inside the decal's own bounding box (≤ 8 per channel).** The bounding box is
   derived per case from the decal-on minus decal-off frame, so it moves with the
   scene instead of being a hand-maintained rectangle. The GDScript comparator
   mirrors `golden_compare` exactly; a change to one is a change to both.
3. **Baselines change only in a reviewed commit.** `--update-goldens` rewrites
   the files; the review looks at the actual/baseline/diff PNGs and the commit
   message explains the visual change. A failing run writes
   `.tools/T26-golden-artifacts/<case>.{actual,baseline,diff}.png`.
4. **The app owns one live pipeline adapter.** `avatar::live::LivePipeline`
   orchestrates camera → tracker → FaceRig → paired timestamp without Godot;
   `AvatarLivePipeline` binds it to the capture source, the C-ABI tracker, the
   T24 ring and the T25 `AvatarLiveSession`. The HUD's camera/face/tracker/
   confidence values and the four degraded states come from this pipeline, not
   from a placeholder.
5. **The soak measures the T05 shape.** `app/tests/m1_soak.gd` runs the real
   `main.tscn` at 1080p60 and writes `avatar_bench.result.v1` with frame time,
   interval, jitter, app-side latency, RSS, CPU, transport counters and pacing.
   `tests/gates/T27.ps1` re-applies the thresholds independently of the
   harness's own verdict. The interval budget is applied to the absolute
   deviation from the 16.667 ms cadence (the T16 definition), not to the raw
   interval, which can never be below its own nominal value.
6. **The app filters capture preferences to formats the ring can upload.**
   NV12 and BGRA/RGBA only. The platform preference order is unchanged, but a
   throughput step-down can no longer land on YUY2, which the T24 shader path
   deliberately does not decode. A camera that cannot sustain a mode steps down
   within the supported set and reports `CAP-003`.

## Consequences

- The golden suite is one 28-image Forward+ gate on this machine. It is not a
  cross-vendor gate; a second renderer or CI GPU needs its own baseline set.
- The decal-bbox guard catches the shifted/attached failures the global SSIM
  misses, at the cost of a per-case bounding-box derivation.
- The M1 output transport is still the T25 window monitor (30 Hz readback, read after the draw).
  App-side latency is honest for that transport but includes its cadence; T46
  replaces the readback with the render-cadence async path and keeps the same
  histogram and JSON shape.
- The measured camera on this host negotiates 640×480@30 NV12 after the
  1080p mode cannot sustain 30 fps. The soak's 1080p60 claim is about the output
  pipeline; camera input format is recorded in the JSON and is a hardware fact.
- The tracker C ABI has no statistics query, so the app's tracker counters are
  app-side submit/poll counts, not vendor inference timers.

