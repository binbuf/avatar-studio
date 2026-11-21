# ADR-0021: Real MediaPipe Windows backend and its isolated build

- **Status:** Accepted
- **Date:** 2026-11-20
- **Related:** [tracking](../tracking.md), [ADR-0003](0003-mediapipe-as-tracking-backend.md), [ADR-0019](0019-tracker-c-abi-v1.md)

## Context

T11 must prove Windows CPU inference through the existing five-function ABI, without importing
MediaPipe's dependency graph into CMake or confusing synthetic results with real tracking.
MediaPipe 0.10.21 builds with Bazel 6.5.0, MSYS2 bash, MSVC 14.51 and OpenCV 3.4.10. Its default
Python 3.13 selection fails; the supported hermetic Python 3.12 selection works. MSVC rejects
a preprocessor directive inside `MP_ASSIGN_OR_RETURN`. The initial DLL also exports glog and
TensorFlow Lite symbols despite a module-definition file.

## Decision

Keep ADR-0003 and use pinned MediaPipe commit `cad7f3ab99ebf175947e40c5252c642612aae927`.
`scripts/build-tracker.ps1` builds an offline SDK under `.tools/tracker-sdk`. Record the small
MSVC macro patch and the glog static-export patch; compile with `TFL_STATIC_LIBRARY_BUILD`.
Check the actual PE export table for exactly the five ABI functions. CMake copies the SDK and
runs ABI tests; vendor headers stay inside `native/tracking/mediapipe/`.

Use synchronous `DetectForVideo` on a dedicated worker, with one owned frame and no queued
successor. This makes busy drops explicit and pairs each result with its original nanosecond
timestamp. Only the graph timestamp is clamped to strictly increasing milliseconds. Reconfigure
replaces the graph and discards outstanding results. One face, CPU delegate, pose and blendshapes
are enabled. Letterbox with replicated edges and invert that mapping for landmarks.

The audited Google model bundle is hash-checked before initialization and ships beside the DLL.
All three model cards explicitly license their weights under Apache-2.0; individual member hashes
and evaluated ONNX alternatives are recorded in `native/licenses.json`. Benchmark 192 and 256
**prepared image** sizes; the upstream detector/landmark tensors remain 128/256 respectively.
The ONNX fallback probe likewise rejects a native 192 tensor; no weights are modified to pretend
otherwise. Confidence is an explicitly documented geometric trust proxy after MediaPipe's
thresholds. `features_valid` additionally requires usable eyes, irises and mouth geometry.

## Consequences

Windows feasibility is demonstrated without a backend pivot. Building from source is substantially
heavier than the app build, and the custom-op compiler step takes several minutes. SDK builds need
MSYS2, MSVC and network access; application execution needs none of those and downloads nothing.
The private measurement tool can inspect exact worker counters and bounded latency samples.
ABI v1 has no stats query; wiring those counters into the future app HUD requires a separate
decision. Landmark geometry is not an occlusion classifier, and the confidence proxy is not a
calibrated probability. Occlusion, pose quality, identity switching and the M0 soak remain later
validation. macOS/Linux builds remain planned. ADR-0012 is already the Godot bootstrap decision
and is preserved; no ONNX pivot ADR is needed.
