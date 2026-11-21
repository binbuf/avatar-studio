# ADR-0019: Tracker C ABI v1: five functions, POD-only, latest-only

- **Status:** Accepted
- **Date:** 2026-11-20
- **Supersedes:** —
- **Related:** [`../tracking.md`](../tracking.md), [ADR-0003](0003-mediapipe-as-tracking-backend.md)

## Context

ADR-0003 requires the tracker to be replaceable and isolated: MediaPipe vendors Abseil, Protobuf and
OpenCV, and those copies will collide with anything else in the process. The application therefore
cannot include a tracker vendor header or link a tracker C++ API directly.

T09 has to make the seam concrete before inference exists, because the rig, renderer and output gate
all need a deterministic `TrackingResult` source. The seam has to be stable enough for T11 to
implement MediaPipe behind it and narrow enough that a failed M0 can swap in ONNX Runtime without
touching the application.

## Decision

Define the tracker boundary as a versioned five-function C ABI in
`native/tracking/include/avatar/tracker/tracker_c_api.h`:

`avatar_tracker_create`, `avatar_tracker_configure`, `avatar_tracker_push_frame`,
`avatar_tracker_poll_result`, `avatar_tracker_destroy`.

1. **Versioned.** `AVATAR_TRACKER_ABI_VERSION` is `1`. `create` and `configure` reject a config whose
   `abi_version` differs with `AVATAR_TRACKER_ERROR_ABI_MISMATCH`. A layout or semantic change bumps
   the version; the caller never guesses.
2. **POD only.** The header includes only `<stdint.h>` and `<stddef.h>`, is C11-compatible, and
   carries no `std::` type, template, exception or allocation across the boundary. All arrays are
   fixed-size. `avatar_tracker_result` is 5992 bytes with a documented offset table and 8-byte
   alignment; `avatar_tracker_frame` is pointer-size dependent and both sides share a process.
3. **One frame in, newest result out.** `push_frame` never blocks on inference and never queues. A
   busy backend drops the frame and returns `AVATAR_TRACKER_OK`; the drop is counted in
   `TrackerStats`. `poll_result` returns the newest unconsumed result or `AVATAR_TRACKER_NO_RESULT`.
   This is the latest-value-slot policy from ADR-0013, not a queue.
4. **Errors, not exceptions.** Every function returns `avatar_tracker_error` (except `destroy`, which
   is `void` and null-safe). The implementation catches every exception at the boundary and converts
   it to `AVATAR_TRACKER_ERROR_INTERNAL`. A test-only `simulate_fault` config field proves this.
5. **Two interfaces, one backend.** `ITrackingBackend` is the app-side C++ seam
   (`itracking_backend.h`); `ScriptedTracker` implements it and the C ABI. T11 implements the C ABI
   in a separate shared library and a thin C++ adapter on this side.
6. **Instrumentation is part of the contract.** `TrackerStats` records submitted/produced/dropped
   counts with a per-reason breakdown, input resolution, a ten-bin confidence distribution and an
   inference-time histogram. The histogram is `core::LatencyHistogram<>`, shared with the
   performance harness, so a percentile has one definition.

## Alternatives considered

| Option | Why not |
|---|---|
| Expose the MediaPipe C++ API directly | Vendor headers and vendored Abseil/Protobuf collide with the app; violates ADR-0003. |
| A C++ virtual interface as the library boundary | ABI fragility across compilers, allocators and STL versions; exceptions and ownership cross the boundary. |
| `std::optional`/`std::vector` in the C ABI | Allocator and layout ownership become part of the contract; impossible to consume from C. |
| Queue frames in the caller | Adds latency and unbounded memory; the product wants the newest face, not every frame. |
| Return the latest result forever from `poll_result` | Makes "new result" indistinguishable from "same result"; the renderer would reprocess or mis-time frames. |
| Six-plus functions (query version, query stats, reset) | The version macro and config cover versioning; stats are C++-side until a real backend needs them across the ABI. |

## Consequences

**Positive**

- The application can be built and tested against `ScriptedTracker` before inference works.
- A real backend is a shared library with no vendor headers in the app.
- The layout is checkable from C and C++ at compile time, so an accidental field move fails the build.
- The same contract tests apply to the scripted backend today and MediaPipe later.

**Negative**

- The fixed result struct is large (5992 bytes) and copies on every poll. That is acceptable for one
  face at 30 Hz; multi-face will need an extension, not a redesign.
- `avatar_tracker_frame` contains pointers, so the ABI is process-local and pointer-width dependent.
  Cross-process tracking is not a goal.
- A backend must copy pixels if it needs them after `push_frame`; the scripted backend does not.
- Adding a field or an error code is an ABI version bump even when the change looks additive.

**Neutral**

- `simulate_fault` exists in the production header only to make exception containment testable. It is
  documented as test-only and defaults to `AVATAR_TRACKER_FAULT_NONE`.
- `TrackerStats` stays on the C++ side for now; T11 can add a stats query if the debug HUD needs it
  across the shared-library boundary.

## Follow-ups

1. T11: implement MediaPipe behind this ABI, record real inference samples, and run the same contract
   suite against the shared library.
2. T12: resolve the scripted landmark order through the named `LandmarkMap`; the ABI's 478 slots do
   not change.
3. T13/T26: add landmark reprojection error and feed the histogram into the debug HUD and the
   performance result.
4. Extend for multi-face only when the product needs it; keep v1's one-face semantics as the default.
