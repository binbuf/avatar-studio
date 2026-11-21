# ADR-0017: Windows-first MVP sequencing

- **Status:** Proposed
- **Date:** 2026-11-20
- **Supersedes:** —
- **Related:** [roadmap.md](../roadmap.md), [architecture.md](../architecture.md),
  [output-and-virtual-camera.md](../output-and-virtual-camera.md),
  [ADR-0001](0001-engine-and-language.md), [ADR-0002](0002-native-camera-capture.md),
  [ADR-0005](0005-output-transports.md), [ADR-0010](0010-output-is-avatar-or-nothing.md)

> **Numbering note.** The task that produced this record asked for ADR-0011. Number 0011 is already
> taken by [pinned local dependencies](0011-pinned-local-dependencies.md), which is referenced from
> `architecture.md`, `testing-and-quality.md`, ADR-0012 and ADR-0016. Renumbering an accepted ADR
> would break those links, so this decision takes the next free number, 0017.

## Context

The design set targets three platforms (Windows 11, macOS 13+, Linux x86-64) and six milestones, and
`roadmap.md` orders the work by risk: tracker viability (M0), then face quality (M1/M2), then OBS
transport (M3), then OS virtual cameras (M4), then release hardening (M5). Nothing in that order says
which platform proves the effect first.

Three facts make a simultaneous three-platform start the wrong shape:

- The primary user is a Windows streamer putting an avatar over gameplay in OBS. OBS has a Spout2
  plugin on Windows, a built-in Syphon client on macOS, and a beta PipeWire camera source on Linux.
  Only one of those is a mature third-party-plugin path.
- Each platform's output work is a different API and a different installer: Windows Media Foundation
  virtual camera plus `HKLM` COM registration, a macOS CMIO system extension plus notarisation, and a
  Linux PipeWire node plus a `v4l2loopback` fallback. Each needs hardware and an OS approval flow that
  CI cannot exercise.
- The product risk is the face, not the platform matrix. Face quality, calibration and OBS alpha are
  the things that can invalidate the design; a second and third platform multiply the cost of every
  change to them without answering any open question earlier.

The alternative — proving parity first — spends the schedule on three capture backends and three
transport stacks before anyone has seen the face look right in OBS.

## Decision

**MVP is a Windows 11 streamer running Avatar Studio with OBS.** The user imports a GLB, calibrates
face, head region and framing, saves an `.avatar`, goes live at 1080p60, and receives a transparent
avatar in OBS over **Spout2**, with **window capture** as the documented fallback. The output gate of
[ADR-0010](0010-output-is-avatar-or-nothing.md) holds on both paths: the avatar or a blank frame,
never the camera, never a frozen face. Ten minutes over OBS plus a game must show zero dropped avatar
frames, no added stutter, and zero GPU readback while no consumer is connected.

MVP is delivered by Phases 0–4 of the task plan (T01–T51): foundation, M0 feasibility on Windows,
the M1 vertical slice, the M2 editor and quality work, then the OBS transport, alpha, onboarding and
the end-to-end gate (T46–T50, closed out by T51).

macOS/Linux parity and the three OS virtual cameras **remain 1.0 requirements**. They are sequenced
after MVP in Phase 5 (T52–T58), followed by release hardening in Phase 6 (T59–T64). The 1.0 scope,
the M0–M5 milestone definitions and their exit criteria are unchanged by this ADR; only their order
relative to the Windows MVP changes.

This is a sequencing decision, not a reversal. [ADR-0001](0001-engine-and-language.md) (Godot +
C++20 GDExtension), [ADR-0002](0002-native-camera-capture.md) (native capture on every platform) and
[ADR-0005](0005-output-transports.md) (render once, distribute many; virtual cameras as separate
processes) stay in force. Every interface added for the MVP must stay platform-neutral so Phase 5 is
an implementation, not a refactor: `ICameraSource`, `IVideoOutput`, the shared-memory frame contract
and the `.avatar` schema are not allowed to grow a Windows-only shape.

**Re-entry condition.** Phase 5 starts only when the MVP end-to-end gate (T50) passes and T51 records
no S1 gap against the MVP definition. If T50 fails, the failure is fixed first; parity work does not
start in parallel with an unproven output path. Phase 5 also needs macOS and Linux hardware on a desk
or in CI — a compile-only result is `blocked`, not passed (T52/T55/T58).

## Alternatives considered

| Option | Why not |
|---|---|
| Three-platform MVP in parallel | Triples capture, transport and installer work before the face is proven; needs three hardware sets and three OS approval flows in the first block. |
| macOS-first MVP | Smaller streamer audience, and the CMIO extension plus notarisation is the heaviest install path to validate first. |
| Virtual-camera-first MVP | Windows virtual camera needs administrator rights and `HKLM` registration; the install funnel, not the effect, would dominate early feedback. |
| OBS plugin instead of Spout2/window capture | A plugin is a second product to build, sign and support; Spout2 already exists and window capture always works. |
| Delay the platform decision until after M0 | M0 already needs a capture backend; choosing Windows then is the same decision made later, with the same cost. |

## Consequences

**Positive**

- One capture backend, one transport and one installer in the critical path until the effect is
  proven in the tool streamers already use.
- The MVP gate (T50) measures the real product: OBS plus a game on one machine, not a synthetic
  three-platform matrix.
- Face-quality iterations stay cheap; a change is validated on one platform before it is ported.
- Windows CI already exists (T06), so the MVP path is covered by the existing matrix.

**Negative**

- macOS and Linux users get nothing until Phase 5; the 1.0 promise is kept, the date is not.
- OBS becomes a hard dependency for MVP output. Window capture carries no alpha, so a streamer
  without the Spout2 plugin gets an opaque avatar or an install step.
- Calls (Discord/Zoom/Teams) are not an MVP capability: they need the OS virtual cameras from
  Phase 5. MVP is a streaming product, not a camera product.
- Windows-specific assumptions can leak into shared interfaces. The mitigation is a review rule on
  T46/T47: a Windows type in a shared header is a defect.
- M0's "native capture on all three platforms" deliverable is met on Windows only during Phase 1.
  The macOS/Linux capture backends move to T52/T55; the M0 exit criteria are unchanged for 1.0 and
  are re-run on the parity platforms before release.

**Neutral**

- The milestone names (M0–M5) and their exit criteria are untouched; the phase plan adds the MVP
  boundary between M3's Windows subset and M3/M4 parity.
- Nothing is removed from 1.0. "Deferred" still means deferred with a re-entry condition, and this
  ADR's re-entry condition is the MVP gate, not a date.

## Follow-ups

1. T46: keep `IVideoOutput`, `OutputFrame` and the readback queue free of Windows types; the Spout2
   backend is one implementation behind the interface.
2. T52/T55: implement macOS and Linux capture against the T10 `ICameraSource` contract without
   changing the ring's ownership or timestamp rules.
3. T51: record the MVP gate result and any S1 gap; that record is the re-entry evidence for Phase 5.
4. T58: run the platform integration matrix on real hardware; a compile-only result is blocked.
5. Revisit this ADR if the Spout2 plugin requirement turns out to exclude a large share of OBS users;
   the fallback is an in-app OBS plugin or a signed Windows virtual camera pulled forward from T57.
