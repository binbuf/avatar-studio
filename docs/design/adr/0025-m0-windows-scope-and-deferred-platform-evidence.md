# ADR-0025: Close M0 on Windows feasibility and defer cross-platform evidence

- **Status:** Accepted
- **Date:** 2026-11-21
- **Supersedes:** [ADR-0024](0024-m0-windows-evidence-and-open-gate.md) (gate-scope conclusion only; its backend retention decision stands)
- **Related:** [ADR-0003](0003-mediapipe-as-tracking-backend.md), [ADR-0017](0017-windows-first-mvp-sequencing.md), [ADR-0021](0021-real-mediapipe-windows-backend.md), [ADR-0023](0023-m0-headless-harness-and-thresholds.md), [roadmap.md](../roadmap.md), [tracking.md](../tracking.md), [implementation-map.md](../implementation-map.md)

## Context

ADR-0024 kept M0 open on three items after T14's local numeric pass: real-backend CI on all
three platforms, Reference/Minimum hardware measurements, and a human pose-stability judgment.
Those items cannot be closed on this machine or in this phase:

- The real macOS/Linux tracker backends do not exist yet. They are T52/T55, which
  [ADR-0017](0017-windows-first-mvp-sequencing.md) schedules after the Windows MVP.
- The hosted CI definition does not build the real tracker, and the last available run failed
  macOS/Linux. A green shell matrix would not demonstrate a real-backend build anyway.
- The available host (Ryzen 9 7900X3D, RTX 4080, 63 GiB) is above the documented Reference
  tier, so it cannot certify Reference or Minimum.

Keeping M0 open therefore deadlocks the entire vertical slice (T15-T27) against work that is
deliberately sequenced after the MVP. That is a plan conflict, not a tracker defect.

The Windows evidence is strong and internally consistent:

| Measurement | 192x192 | 256x256 |
|---|---|---|
| Inference p95 / p99 | 10.012 / 12.004 ms | 11.510 / 13.385 ms |
| Dropped frames, 900 s | 0 | 0 |
| RSS growth | 1.97% | 2.52% |
| Handles | 286 → 286 | 287 → 287 |
| Same-phase repeatability over 15 min | 0.0222° max | 0.0369° max |

The review package samples 152/240 valid frames at 192 and 154/240 at 256. Sampled frames show
landmarks on the eyes, glasses and mouth with no feature swap, and invalid results render an
explicit `INVALID / NO FACE - pose blank, not held` card rather than a frozen pose. The
37-38% no-face rate belongs to the short, unground-truthed source recording, not to a tracker
regression.

## Decision

Accept M0 for the Windows 11 MVP track and let rendering work begin. ADR-0003 and the pinned
MediaPipe Windows CPU backend stand; no ONNX pivot is triggered and no threshold is waived.

The three open items are re-homed as explicit acceptance criteria rather than dropped:

- **Cross-platform real-backend build and capture** become exit criteria of T52 (macOS) and
  T55 (Linux). A failure there blocks 1.0 parity, not the Windows MVP.
- **Reference/Minimum hardware measurement** becomes a T26 performance-evidence requirement and
  a T64 release-gate item. Until then, all tier statements are labelled local feasibility.
- **Pose stability** is accepted for M0 on measured repeatability plus sampled-frame review.
  Motion "swimming" is a rendered-composite artifact and is re-judged at T27, where rendered
  blind-review clips exist. M0 has no renderer, so judging it there is neither possible nor
  meaningful.

## Alternatives considered

| Option | Why not |
|---|---|
| Keep M0 open until T52/T55 land | Deadlocks T15-T27 against post-MVP work; the plan would stall for weeks on a port that is deliberately later |
| Port macOS/Linux trackers early | Reverses ADR-0017's Windows-first sequencing for no MVP benefit and adds unbounded platform build risk now |
| Accept silently through the harness note only | Leaves the milestone definition contradicting the accepted state and loses the decision history AGENTS.md requires |
| Record a threshold waiver | No threshold failed; writing a waiver would misstate the evidence |

## Consequences

- **Positive:** the vertical slice and editor work can proceed on the platform the MVP targets.
  The decision is recorded and reversible. The deferral is attached to the tasks that can
  actually satisfy it.
- **Negative:** M0 is now a Windows-scoped statement. A reader who assumes three-platform
  feasibility from the milestone name would be wrong, so the roadmap and implementation map
  must say so explicitly. If T52/T55 expose a tracker portability problem, 1.0 parity is
  blocked and may force a backend or build change late.
- **Neutral:** ADR-0024 remains as history; only its "keep the gate open" conclusion is
  superseded. The evidence and its limitations are unchanged.

## Follow-ups

- T15 must audit this scoping rather than treating the milestone as unconditionally passed.
- T52/T55 must add real-backend build and capture CI for their platforms and record the result.
- T26/T64 must record Reference/Minimum hardware evidence before any tier claim reaches release
  notes.
- T27 must judge motion swimming on rendered clips and may reopen tracking quality if it fails.
