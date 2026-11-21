# ADR-0014: Deterministic sources, ground truth, and the golden comparator

- **Status:** Accepted
- **Date:** 2026-11-20
- **Supersedes:** —
- **Related:** [`../testing-and-quality.md`](../testing-and-quality.md), [`../camera-capture.md`](../camera-capture.md), [`../tracking.md`](../tracking.md), [`../face-compositing.md`](../face-compositing.md)

## Context

T04 is what makes the pipeline testable without a camera, a person or a GPU-specific driver. The
frame ownership model, the shape of synthetic ground truth, the replay fixture format and the image
comparison metric all become contracts that later tasks build on, so they are expensive to change
after M0. The design already requires ring-owned frames, an injected `FrameClock`, a synthetic face
with exact pose, a scripted tracker and a perceptual golden comparator; T04 makes those choices
concrete.

## Decision

1. **Ring-owned frames with pin/release.** `FrameRing` owns a fixed number of slots (default 4).
   `ICameraSource::ring()` returns the producer's ring; a consumer holds a `FrameLease` while it
   reads and the producer never overwrites a pinned slot. `VideoFrame::planes` point into slot
   storage, so there is no implicit pixel copy. A producer with every slot pinned drops the frame and
   increments `dropped()`.
2. **Every source takes an injected `FrameClock`.** Test sources publish only when the clock has
   advanced; they never read a wall clock, allocate per frame, or use unseeded randomness. The
   ScriptedTracker's jitter uses a seeded LCG.
3. **Synthetic ground truth is canonical correspondences plus a pose.** `SyntheticFaceSource` renders
   a canonical face plane through the exact plane-induced homography for a scripted 6-DOF path. It
   emits a row-major `camera_from_head` 4x4 and, per canonical landmark, both the pinhole image point
   and the post-distortion point. A barrel variant applies a radial `k1` model to the render and to
   ground truth. This is precise enough for a test to solve a homography and compare numerically.
4. **Replay is Y4M plus a versioned JSON sidecar.** `ReplaySource` reads YUV4MPEG2 4:2:0/4:2:2/4:4:4,
   converts into the ring format, and takes `timestamp_ns` from the sidecar (arrival from the clock).
   The sidecar carries the pose and the same canonical correspondences.
5. **Golden comparison is PPM + SSIM + max delta.** `tests/tools/golden_compare` compares 8-bit RGB
   PPM (P6) images with global SSIM over 8x8 luma windows and a maximum per-channel delta guard. The
   CLI exits 0 only on pass, 1 on a threshold failure and 2 on usage/IO errors, and writes
   `<name>.actual.ppm`, `<name>.baseline.ppm` and `<name>.diff.ppm` when asked.
6. **Small fixtures are tracked; large clips are generated.** A 64x48, three-frame Y4M and its JSON
   sidecar are checked in for the replay test. `scripts/gen_fixtures.ps1 -Large` writes larger clips
   to the gitignored `tests/fixtures/generated/`.

## Alternatives considered

| Option | Why not |
|---|---|
| Producer copies into a consumer-owned buffer | Adds a copy on the capture path and makes lifetime depend on the consumer; the ring-owned model bounds memory and keeps the contract local. |
| Seqlock or a mutex around frame slots | A seqlock permits a torn read in the C++ memory model; a mutex is forbidden on the data plane. Pin counts plus a state CAS are lock-free and testable. |
| Let sources read `steady_clock` | Breaks determinism and makes frame hashes and timestamps machine-dependent. |
| Render the synthetic face by forward-warping a bitmap | Sampling holes and aliasing make the ground truth approximate. Inverse-mapping through the analytic homography is exact by construction. |
| PNG baselines via stb_image | Adds an unpinned dependency for a tool that only needs RGB pixels. PPM is trivial, lossless and diffable; T26 can add a converter if CI needs viewable artifacts. |
| Exact pixel equality | GPU rasterisation differs across vendors; the design already specifies SSIM plus a delta guard. |
| Per-region max-delta in T04 | The decal bounding box belongs to the rendered avatar; T26 owns that region mask. T04 provides the global guard and the artifact plumbing. |

## Consequences

**Positive**

- Component tests run in under a second on CPU and can assert pose, homography, marker position,
  timestamps and frame hashes numerically.
- The capture backend (T10), tracker backend (T11) and rig (T18) get a stable frame and ground-truth
  contract before they are written.
- Golden failures produce three inspectable artifacts from both CTest and a manual command.

**Negative**

- PPM artifacts are larger than PNG and not viewable in a browser without conversion.
- The global SSIM/max-delta guard is weaker than a region-aware guard; T26 must add the decal box.
- `SyntheticFaceSource` renders on the CPU, so 1080p frames are only suitable for component tests,
  not a performance benchmark.

**Neutral**

- The sidecar schema is versioned and additive; unknown landmark names are ignored, matching the
  runtime `LandmarkMap` rule.
- The `ICameraSource` in T04 is the frame-producing subset of the design sketch; device enumeration,
  capabilities and callbacks arrive with T10.

## Follow-ups

1. T10: implement the real Windows capture backend against `FrameRing` and `ICameraSource`; add
   enumerate/capabilities without changing the pin/release contract.
2. T11/T12: replace `ScriptedTracker` with the real backend and resolve the scripted landmark order
   through the named `LandmarkMap`.
3. T26: add per-region max-delta, PNG conversion if CI needs it, and the first avatar golden baselines.
4. Revisit the 4-slot ring and the 8x8 SSIM window if real capture or GPU variance reports drops or
   false positives.
