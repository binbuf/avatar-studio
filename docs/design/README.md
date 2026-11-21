# Avatar Studio — Design Documentation

**Status:** proposal; the foundation is implemented (native core T01, headless Godot shell T02,
data-plane primitives and diagnostics registry T03, deterministic sources, scripted tracker, replay
fixtures and golden comparator T04, performance harness and local baseline T05, CI/format/licence
gates T06), the tracker C ABI and contract harness are implemented (T09), the Windows Media
Foundation capture backend is implemented (T10), and the foundation was re-verified from a clean
checkout by the T08 audit (ADR-0018), the real Windows CPU tracker is implemented (T11), the
named landmark map with its asymmetric-marker gate is implemented (T12), and the headless M0
harness with its 30-minute soak is implemented (T13). Rendering, the editor, output and the
macOS/Linux capture backends remain `Planned` unless explicitly marked otherwise.
T14 retains the Windows tracker but leaves M0 open pending platform-build and pose-review
evidence; [ADR-0024](adr/0024-m0-windows-evidence-and-open-gate.md) records the gate boundary.
**Last updated:** 2026-11-21
**Origin:** an exported research conversation ("the brief", 2026-11-20), not retained in this
repository. It is the origin of this design, not a specification. Its reviewed claims are quoted in
[Provenance and corrections](#provenance-and-corrections-to-the-original-brief) so the decision
record survives without the source file.

Avatar Studio is a desktop application that turns a rigid 3D model into a live avatar whose
eyes and mouth are real webcam pixels, driven by head tracking. It targets Windows, macOS and
Linux, runs fully offline, and is meant to be usable without Blender, without the Godot editor,
and without OBS (though OBS is a first-class consumer).

The defining technique is not a face rig: it is **webcam footage projected onto an ordinary
static mesh, in a head-stable coordinate space**. That single decision drives most of the
architecture in this document set.

---

## Document map

| Document | Scope | Primary audience |
|---|---|---|
| [`product-overview.md`](product-overview.md) | Vision, users, scope, non-goals, success criteria, legal posture | everyone |
| [`architecture.md`](architecture.md) | System structure, processes, threads, interfaces, build, distribution, risks | engineers |
| [`face-compositing.md`](face-compositing.md) | The core rendering technique: face frame math, projection modes, shader design | rendering engineers |
| [`tracking.md`](tracking.md) | Tracking backend contract, MediaPipe usage, filtering, failure handling | CV / systems engineers |
| [`camera-capture.md`](camera-capture.md) | Capture abstraction, formats, timestamps, per-platform backends, permissions | systems engineers |
| [`output-and-virtual-camera.md`](output-and-virtual-camera.md) | Render-once/distribute-many, OBS transports, OS virtual cameras, cross-process frames | systems engineers |
| [`avatar-package-format.md`](avatar-package-format.md) | `.avatar` package, JSON schema, anchors, props, presets, versioning | tooling / content |
| [`user-data.md`](user-data.md) | Managed library, FaceProfiles, shows/settings, migration, permissions | tooling / app |
| [`editor-ux.md`](editor-ux.md) | Screens, calibration flow, states, error surfaces, accessibility | product / UX |
| [`security-privacy.md`](security-privacy.md) | Threat model, camera-data policy, package sanitisation, installer privileges | security reviewers |
| [`testing-and-quality.md`](testing-and-quality.md) | Test pyramid, synthetic face source, golden frames, CI matrix, perf gates | QA / CI owners |
| [`diagnostics.md`](diagnostics.md) | Stable support codes, logging contract, diagnostics bundle | support / QA / engineers |
| [`roadmap.md`](roadmap.md) | Milestones with exit criteria, MVP sequencing, open questions, risk register | leads |
| [`implementation-map.md`](implementation-map.md) | Row-by-row status: subsystem, owning module, task ids, implemented vs planned | everyone |
| [`adr/`](adr/README.md) | Architecture decision records | everyone |

Read in this order: implementation map → overview → architecture → face-compositing → roadmap. The
rest are reference. The map is the only document that tracks what is built; the others describe
intent.

---

## Status vocabulary

Used consistently in every document:

| Status | Meaning |
|---|---|
| `Planned` | Designed here, not built |
| `Prototype` | A throwaway spike exists or is scheduled in the current milestone |
| `Implemented` | Shipping behaviour, covered by tests |
| `Deferred` | Deliberately out of the current plan, with a documented re-entry condition |
| `Rejected` | Considered and discarded; reason recorded in an ADR |

Current reality: only the foundation exists. Implemented today: the framework-free C++20 core
(SPSC ring, latest-value slot, `FrameClock`, diagnostics registry), the pinned
bootstrap/verification contract, the headless Godot shell with a loadable GDExtension (T01–T03), the
deterministic capture and tracking sources with their golden comparator (T04), the performance
harness and local baseline (T05), the three-platform CI matrix with formatting and licence gates
(T06), the tracker C ABI with its scripted contract harness (T09), the Windows Media Foundation
capture backend with its platform-neutral state machine (T10), the real Windows CPU tracker (T11),
the named landmark map with profile correspondences (T12), and the headless M0 harness with its
30-minute soak (T13). Every avatar capability — the face
rig, the decal modes, the editor, the output manager and the OS virtual cameras — is `Planned`. The MVP is a Windows 11
streamer running OBS over Spout2 or window capture; macOS/Linux parity and the OS virtual cameras
remain 1.0 requirements and are sequenced after it by
[ADR-0017](adr/0017-windows-first-mvp-sequencing.md).

[`implementation-map.md`](implementation-map.md) is the authoritative status: one row per design
section or subsystem with its owning module, task ids and status. Read it before trusting any other
document, and update it in the same change that flips a row. The first spike is M0 in
[`roadmap.md`](roadmap.md). The only concrete prior art is the original research conversation and the
third-party projects reviewed in
[`product-overview.md`](product-overview.md#prior-art-reviewed).

---

## Conventions used throughout

- **Coordinate systems.** Right-handed, Y-up, forward is −Z (glTF convention). Avatar and world
  units are metres. The canonical face space is normalised (inter-ocular distance = 1.0) and is
  defined in [`face-compositing.md`](face-compositing.md#coordinate-spaces).
- **Timestamps.** All timestamps are monotonic nanoseconds from a single process-wide
  `FrameClock`. Wall-clock time is never used for synchronisation.
- **Identifiers.** Stable IDs are lower_snake_case ASCII (`head_top`, `eye_l`). Package IDs are
  UUIDv4.
- **Sidedness.** `_l` and `_r` are the subject's left and right, never the viewer's. In an
  unmirrored image the subject's right eye appears on the viewer's left.
- **Head motion.** 1.0 is rotation-only, applied to the head region (a node or a neck cut); the body
  stays still. Framing is fixed; a translation gain is `Deferred`.
- **Output.** The output is the rendered avatar or a blank frame. The camera is never passed through,
  and a lost face blanks rather than freezing.
- **Colour.** Internal working space is linear; output is encoded to sRGB. Premultiplication is
  explicit at every boundary (see [`architecture.md`](architecture.md#colour-and-alpha)).
- **Units in JSON.** Metres and degrees. Quaternions are `[x, y, z, w]`. Both are stated in the
  schema so that no field is ambiguous.

---

## Provenance and corrections to the original brief

The design began as a review of an exported research conversation ("the brief"), which is not
retained in this repository. It was a research conversation, not an engineering document: it
reached the right headline conclusions, but several load-bearing claims were stale, imprecise, or
silently scope-limiting. This section records what was checked and what changed as a result, and
quotes each claim so the decision history survives without the source file. All checks were
performed on 2026-11-20 against upstream documentation.

| # | Claim in the brief | Finding | Design response |
|---|---|---|---|
| 1 | Godot 4.7.2 is current stable | Confirmed. 4.7.2 shipped 2026-08-18; 4.8 is in beta. | Pin 4.7.2; upgrade policy in [ADR-0001](adr/0001-engine-and-language.md) |
| 2 | Godot has no webcam support on Windows, but does on macOS/Linux | Confirmed. `CameraServer` is implemented only on Linux, Android, macOS, iOS. | Still write native capture everywhere for timestamp/format control — [ADR-0002](adr/0002-native-camera-capture.md) |
| 3 | MediaPipe C++ is "experimental" on Windows | Confirmed, and worse: Windows builds need MSYS2 + Bazel + a pinned OpenCV, and GPU inference is unavailable on Windows *and* macOS desktop. CPU-only inference must be assumed. | M0 validation gate, narrow C-ABI wrapper, replaceable tracker — [ADR-0003](adr/0003-mediapipe-as-tracking-backend.md) |
| 4 | Windows virtual camera = `MFCreateVirtualCamera` | Correct but understated: Windows 11 build 22000+, the media source is a COM server that must be registered in `HKLM` (administrator), packaged apps need the webcam capability, and it must not be called from the UI thread. | Installer design + cross-process frame transport — [ADR-0005](adr/0005-output-transports.md) |
| 5 | macOS virtual camera = signed camera extension | Correct, and the extension runs as its own sandboxed process, communicating with the app over an app-group Mach service. App Store delivery is supported. | Cross-process transport, not an in-process sink |
| 6 | Linux virtual camera = `v4l2loopback` | Outdated as the primary path. A PipeWire `Video/Source` node with `media.role=Camera` is user-space, needs no kernel module, no root, and is consumed by OBS's PipeWire camera source and by browsers that enable PipeWire camera support. | PipeWire primary, `v4l2loopback` fallback |
| 7 | `SpectatorStreamExporter` is a good reference for OBS output | Confirmed: MIT, Godot 4.7, Spout2/Syphon/PipeWire, async readback with a drop-oldest ring. Also confirmed: all backends copy through the CPU (~500 MB/s at 1080p60) and it requires Forward+/Mobile (RenderingDevice). | Adopt its frame-queue design; treat zero-copy as research — [ADR-0005](adr/0005-output-transports.md) |
| 8 | "≤70 ms median capture-to-preview latency" | Ambiguous and partly unachievable: consumer webcams add 30–60 ms of sensor/ISP latency before the app sees a frame. | Split into app-side and end-to-end budgets with explicit measurement definitions — [`architecture.md`](architecture.md#latency-budget) |
| 9 | "We do not need a companion app" | True for capture. False for virtual cameras: on Windows the media source is loaded into *other* applications' processes, and on macOS the extension is a separate process. | Recorded as a correction; cross-process frame sharing is now a first-class subsystem |
| 10 | Recreating the effect is legally fine; the character is not | Directionally right, but this is not a clearance opinion, and the trademark-application claim is unverified. | Written as policy, not legal advice — [ADR-0007](adr/0007-licensing-and-assets.md) |
| 11 | Don't rewrite Godot's renderer; C++ for the rest | Sound. One refinement: the GDExtension boundary is expensive *per call*, not per byte, so the data plane must never round-trip through `Variant`. | Binding rules in [`architecture.md`](architecture.md#gdextension-binding-rules) |
| 12 | "Any `.glb` works; no special mesh needed" | True for flat insets; overstated for surface projection and head rotation, where occlusion, silhouette, decal placement and the neck cut all need per-model calibration. | Automatic placement is an assistant, never a guarantee - [`editor-ux.md`](editor-ux.md#face-calibration) |

Two things the brief does not discuss at all, and which this design treats as first-class:
**frame-content vs. pose synchronisation** (mismatched camera and tracking timestamps cause the
face to "swim" across the model) and **packaging/privilege** (the two virtual-camera platforms
require installer-level registration).

---

## What was deliberately left out of this proposal

- **Full-body or rigged characters.** A rigged `.glb` loads and animates nothing: only the root
  transform is driven. Rig-driven animation is not designed here.
- **Audio, recording, and stream orchestration.** Out of scope; OBS owns that.
- **Mobile and web targets.** Not planned.
- **Cloud anything.** The product is offline by design.

See [`product-overview.md`](product-overview.md#non-goals) for reasoning and re-entry conditions.
