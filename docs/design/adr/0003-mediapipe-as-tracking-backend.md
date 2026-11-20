# ADR-0003: MediaPipe Face Landmarker behind a replaceable, C-ABI-isolated backend

- **Status:** Proposed
- **Date:** 2026-11-20
- **Related:** [`../tracking.md`](../tracking.md)

## Context

The compositor needs, from one pass over a frame: dense landmarks (for the face frame and region
anchors), a metric head pose (to rotate the avatar), and — for future work — expression coefficients.

MediaPipe Face Landmarker provides all three, is Apache-2.0 licensed at the source level, and runs on
CPU. But its desktop C++ story is weak:

- Windows support is officially experimental: MSYS2, Bazel, a pinned OpenCV 3.4.x, MSVC only.
- GPU inference is unavailable on Windows and macOS desktop, so CPU performance is the only
  performance.
- It vendors Abseil, Protobuf and OpenCV, which will collide with any other copy in the process.
- Its Tasks C++ API has changed between releases.

At the same time, no alternative provides all three outputs with a better maintenance story.

## Decision

Use **MediaPipe Face Landmarker** as the first `ITrackingBackend`, but:

1. compile it into its own shared library exposing a **five-function C ABI**
   (`create`, `configure`, `push_frame`, `poll_result`, `destroy`);
2. never include a MediaPipe header outside that library;
3. pin the version and the model file (with a checksum) in the dependency manifest;
4. keep the post-processing (pose interpretation, filtering, face frame) in our code, not in the
   backend, so a replacement backend inherits it;
5. treat a fallback to **ONNX Runtime + a landmark model** as a planned pivot, not a crisis.

## Alternatives considered

| Option | Why not (yet) |
|---|---|
| **ONNX Runtime + landmark model** | Best licensing and build story, but requires model selection/training work and a pose solver. Kept as the designed fallback; not worth doing first while MediaPipe works. |
| **Apple Vision (macOS only)** | Excellent on macOS, useless on the other two platforms, coarse mouth landmarks, no expression coefficients. |
| **OpenSeeFace** | VTubing-oriented, older model stack, separate runtime; no clear advantage over MediaPipe. |
| **Cloud inference** | Violates the offline and privacy principles outright. |
| **Custom model** | Training and maintenance burden with no product differentiation. |
| **Link MediaPipe statically into the app** | ODR/ABI collisions with vendored Abseil/Protobuf, and it couples every build to Bazel. |

## Consequences

**Positive**

- Landmarks, pose and blendshapes from one dependency.
- The C ABI makes the tracker a build artifact that can be swapped without rebuilding the app, and
  isolates the vendored dependency graph.
- A failed M0 has a pre-designed exit instead of a redesign.

**Negative**

- A Bazel build in a CMake project, with its own toolchain pinning. This is the least pleasant part
  of the build and must be cached in CI aggressively.
- CPU-only inference sets the performance floor for the whole product.
- Windows builds are the fragile ones; a CI break there blocks releases.
- Model weights need separate licence review from the source licence.

**Neutral**

- The five-function C ABI is a constraint on what a future backend can expose; it is deliberately
  shaped around "one frame in, one result out" and will need extending (not redesigning) for
  multi-face.

## Follow-ups

1. M0 exit criteria in [`../roadmap.md`](../roadmap.md#m0--feasibility-spike) are the validation of
   this ADR. If they fail, write ADR-0008 superseding this one.
2. Record the exact MediaPipe commit, Bazel version, OpenCV version and model hash in the manifest.
3. Add a nightly job that rebuilds the tracker from a clean container, so drift is caught early.
4. Confirm the model weight licence in writing before M5.
