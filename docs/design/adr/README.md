# Architecture Decision Records

Decisions that are expensive to reverse, recorded with their context so that a future maintainer can
tell the difference between "we chose this" and "this happened".

| ADR | Title | Status |
|---|---|---|
| [0001](0001-engine-and-language.md) | Godot 4.7 + C++20 GDExtension as the application platform | Proposed |
| [0002](0002-native-camera-capture.md) | Native camera capture on all three platforms | Proposed |
| [0003](0003-mediapipe-as-tracking-backend.md) | MediaPipe Face Landmarker behind a replaceable, C-ABI-isolated backend | Proposed |
| [0004](0004-face-decal-projection.md) | Face decals rendered on the model's own surface in canonical face space | Proposed |
| [0005](0005-output-transports.md) | Render once, distribute many; OS virtual cameras as separate processes | Proposed |
| [0006](0006-avatar-package-format.md) | Directory-based `.avatar` package with versioned declarative JSON | Proposed |
| [0007](0007-licensing-and-assets.md) | MIT application, original assets only, per-asset provenance | Proposed |
| [0008](0008-eigen-for-rig-math.md) | Eigen for native rig math | Proposed |
| [0009](0009-head-region-rotation.md) | Head-region rotation instead of whole-model rotation | Proposed |
| [0010](0010-output-is-avatar-or-nothing.md) | The output is the avatar or nothing | Proposed |
| [0011](0011-pinned-local-dependencies.md) | Pinned local dependencies and one verification entry point | Proposed |
| [0012](0012-godot-shell-bootstrap.md) | Godot shell bootstrap, godot-cpp 10 pinning, and CMake build | Accepted |
| [0013](0013-core-data-plane-primitives.md) | Core data-plane primitives and the stable diagnostics registry | Accepted |
| [0014](0014-deterministic-sources-and-golden-comparator.md) | Deterministic sources, ground truth, and the golden comparator | Accepted |
| [0015](0015-performance-harness-and-local-baseline.md) | Performance harness result schema and local baselines | Accepted |
| [0016](0016-ci-quality-gates.md) | CI quality gates, pinned formatting, and the licence manifest | Accepted |
| [0017](0017-windows-first-mvp-sequencing.md) | Windows-first MVP sequencing | Proposed |

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
