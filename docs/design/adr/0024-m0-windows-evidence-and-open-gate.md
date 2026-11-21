# ADR-0024: Retain the Windows tracker while the full M0 gate remains open

- **Status:** Accepted
- **Date:** 2026-11-21
- **Supersedes:** —
- **Related:** [ADR-0003](0003-mediapipe-as-tracking-backend.md), [ADR-0021](0021-real-mediapipe-windows-backend.md), [ADR-0023](0023-m0-headless-harness-and-thresholds.md), [roadmap.md](../roadmap.md), [tracking.md](../tracking.md)

## Context

T14's fresh real Windows replay soak measured inference p95 10.012 ms at 192x192 and
11.510 ms at 256x256, with zero drops, RSS growth 1.97% / 2.52% and constant handles
(286 / 287). The ADR-0023 budget is 900 measured seconds per size, 1800 total; this does not
test one DLL instance for 30 minutes. Matching replay phases changed at most 0.0222° / 0.0369°
between the first and last ten loops, a repeatability aid without a stationary-jitter verdict.
The Ryzen 9 7900X3D (12 cores/24 threads), RTX 4080 and 63.2 GiB machine exceeds
the documented Windows Reference (6-core CPU, RTX 3060 class, 16 GB). The harness's automatic
`reference` label is a threshold selection heuristic, not evidence on that slower machine.

The three-platform CI definition does not build the real tracker. Latest available hosted
run [44874769735](https://github.com/binbuf/avatar-studio/actions/runs/44874769735), at
`478c2e08a191781b94b2eff88a97a9d079c2c858`, passed Windows but failed macOS/Linux and
formatting; it predates the T14 checkout. Real macOS/Linux tracker implementations are still
planned. The short local replay is not a stationary/motion ground-truth corpus, and the
soak's sparsely sampled BMPs cannot demonstrate normal-speed pose stability.

## Decision

Retain ADR-0003 and the pinned MediaPipe Windows CPU backend. Apply the stricter 15 ms p95
limit to local measurements, but make no Reference/Minimum certification or inferred CPU
scaling claim. The numeric harness gate is one part of M0; a passing local script does not
close the milestone. Keep M0 open and rendering work gated until the platform-build and
human pose-review requirements are satisfied, or an explicit milestone replan is approved
and recorded. This is not a threshold waiver.

No measured greater-than-2x latency miss or demonstrated failure to reproduce the pinned
Windows backend build currently establishes the automatic ONNX pivot trigger. Missing
platform implementations and unrelated shell CI failures are unresolved evidence, not a
measured backend build failure. If the trigger is established, write a new superseding ADR
for ONNX Runtime plus a licensed landmark model with pose solved in FaceRig. Do not reuse
0012 (Godot bootstrap) or 0008 (Eigen), despite stale numbers in the original task/ADR text.

## Consequences

- Later work continues to target MediaPipe unless a superseding ADR is recorded; no ONNX
  replacement has been implemented or selected by this gate.
- T14's local decision note, JSON, repeatability analysis and contiguous pose-review video
  remain in the ignored progress/results workspace; this ADR preserves the decision boundary.
- Completion needs successful clean-checkout CI evidence for the required platforms and real
  backend, adequate hardware evidence, and a recorded human rest-jitter/motion/handedness
  judgment. T52/T55 remain the current owners of the missing platform implementations;
  their scheduling conflict with M0 needs an explicit replan before rendering proceeds.
