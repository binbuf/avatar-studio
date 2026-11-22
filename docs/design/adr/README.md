# Architecture Decision Records

Decisions that are expensive to reverse, recorded with their context so that a future maintainer can
tell the difference between "we chose this" and "this happened".

| ADR | Title | Status |
|---|---|---|
| [0001](0001-engine-and-language.md) | Godot 4.7 + C++20 GDExtension as the application platform | Proposed |
| [0002](0002-native-camera-capture.md) | Native camera capture on all three platforms | Proposed |
| [0003](0003-mediapipe-as-tracking-backend.md) | MediaPipe Face Landmarker behind a replaceable, C-ABI-isolated backend | Accepted |
| [0004](0004-face-decal-projection.md) | Face decals rendered on the model's own surface in canonical face space | Accepted |
| [0005](0005-output-transports.md) | Render once, distribute many; OS virtual cameras as separate processes | Proposed |
| [0006](0006-avatar-package-format.md) | Directory-based `.avatar` package with versioned declarative JSON | Proposed |
| [0007](0007-licensing-and-assets.md) | MIT application, original assets only, per-asset provenance | Proposed |
| [0008](0008-eigen-for-rig-math.md) | Eigen for native rig math | Accepted |
| [0009](0009-head-region-rotation.md) | Head-region rotation instead of whole-model rotation | Accepted |
| [0010](0010-output-is-avatar-or-nothing.md) | The output is the avatar or nothing | Proposed |
| [0011](0011-pinned-local-dependencies.md) | Pinned local dependencies and one verification entry point | Accepted |
| [0012](0012-godot-shell-bootstrap.md) | Godot shell bootstrap, godot-cpp 10 pinning, and CMake build | Accepted |
| [0013](0013-core-data-plane-primitives.md) | Core data-plane primitives and the stable diagnostics registry | Accepted |
| [0014](0014-deterministic-sources-and-golden-comparator.md) | Deterministic sources, ground truth, and the golden comparator | Accepted |
| [0015](0015-performance-harness-and-local-baseline.md) | Performance harness result schema and local baselines | Accepted |
| [0016](0016-ci-quality-gates.md) | CI quality gates, pinned formatting, and the licence manifest | Accepted |
| [0017](0017-windows-first-mvp-sequencing.md) | Windows-first MVP sequencing | Proposed |
| [0018](0018-clean-checkout-verification.md) | Clean-checkout verification and the implementation-map audit | Accepted |
| [0019](0019-tracker-c-abi-v1.md) | Tracker C ABI v1: five functions, POD-only, latest-only | Accepted |
| [0020](0020-capture-state-machine-and-mf-backend.md) | Platform-neutral capture state machine with a thin Media Foundation backend | Accepted |
| [0021](0021-real-mediapipe-windows-backend.md) | Real MediaPipe Windows backend and its isolated build | Accepted |
| [0022](0022-named-landmark-map-and-asymmetric-marker.md) | Named landmark map, pinned backend tables, and the asymmetric-marker gate | Accepted |
| [0023](0023-m0-headless-harness-and-thresholds.md) | Headless M0 harness, measurement definitions, and soak thresholds | Accepted |
| [0024](0024-m0-windows-evidence-and-open-gate.md) | Retain the Windows tracker while the full M0 gate remains open | Accepted |
| [0025](0025-m0-windows-scope-and-deferred-platform-evidence.md) | Close M0 on Windows feasibility and defer cross-platform evidence | Accepted |
| [0026](0026-fixed-60hz-pacing-and-one-viewport-preview.md) | Fixed 60 Hz pacing, one viewport, and a preview copy | Accepted |
| [0027](0027-runtime-glb-import.md) | Runtime GLB import builds a rigid bind-pose scene from GLTFState | Accepted |
| [0028](0028-facerig-estimation-and-temporal-contract.md) | FaceRig estimation and temporal contract | Accepted |
| [0029](0029-shared-cpu-head-deformation.md) | Shared CPU vertex buffers for head deformation | Accepted |
| [0030](0030-flat-decal-quad-pass.md) | Flat decal as a head-parented quad pass with clamped sampling | Accepted |
| [0031](0031-surface-decal-shared-pass.md) | One shared-geometry surface decal pass | Accepted |
| [0032](0032-presentation-framing-and-mirroring.md) | Fixed presentation, swept fit, and display-only preview mirroring | Accepted |

Status values: **Proposed** (agreed, not yet validated by implementation), **Accepted** (validated
and in force), **Superseded by ADR-XXXX** (kept for history), **Rejected**.

## Template

```markdown
# ADR-000N: Title

- **Status:** Proposed
- **Date:** YYYY-MM-DD
- **Supersedes:** —
- **Related:** docs/design/<doc>.md

## Context
What forces are at play. Include constraints and what was true at the time.

## Decision
One paragraph, present tense, unambiguous.

## Alternatives considered
Table: option, why not.

## Consequences
Positive / negative / neutral. Be honest about the cost.

## Follow-ups
What must be verified, and by when.
```

Rules:

1. ADRs are immutable once accepted. Corrections are new ADRs that supersede them.
2. An ADR that is not linked from a design document is probably not a real decision.
3. "We'll decide later" is a valid ADR outcome only if it names the milestone that decides.
