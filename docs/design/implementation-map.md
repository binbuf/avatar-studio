# Implementation map

**Status:** current as of T10 · **Last updated:** 2026-11-20
**Companion documents:** [README.md](README.md) (status vocabulary), [roadmap.md](roadmap.md)
(milestones and phases), [adr/](adr/README.md) (decisions)

This map is the honest answer to "what exists?". It is one row per design-document section or
subsystem, with the module that owns it, the task ids that implement it, and a status from the
vocabulary in [README.md](README.md#status-vocabulary). It is not a restatement of the design: the
design documents say what *should* be true, this file says what *is*.

Rules for keeping it true:

- A task flips its row from `Planned` to `Implemented` only when the behaviour ships with tests and
  the task's verify command passes. Design text alone never earns `Implemented`.
- A row that lands partially is split into two rows. There is no "partially implemented" status.
- When a task changes an owner or a task id, update the row in the same change. A stale row is a
  documentation defect, not a cosmetic one.
- `node scripts/check-design-links.mjs` fails when a row names a task id that has no task file, or
  when an ADR is not indexed. Run it after any edit here.

Status vocabulary (from [README.md](README.md#status-vocabulary)): `Planned` — designed, not built;
`Prototype` — a throwaway spike exists or is scheduled; `Implemented` — shipping behaviour, covered
by tests; `Deferred` — out of the current plan with a documented re-entry condition; `Rejected` —
considered and discarded.

**At T10 the `Implemented` rows are the foundation, the tracker seam, and the Windows capture
backend.** There is no real tracker, no renderer, no editor and no output. Everything that would let
a user see an avatar is `Planned`. The tracker C ABI and `ScriptedTracker` exist so the rig and
renderer can be built and tested before inference is proven; they are not a working tracker. The
Windows capture backend produces real NV12/YUY2/BGRA8 frames through `ICameraSource`, but nothing
consumes them yet.

The foundation rows were re-verified from a clean worktree at T08 (2026-11-20): bootstrap, configure,
RelWithDebInfo build, the full `fast` CTest label, the headless smoke, `scripts/ci.ps1`, the format,
licence and design-link checks, and the performance harness against the local baseline all passed.
The T09 tracker rows are verified by `tests/gates/T09.ps1`: the C header compiles as C11 and C++20
with warnings-as-errors, and the contract suite runs under `fast;task_T09`. The T10 capture rows are
verified by `tests/gates/T10.ps1`: the state machine runs against a fake backend under
`fast;task_T10`, `--list-cameras` enumerates without opening, and a real Logitech C920 produced
866 NV12 frames over 60 s with monotonic timestamps and a p50 callback-to-publish latency of 27 us
(`docs/progress/T10.md`).
Rows that name `.github/workflows/` are verified as workflow definitions by `tests/gates/T06.ps1`;
they have not been executed on hosted runners from this machine, which is the one residual
foundation risk recorded in [ADR-0018](adr/0018-clean-checkout-verification.md).

---

## Foundation (implemented)

| Subsystem | Owning module | Tasks | Status |
|---|---|---|---|
| Native build: CMake, warnings-as-errors, CTest harness, `verify` contract | `CMakeLists.txt`, `tests/unit/`, `scripts/verify.*` | T01 | `Implemented` |
| Core primitives: `FrameClock`, SPSC ring, latest-value slot, status codes, diagnostics registry | `native/core/` | T01, T03 | `Implemented` |
| Godot 4.7.2 shell, godot-cpp 10 pinning, GDExtension load, headless smoke | `app/`, `native/gdextension/`, `app/tests/smoke.gd` | T02 | `Implemented` |
| Capture data plane: `VideoFrame`, `FrameRing`, `ICameraSource`, test-pattern/synthetic-face/replay sources, `FaceGroundTruth` | `native/capture/` | T04 | `Implemented` |
| Tracking data plane: `TrackingResult`, `ScriptedTracker`, replay sidecar | `native/tracking/` | T04 | `Implemented` |
| Tracker seam: `ITrackingBackend`, versioned five-function C ABI, `ScriptedTracker` contract harness, shared latency histogram | `native/tracking/`, `native/core/` | T09 | `Implemented` |
| Windows capture backend: `CameraSource`, `ICameraDeviceBackend`, async Media Foundation reader, `--list-cameras`/`--capture` smoke | `native/capture/`, `native/capture/windows/` | T10 | `Implemented` |
| Golden comparator and replay fixtures | `tests/tools/golden_compare/`, `tests/fixtures/replay/` | T04 | `Implemented` |
| Performance harness, hardware fingerprint, local baseline, result schema | `tests/performance/` | T05 | `Implemented` |
| CI matrix, pinned clang-format/clang-tidy, licence manifest gate, diagnostics source scan | `.github/workflows/`, `scripts/`, `native/licenses.json` | T06 | `Implemented` |
| Design set, ADR index, this map, design-link checker | `docs/design/`, `scripts/check-design-links.mjs` | T07 | `Implemented` |

The foundation is not a vertical slice: it proves the build, the data contracts and the guardrails.
It does not produce a frame that a user can see.

---

## What does not exist yet

Nothing in this list is implemented. Most of these paths do not exist yet; `app/scenes/` contains only
the shell's `main.tscn`, and `assets/models/` contains one development model (see the legal note below).
The table is listed so that a later session cannot mistake a design section for a stub.

| Missing component | Expected location | Owning tasks |
|---|---|---|
| Real tracker backend (MediaPipe, ONNX fallback) and model files | `native/tracking/mediapipe/`, `models/` | T11 |
| macOS AVFoundation capture | `native/capture/macos/` | T52 |
| Linux V4L2/PipeWire capture | `native/capture/linux/` | T55 |
| FaceRig math: pose filter, face frame, homography, head region, framing | `native/rig/` | T18, T19, T22 |
| Avatar package load/save/validate/migrate, anchors, presets | `native/avatar/` | T29, T32 |
| Managed library, FaceProfiles, shows and settings | `native/avatar/`, `native/core/` | T30, T31, T42 |
| Output manager, readback queue, transport backends | `native/output/` | T23, T46, T47 |
| Cross-process frame transport and the three OS virtual cameras | `platform/windows/vcam/`, `platform/macos/vcam/`, `platform/linux/vcam/` | T53, T54, T56, T57 |
| Runtime scene, decal shaders, YUV conversion, preview | `app/scenes/`, `app/shaders/` | T16, T20, T21, T24, T39 |
| Editor, library, live and settings screens | `app/ui/` | T33–T42 |
| Golden-image suite, matte tests, real-model corpus | `tests/golden/` | T26, T40, T43 |
| Integration tests per platform, fuzzing | `tests/integration/` | T58, T61 |
| Bundled CC0 examples and provenance manifests | `examples/`, `assets/models/` (one unmanifested dev model exists) | T43a, T60 |
| Installers, signing, notarisation, `NOTICE` | `installer/`, `NOTICE` | T57, T59, T60 |

The **MVP** (Windows 11 + OBS, per [ADR-0017](adr/0017-windows-first-mvp-sequencing.md)) needs the
rows owned by T11–T50. The Windows capture row (T10) now exists; the rest do not.

---

## README and document map

| Section | Owning module | Tasks | Status |
|---|---|---|---|
| Status vocabulary, conventions, provenance table | `docs/design/README.md` | T07 | `Implemented` |
| Document map and this implementation map | `docs/design/` | T07 | `Implemented` |

## [product-overview.md](product-overview.md)

| Section | Owning module | Tasks | Status |
|---|---|---|---|
| Rigid-model avatar pipeline: import, calibrate, live | `native/rig/`, `native/avatar/`, `app/` | T17–T25, T29–T42 | `Planned` |
| Platform matrix (Windows 11 blocking; Windows 10 best-effort; macOS/Linux) | `native/capture/`, `native/output/`, `platform/` | T10, T52, T55, T58 | `Planned` |
| 1.0 success criteria and their measurement | `tests/`, review packages | T27, T44, T50 | `Planned` |
| Legal posture and per-asset provenance | `native/licenses.json`, package `assets.json` | T06, T29, T43a, T60 | `Planned` |
| Deferred capabilities and non-goals | — | — | `Deferred` |

The legal *policy* is written. One original development model is tracked at
`assets/models/avocado_half.glb`, but it has no per-asset provenance manifest and there is no bundled
example set; T43a/T60 own the manifests and the `examples/` package, so the row stays `Planned`.

## [architecture.md](architecture.md)

| Section | Owning module | Tasks | Status |
|---|---|---|---|
| Process and thread model: primitives | `native/core/` | T03 | `Implemented` |
| Process and thread model: capture, tracking, render, output threads | `native/`, `app/` | T16, T24, T46 | `Planned` |
| Subsystem contract — capture (`ICameraSource`, `VideoFrame`, `FrameRing`) | `native/capture/` | T04 | `Implemented` |
| Subsystem contract — capture device enumeration, capabilities, callback | `native/capture/`, `native/capture/windows/` | T10 | `Implemented` |
| Subsystem contract — tracking (`TrackingResult`, scripted tracker) | `native/tracking/` | T04 | `Implemented` |
| Subsystem contract — tracking backend (`ITrackingBackend`, C ABI) | `native/tracking/` | T09 | `Implemented` |
| Subsystem contract — real tracker backend | `native/tracking/mediapipe/` | T11 | `Planned` |
| Subsystem contract — face rig | `native/rig/` | T18, T19, T22 | `Planned` |
| Subsystem contract — output | `native/output/` | T23, T46 | `Planned` |
| Frame lifecycle (capture → pose → decal → output) | `native/`, `app/` | T16, T24 | `Planned` |
| GDExtension binding rules: shell and binding skeleton | `native/gdextension/` | T02 | `Implemented` |
| GDExtension binding rules: per-frame data-plane bindings | `native/gdextension/`, `app/` | T16 | `Planned` |
| Rendering strategy: viewport, decal pass, pacing | `app/scenes/`, `app/shaders/` | T16, T20, T21 | `Planned` |
| Colour and alpha | `app/shaders/`, `native/output/` | T20, T39, T48 | `Planned` |
| Latency budget: harness proxy metric | `tests/performance/` | T05 | `Implemented` |
| Latency budget: real capture-to-transport measurement | `tests/performance/`, `native/output/` | T26, T46 | `Planned` |
| Failure handling and recovery paths | `native/`, `app/`, `native/output/` | T23, T41, T46, T58 | `Planned` |
| Repository layout: foundation subtree | `native/`, `app/`, `tests/`, `scripts/` | T01–T07 | `Implemented` |
| Repository layout: `rig/`, `avatar/`, `output/`, `platform/`, `models/`, `examples/` | see the missing-component table above | T10–T57 | `Planned` |
| User data (delegated to `user-data.md`) | `native/avatar/` | T30, T31, T42 | `Planned` |
| Build and distribution: local build, pinned dependencies | `scripts/bootstrap.*`, `CMakeLists.txt` | T01, T06 | `Implemented` |
| Build and distribution: three-platform build in CI | `.github/workflows/` | T06 | `Implemented` |
| Build and distribution: platform runtime parity | `native/capture/*`, `native/output/`, `platform/` | T52–T58 | `Planned` |
| Build and distribution: installers, signing, notarisation | `installer/` | T57, T59 | `Planned` |
| Extensibility seam — tracker backend | `native/tracking/` | T09 | `Implemented` |
| Extensibility seam — output transports | `native/output/` | T46 | `Planned` |
| Risks and their mitigations | gates and review packages | T14, T27, T45, T50, T64 | `Planned` |
| Verification notes | `scripts/`, `.github/workflows/` | T06, T07 | `Implemented` |

## [face-compositing.md](face-compositing.md)

| Section | Owning module | Tasks | Status |
|---|---|---|---|
| Coordinate spaces: canonical face space and face-frame estimation | `native/rig/` | T18 | `Planned` |
| Region anchors | `native/rig/`, `native/avatar/` | T21, T32, T34 | `Planned` |
| Head motion and placement: rotation scope (node/cut) | `native/rig/`, `app/` | T19 | `Planned` |
| Head motion and placement: presentation and framing | `native/rig/`, `app/` | T22 | `Planned` |
| Projection mode `flat` | `app/shaders/` | T20 | `Planned` |
| Projection mode `surface` | `app/shaders/` | T21 | `Planned` |
| Projection mode `shaded surface` | `app/shaders/` | T39 | `Planned` |
| Sampling math and the decal shader | `app/shaders/` | T20, T21 | `Planned` |
| Face matte: chroma and software segmentation | `native/`, `app/shaders/` | T40 | `Planned` |
| Output contract: the avatar or nothing | `native/output/` | T23 | `Planned` |
| Lens distortion | — | — | `Deferred` |
| Colour and light matching | `app/shaders/` | T38, T39 | `Planned` |
| Model compatibility and authoring guidance | `native/avatar/`, docs | T32, T43, T62 | `Planned` |
| Performance budget for the decal pass | `tests/performance/` | T26, T46 | `Planned` |

## [tracking.md](tracking.md)

| Section | Owning module | Tasks | Status |
|---|---|---|---|
| Backend contract (five-function C ABI, versioned) | `native/tracking/` | T09 | `Implemented` |
| `TrackingResult` and the scripted tracker | `native/tracking/` | T04 | `Implemented` |
| Landmark map, asymmetric marker, profile correspondences | `native/tracking/` | T12 | `Planned` |
| MediaPipe Face Landmarker backend | `native/tracking/mediapipe/`, `models/` | T11 | `Planned` |
| Pipeline and confidence handling | `native/tracking/` | T11, T13 | `Planned` |
| Filtering, motion model, tracking rate vs render rate | `native/rig/` | T18, T24 | `Planned` |
| Segmentation output | `native/tracking/`, `native/` | T40 | `Planned` |
| Failure behaviour (blank, hold, recovery) | `native/tracking/`, `native/output/` | T23, T41 | `Planned` |
| Alternative backends: ONNX Runtime fallback | `native/tracking/` | T14 | `Deferred` (re-entry: M0 numbers miss by more than 2x) |
| Instrumentation: `TrackerStats` and latency histogram | `native/tracking/`, `native/core/` | T09 | `Implemented` |
| Instrumentation: real backend inference time and reprojection error | `native/tracking/`, `tests/performance/` | T11, T13, T26 | `Planned` |

## [camera-capture.md](camera-capture.md)

| Section | Owning module | Tasks | Status |
|---|---|---|---|
| Frame model and `FrameRing` | `native/capture/` | T04 | `Implemented` |
| Format negotiation and device enumeration | `native/capture/` | T10 | `Implemented` |
| Windows backend: Media Foundation | `native/capture/windows/` | T10 | `Implemented` |
| macOS backend: AVFoundation | `native/capture/macos/` | T52 | `Planned` |
| Linux backend: V4L2, PipeWire where required | `native/capture/linux/` | T55 | `Planned` |
| Permission and busy detection (`CAP-002`, `CAP-006`) | `native/capture/` | T10 | `Implemented` |
| Permission/privacy UI gates and deep links | `app/` | T41 | `Planned` |
| Frame ring → GPU upload | `native/capture/`, `app/` | T16, T24 | `Planned` |
| GPU zero-copy paths | — | — | `Deferred` (research spike, post-1.0) |
| Test sources: test pattern, synthetic face, replay | `native/capture/` | T04 | `Implemented` |
| Failure state machine: unplug, busy, preemption, stall, suspend/resume | `native/capture/` | T10 | `Implemented` |
| Failure surfaces in the UI | `app/` | T41 | `Planned` |
| Capture callback → ring publish (< 1 ms) | `native/capture/` | T10 | `Implemented` |
| NV12 upload, downscale and long-run drop gates | `tests/performance/` | T13, T26 | `Planned` |

## [output-and-virtual-camera.md](output-and-virtual-camera.md)

| Section | Owning module | Tasks | Status |
|---|---|---|---|
| Topology: one render, one readback, per-transport rings | `native/output/` | T16, T46 | `Planned` |
| `OutputFrame` and async readback | `native/output/` | T46 | `Planned` |
| Transport: Spout2 (Windows) | `native/output/` | T47 | `Planned` |
| Transport: Syphon (macOS) | `native/output/` | T52 | `Planned` |
| Transport: PipeWire node (Linux) | `native/output/` | T55 | `Planned` |
| Transport: window capture and OBS fallback | `app/`, docs | T23, T49 | `Planned` |
| Windows Media Foundation virtual camera | `platform/windows/vcam/` | T57 | `Planned` |
| macOS CoreMediaIO camera extension | `platform/macos/vcam/` | T54 | `Planned` |
| Linux virtual camera (`v4l2loopback` fallback) | `platform/linux/vcam/` | T56 | `Planned` |
| Cross-process frame transport | `platform/`, `native/output/` | T53 | `Planned` |
| Transport security (DACL, entitlement, nonce) | `platform/`, `native/output/` | T53, T61 | `Planned` |
| Alpha policy and test cards | `native/output/`, `app/` | T48 | `Planned` |
| Lifecycle (start, exit, suspend, lock, crash, repair) | `native/output/`, `platform/` | T46, T58 | `Planned` |
| Output performance budgets | `tests/performance/`, `native/output/` | T46 | `Planned` |

## [avatar-package-format.md](avatar-package-format.md)

| Section | Owning module | Tasks | Status |
|---|---|---|---|
| Three levels of configuration (package, profile, show) | `native/avatar/` | T29, T31, T42 | `Planned` |
| Model requirements and what the editor calibrates | `native/avatar/`, `native/rig/` | T17, T32 | `Planned` |
| Layout and `avatar.json` | `native/avatar/` | T29 | `Planned` |
| Appearance and lighting | `native/avatar/`, `app/` | T29, T38 | `Planned` |
| `face-calibration.json` | `native/avatar/`, `native/rig/` | T29, T34 | `Planned` |
| `assets.json` provenance | `native/avatar/`, `native/licenses.json` | T29, T43a, T60 | `Planned` |
| Outfits and shows | `native/avatar/` | T37, T42 | `Planned` |
| Validation | `native/avatar/` | T29 | `Planned` |
| Versioning and migration | `native/avatar/` | T29 | `Planned` |
| Import safety | `native/avatar/`, `native/capture/` | T29, T61 | `Planned` |
| Interoperability (real downloaded GLBs) | `native/avatar/` | T43 | `Planned` |

## [user-data.md](user-data.md)

| Section | Owning module | Tasks | Status |
|---|---|---|---|
| Directory layout and permissions | `native/avatar/` | T30 | `Planned` |
| Managed library and import copies | `native/avatar/` | T30 | `Planned` |
| FaceProfile: schema, capture, lifecycle | `native/avatar/` | T31 | `Planned` |
| Shows and settings | `native/avatar/`, `app/` | T42, T42b | `Planned` |
| Migration and backups | `native/avatar/` | T30, T42 | `Planned` |
| Permissions and deletion | `native/avatar/` | T30, T31, T41a | `Planned` |

## [editor-ux.md](editor-ux.md)

| Section | Owning module | Tasks | Status |
|---|---|---|---|
| Information architecture: Library, Editor, Live, Settings modes | `app/ui/` | T33 | `Planned` |
| First run and onboarding | `app/ui/` | T42a | `Planned` |
| Face calibration gizmo and behaviours | `app/ui/`, `native/rig/` | T34 | `Planned` |
| Framing editor and quality presets | `app/ui/`, `native/rig/` | T36, T42c | `Planned` |
| Props and anchors | `app/ui/`, `native/avatar/` | T37 | `Planned` |
| Live mode, HUD and transport status | `app/ui/`, `native/output/` | T25, T49 | `Planned` |
| Settings panels | `app/ui/` | T42b | `Planned` |
| States and error surfaces | `app/ui/`, `native/core/` | T41 | `Planned` |
| Keyboard and accessibility | `app/ui/` | T63 | `Planned` |
| Internationalisation | `app/ui/` | T63 | `Planned` |
| Deliberate non-features | — | — | `Deferred` |

## [security-privacy.md](security-privacy.md)

| Section | Owning module | Tasks | Status |
|---|---|---|---|
| Data inventory and retention | `native/avatar/`, `native/core/` | T30, T31, T41a | `Planned` |
| Threat model T1–T15 and its controls | `native/`, `platform/` | T10, T29, T53, T57, T61 | `Planned` |
| Untrusted avatar packages | `native/avatar/` | T29, T61 | `Planned` |
| Dependency manifest | `native/licenses.json`, `scripts/check-licenses.mjs` | T06 | `Implemented` |
| Asset manifest and `NOTICE` | `native/licenses.json`, `NOTICE` | T43a, T60 | `Planned` |
| Privileges: installer and virtual-camera registration | `installer/`, `platform/windows/vcam/` | T57, T59 | `Planned` |
| Network policy | `native/`, `.github/workflows/` | T61 | `Planned` |
| Logging and diagnostics: registry and redaction rules | `native/core/` | T03 | `Implemented` |
| Diagnostics bundle and redaction in practice | `native/core/`, `app/` | T41a | `Planned` |
| Updates and signing | `installer/` | T59 | `Planned` |
| Privacy commitments (user-facing) | docs | T62 | `Planned` |
| Incident response | docs | T64 | `Planned` |

## [testing-and-quality.md](testing-and-quality.md)

| Section | Owning module | Tasks | Status |
|---|---|---|---|
| Reference hardware and local baseline | `tests/performance/` | T05 | `Implemented` |
| Test pyramid and CTest labels | `CMakeLists.txt`, `tests/unit/` | T01, T06 | `Implemented` |
| Headless shell smoke | `app/tests/smoke.gd`, `scripts/smoke.*` | T02, T06 | `Implemented` |
| Determinism: synthetic sources, replay, golden comparator | `native/capture/`, `tests/tools/golden_compare/` | T04 | `Implemented` |
| Golden-image tests | `tests/golden/` | T26, T43 | `Planned` |
| Output invariant test (no passthrough, blank on loss) | `tests/`, `native/output/` | T23 | `Planned` |
| Matte tests | `tests/`, `native/` | T40 | `Planned` |
| Real-model corpus | `tests/`, `examples/` | T43 | `Planned` |
| Performance harness | `tests/performance/` | T05 | `Implemented` |
| Render and transport performance gates | `tests/performance/` | T26, T46 | `Planned` |
| Integration tests per platform | `tests/integration/` | T58 | `Planned` |
| CI matrix and workflow jobs | `.github/workflows/` | T06 | `Implemented` |
| Sanitizers and clang-tidy | `.github/workflows/nightly.yml` | T06 | `Implemented` |
| Fuzzing (glTF, JSON, shared-memory header) | `tests/` | T61 | `Planned` |
| Formatting and licence gates | `scripts/`, `native/licenses.json` | T06 | `Implemented` |
| Manual QA checklist | review packages | T64 | `Planned` |
| Definition of done: verify plus task gate | `scripts/verify.*`, `tests/gates/` | T01, T06 | `Implemented` |
| Bug triage policy in practice | review packages | T64 | `Planned` |

## [diagnostics.md](diagnostics.md)

| Section | Owning module | Tasks | Status |
|---|---|---|---|
| Rules: code format, stability, no reuse | `native/core/` | T03 | `Implemented` |
| Registry: CAP, TRK, RIG, AVT, OUT, VCAM, GPU, CFG, SEC codes | `native/core/` | T03 | `Implemented` |
| Logging contract | `native/core/` | T03 | `Implemented` |
| Diagnostics bundle | `native/core/`, `app/` | T41a | `Planned` |
| CI enforcement: advisory source scan | `scripts/check-diagnostics.mjs` | T06 | `Implemented` |
| CI enforcement: blocking scan and generated table | `scripts/check-diagnostics.mjs` | T41 | `Planned` |

The registry is `Implemented` ahead of the subsystems that emit its codes: codes are declared and
stable now, emission lands with each subsystem's task. No capture, tracking, output or virtual-camera
code exists to emit them yet.

## [roadmap.md](roadmap.md)

| Section | Owning module | Tasks | Status |
|---|---|---|---|
| M0–M5 milestones and exit criteria | gates and review packages | T14, T27, T45, T50, T64 | `Planned` |
| MVP definition and sequencing | this map, [ADR-0017](adr/0017-windows-first-mvp-sequencing.md) | T07 | `Implemented` |
| Consolidated open questions Q1–Q18 | gates and review packages | T14, T27, T45, T50 | `Planned` |
| Risk register | gates and review packages | T14, T27, T45, T50, T64 | `Planned` |
| Post-1.0 candidates | — | — | `Deferred` |

Milestones are `Planned` by definition: a milestone exists when its exit criterion is measured, not
when it is written down.

## [adr/](adr/README.md)

The ADR status column is the ADR vocabulary (Proposed, Accepted, Superseded), not the implementation
vocabulary. It is reproduced here so the map shows which decisions are still unvalidated; the
authoritative status is the record itself, and `check-design-links` keeps the two in step.

| Record | Scope | Tasks | ADR status |
|---|---|---|---|
| [ADR-0001](adr/0001-engine-and-language.md) | Godot 4.7 + C++20 GDExtension as the application platform | T02 | Proposed |
| [ADR-0002](adr/0002-native-camera-capture.md) | Native camera capture on all three platforms | T10, T52, T55 | Proposed |
| [ADR-0003](adr/0003-mediapipe-as-tracking-backend.md) | MediaPipe Face Landmarker behind a replaceable, C-ABI-isolated backend | T11, T14 | Proposed |
| [ADR-0004](adr/0004-face-decal-projection.md) | Face decals on the model's own surface in canonical face space | T20, T21 | Proposed |
| [ADR-0005](adr/0005-output-transports.md) | Render once, distribute many; OS virtual cameras as separate processes | T46, T53, T57 | Proposed |
| [ADR-0006](adr/0006-avatar-package-format.md) | Directory-based `.avatar` package with versioned declarative JSON | T29 | Proposed |
| [ADR-0007](adr/0007-licensing-and-assets.md) | MIT application, original assets only, per-asset provenance | T43a, T60 | Proposed |
| [ADR-0008](adr/0008-eigen-for-rig-math.md) | Eigen for native rig math, confined to `native/rig/` | T18 | Proposed |
| [ADR-0009](adr/0009-head-region-rotation.md) | Head-region rotation instead of whole-model rotation | T19 | Proposed |
| [ADR-0010](adr/0010-output-is-avatar-or-nothing.md) | The output is the avatar or nothing | T23 | Proposed |
| [ADR-0011](adr/0011-pinned-local-dependencies.md) | Pinned local dependencies and one verification entry point | T01, T06 | Accepted |
| [ADR-0012](adr/0012-godot-shell-bootstrap.md) | Godot shell bootstrap, godot-cpp 10 pinning, and CMake build | T02 | Accepted |
| [ADR-0013](adr/0013-core-data-plane-primitives.md) | Core data-plane primitives and the stable diagnostics registry | T03 | Accepted |
| [ADR-0014](adr/0014-deterministic-sources-and-golden-comparator.md) | Deterministic sources, ground truth, and the golden comparator | T04 | Accepted |
| [ADR-0015](adr/0015-performance-harness-and-local-baseline.md) | Performance harness result schema and local baselines | T05 | Accepted |
| [ADR-0016](adr/0016-ci-quality-gates.md) | CI quality gates, pinned formatting, and the licence manifest | T06 | Accepted |
| [ADR-0017](adr/0017-windows-first-mvp-sequencing.md) | Windows-first MVP sequencing | T07, T50, T51 | Proposed |
| [ADR-0018](adr/0018-clean-checkout-verification.md) | Clean-checkout verification and the implementation-map audit | T08 | Accepted |
| [ADR-0019](adr/0019-tracker-c-abi-v1.md) | Tracker C ABI v1: five functions, POD-only, latest-only | T09 | Accepted |
| [ADR-0020](adr/0020-capture-state-machine-and-mf-backend.md) | Platform-neutral capture state machine with a thin Media Foundation backend | T10, T52, T55 | Accepted |

---

## Phase roll-up

| Phase | Tasks | What it flips in this map |
|---|---|---|
| 0 Foundation | T01–T08 | Foundation rows; this map; the guardrails |
| 1 Feasibility (M0) | T09–T15 | Tracker backend and Windows capture rows |
| 2 Vertical slice (M1) | T16–T28 | Face rig, decal modes, framing, output gate, golden suite |
| 3 Quality and editor (M2) | T29–T45 | Package, library, profiles, editor screens, matte, corpus |
| 4 OBS and MVP | T46–T51 | Output manager, Spout2, alpha, OBS onboarding, MVP gate |
| 5 Platform parity | T52–T58 | macOS/Linux capture and transports, all three virtual cameras |
| 6 Release hardening | T59–T64 | Signing, licence audit, security, docs, release gate |

Phase 5 is sequenced after the MVP gate by
[ADR-0017](adr/0017-windows-first-mvp-sequencing.md); it is not optional and not out of 1.0.
