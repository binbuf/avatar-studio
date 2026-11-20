# ADR-0008: Eigen for native rig math

- **Status:** Proposed
- **Date:** 2026-11-20
- **Related:** [`../architecture.md`](../architecture.md), [`../face-compositing.md`](../face-compositing.md)

## Context

The native rig needs linear algebra that is small in surface area but unforgiving in correctness:

- normalised DLT for the face homography, which needs an SVD of a 9×9 system,
- Gauss–Newton refinement and RANSAC over 2D correspondences,
- quaternion filtering (One Euro) and slerp,
- a PnP solve if the MediaPipe pose is ever replaced by the ONNX fallback,
- double precision, because a homography estimated in float visibly drifts on a 1080p frame.

The application is MIT licensed and must not add a GPL dependency. The team is small, and the
differentiator is the compositor and editor, not a hand-written numerical library.

## Decision

Use **Eigen 3.4.x** (MPL2, header-only) for the native rig and tracking post-processing:

1. Pin the version and record it in `native/licenses.json`.
2. Confine Eigen to `native/rig` and the tracking adapter's post-processing. It is not exposed
   through the GDExtension boundary or the tracker's C ABI.
3. Use double precision for estimation (`Matrix3d`, `Vector3d`, `Quaterniond`); convert to Godot's
   float types only at the boundary.
4. Treat MPL2 as compatible with the MIT application: Eigen is included unmodified, and file-level
   copyleft does not reach our files.

## Alternatives considered

| Option | Why not |
|---|---|
| **Hand-rolled 3×3/4×4/quaternion math** | The small types are easy; a numerically stable SVD and PnP are not. We would be debugging the linear algebra instead of the face. |
| **GLM** | MIT and header-only, but float-oriented. Double-precision homography estimation is the requirement, and GLM's decompositions are not the right tool. |
| **Ceres Solver** | Excellent for bundle adjustment; far more dependency and build surface than a 9×9 SVD and a few refinements. |
| **OpenCV calib3d** | Already a transitive MediaPipe dependency, but pulling OpenCV into the rig couples us to its ABI and build, and duplicates what Eigen gives directly. |
| **Godot's math types** | `Transform3D`/`Basis` are float and scene-oriented; using them in the rig would put engine types in the headless core. |

## Consequences

**Positive**

- Robust SVD, decompositions, and quaternion math without writing them.
- Header-only: no runtime dependency, no ABI to ship, no DLL to sign.
- Double precision where it matters, float at the render boundary.
- The rig stays headlessly testable with no engine types.

**Negative**

- Compile time in `native/rig` increases measurably; keep Eigen out of translation units that do not
  need it.
- A new dependency to audit and pin, with an MPL2 notice.
- Contributors must not leak Eigen types across the C ABI; the boundary uses plain structs.

**Neutral**

- The license is compatible, but the `NOTICE` must include Eigen's MPL2 text.

## Follow-ups

1. Pin the exact Eigen release and hash in the dependency manifest.
2. Measure clean-build time for `native/rig` in M0; if it is excessive, isolate the SVD path behind a
   smaller interface.
3. Add a unit test that estimates a known homography with synthetic noise and asserts the expected
   error, so a version bump cannot silently change behaviour.
4. Confirm no Eigen header is reachable from `native/gdextension` or the tracker C ABI.
