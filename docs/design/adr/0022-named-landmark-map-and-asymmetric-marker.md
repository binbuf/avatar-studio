# ADR-0022: Named landmark map, pinned backend tables, and the asymmetric-marker gate

- **Status:** Accepted
- **Date:** 2026-11-20
- **Related:** [tracking](../tracking.md), [user-data](../user-data.md),
  [ADR-0003](0003-mediapipe-as-tracking-backend.md), [ADR-0019](0019-tracker-c-abi-v1.md)

## Context

T11 proved a real MediaPipe backend, but its result is a flat array of 478 points. The rig needs
anatomical correspondences (eye corners, nose, mouth, jaw, temples, cheeks) to build a homography and
a `FaceProfile`; a raw index in the rig or in a saved profile couples both to one model version. A
mirrored index table produces a plausible but wrong face, so a mapping bug can pass every geometry
check while silently swapping the subject's left and right.

The evaluated ONNX Face Mesh V2 conversion is not a shipping backend, but a future fallback must be
able to replace MediaPipe without rewriting the rig or the profile. The ONNX conversion preserves the
478-point order today; inheriting that fact implicitly would hide the dependency.

## Decision

Use a stable `LandmarkName` set from `tracking.md` and a `LandmarkMap` that resolves names to the
active backend's indices. Each backend owns a separate, pinned table: MediaPipe 0.10.21 with the
`face_landmarker/face_landmarker/float16/1` bundle, and the evaluated
`senty-au/face_landmarks_detector-ONNX` conversion at `337d582…`. The tables live in
`native/tracking/include/avatar/tracking/landmark_map.h`; a future adapter calls
`LandmarkMap::for_backend` rather than copying indices.

Resolution is explicit: an unknown name or a name the backend cannot provide is `nullopt`, never a
guessed index. A table that binds two names to one index reports the duplicate and ignores the later
binding. The rig never hard-codes a backend index.

Canonical face space is a 2-D similarity: origin at the eye midpoint, +u toward the subject's right
eye, +v toward the forehead, inter-ocular distance 1.0, image x scaled by `width / height`, and the
frame follows head roll. The build includes an asymmetric-marker test: a dot on the subject's right
cheek must be left of the eye midpoint in an unmirrored image and have positive canonical u; a
deliberately swapped `_l`/`_r` table must fail. Both pinned tables are tested.

A `FaceProfile` stores names and canonical `(u,v)`, never indices, and round-trips through JSON
without losing names. The solver requires at least eight usable correspondences before RANSAC. A
profile name the active backend cannot provide falls back to the remaining subset and raises
`RIG-003`; unknown names are ignored; duplicate backend indices are reported and used once. T18's
post-RANSAC inlier gate is a separate `RIG-002` check.

## Alternatives considered

| Option | Why not |
|---|---|
| Hard-code MediaPipe indices in the rig and profile | A model update or backend swap silently changes meaning; profiles become model-specific |
| Store backend indices in `FaceProfile` | Same coupling; a profile could not survive a tracker replacement |
| Use one shared table for MediaPipe and ONNX | Hides that the ONNX table is an independent, unvalidated pin; a conversion could reorder points |
| Infer handedness from face geometry at runtime | A mirrored table is self-consistent and can pass; the build would not fail loudly |
| Make the marker test a manual M0 checklist item | The failure is easy to miss and expensive to debug later |

## Consequences

A backend swap is mechanical: add or select a table, resolve names, and keep the profile. The
asymmetric-marker test is a build gate, so a mirrored table fails in CI rather than in a user's face.
The cost is two tables to review whenever a model or conversion changes, and the ONNX table is pinned
without a shipping ONNX runtime today. The full profile document, capture and storage remain T31;
the homography and RANSAC remain T18.

## Follow-ups

- T18 consumes the named map for the homography and reports the post-RANSAC inlier count.
- T31 embeds the named landmark array in the full `FaceProfile` document without adding indices.
- T14 revalidates the ONNX table only if an ONNX backend is actually built.
- Any pinned model or conversion change must rerun the asymmetric-marker test and review the table.
