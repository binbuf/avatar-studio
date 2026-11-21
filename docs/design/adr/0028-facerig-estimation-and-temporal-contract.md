# ADR-0028: FaceRig estimation and temporal contract

- **Status:** Accepted
- **Date:** 2026-11-21
- **Supersedes:** —
- **Related:** [face-compositing.md](../face-compositing.md#implemented-core-math-t18),
  [tracking.md](../tracking.md#filtering-and-motion),
  [ADR-0008](0008-eigen-for-rig-math.md)

## Context

T18 needs headless, deterministic math before any decal shader or engine binding.
The tracker supplies camera-from-head pose and named image landmarks. Predicting
image sampling with the head pose would sample a different instant of the camera
texture. A failed solve or repeated invalid timestamp must not keep a frozen face
valid indefinitely. The local CPU budget is 0.1 ms per update, including outliers.

## Decision

`avatar::rig::FaceRig` owns an explicit named profile, or bootstraps one neutral
profile through T12's canonical frame. Stable correspondences exclude irises and
lip centres. Hartley-normalised DLT uses double-precision Eigen SVD for the final
fit, followed by up to three Gauss–Newton steps. Deterministic four-point RANSAC
uses a 0.008 normalised-image distance threshold, at least eight inliers and 60%
consensus, with adaptive 99.9% sampling target capped at 48 hypotheses. Minimal
hypotheses solve the normalised DLT equations in the h33=1 gauge with pivoted LU;
singular samples are discarded. The accepted fit always uses SVD.

Pose uses unwrapped Z/Y/X Euler One Euro channels, translation One Euro channels,
low-speed deadband, and at most 50 ms prediction. Public output is plain float
arrays: `head_from_camera` is the inverse filtered/predicted tracker pose; H still
samples the accepted source image. Region anchors are absolute canonical centres.
No Eigen type, clock read, engine object, GPU operation or I/O crosses this API.

Only a successful complete sample advances the 150 ms loss deadline. Duplicate,
older, rejected, invalid and failed-solve inputs cannot refresh it. Failed solves
halve confidence and use bounded damped H extrapolation. Loss never resets or
decays the pose to neutral. Filter state is retained across reacquisition; filter
time steps are capped at 50 ms to avoid a snap after a long gap. The renderer must
respect `features_valid`; transport blanking remains T23/T24 work.

## Consequences

Synthetic ground truth exercises estimation separately from intentional filter
lag. Normal and outlier benchmarks are local hardware gates, not Reference-tier
certification. The core tolerates a short missing-feature interval under the same
150 ms hold as a dropped frame; persistent loss always blanks. Explicit reset or
configure is required after timestamp epoch changes. Eigen 3.4.0 is SHA256-pinned,
private to `native/rig`, unmodified, and built with `EIGEN_MPL2_ONLY`; bootstrap and
the licence notice cover clean checkouts. Euler singularities near ±90° yaw remain
outside the frontal-face operating range; real-corpus and rendered quality gates
remain T26/T27 work.
