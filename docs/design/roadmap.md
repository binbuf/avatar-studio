# Roadmap

**Status:** proposal · **Last updated:** 2026-11-20

Six milestones. Each has an exit criterion that is a measurement, not a feeling, and each retires a
specific risk. The ordering is chosen so that the two things that could invalidate the whole design
(tracker viability, face quality) are answered before any product surface is built.

---

## Sequence

```
M0 Feasibility ──▶ M1 Vertical slice ──▶ M2 Quality + editor ──▶ M3 OBS transports ──▶ M4 Virtual cameras ──▶ M5 Release
   (can we?)          (does it work?)        (is it good?)            (does it reach OBS?)    (does it reach apps?)   (can we ship?)
```

M3 and M4 are independent and can run in parallel if there are two systems engineers. M2 is the
long pole and the only milestone where schedule slip is expected.

---

## MVP definition and sequencing

**MVP** is a Windows 11 streamer using OBS: import a GLB, calibrate face, head region and framing,
save an `.avatar`, go live at 1080p60, and receive a transparent avatar over **Spout2**, with
**window capture** as the documented fallback. The output is the avatar or a blank frame — never the
camera, never a frozen face — and ten minutes over OBS plus gameplay must hold zero dropped avatar
frames, no added stutter, and zero GPU readback while no consumer is connected.

The MVP is delivered by Phases 0–4 of the task plan (T01–T51): foundation, Windows M0, the M1
vertical slice, M2 quality and editor, then the OBS transport, alpha and onboarding work with the
end-to-end gate at T46–T50, closed out by T51. macOS/Linux parity and the three OS virtual cameras
remain 1.0 requirements and follow the MVP in Phase 5 (T52–T58); release hardening is Phase 6
(T59–T64). [ADR-0017](adr/0017-windows-first-mvp-sequencing.md) records the decision, its
consequences, and the re-entry condition: Phase 5 starts only when the MVP gate (T50) passes and T51
records no S1 gap, and only with real macOS and Linux hardware available.

This subsection sequences the milestones; it does not change the 1.0 scope, the M0–M5 definitions
below, or any exit criterion. Row-by-row implementation status is in
[implementation-map.md](implementation-map.md).

---

## M0 — Feasibility spike

**Question:** can the tracker and capture stack actually run on all three platforms at acceptable
latency?

**Deliverables**

- MediaPipe Face Landmarker built as a shared library behind the five-function C ABI, on Windows
  (MSVC), macOS (universal), Linux (x86-64).
- Native capture on all three platforms producing timestamped frames.
- A headless harness: frames in, `TrackingResult` out, latency histogram written as JSON.
  Implemented in T13 as `avatar_m0` with a 2-minute CI mode and a 30-minute soak; the M0
  thresholds are applied by `tests/gates/T14.ps1`.
- Numbers on Reference and Minimum hardware, at 256² and 192² input.

**Exit criteria**

| Criterion | Threshold |
|---|---|
| Inference p95 | ≤ 15 ms on Reference, ≤ 30 ms on Minimum |
| Tracker runs 30 min without leak or drift | Required |
| All three platforms build in CI from a clean checkout | Required |
| Pose is stable enough to drive rotation without visible jitter | Subjective, recorded clip reviewed |

**Retires:** the largest risk in the project. If M0 fails, [ADR-0003](adr/0003-mediapipe-as-tracking-backend.md)
is superseded by the ONNX Runtime fallback before any UI work begins. That is a planned pivot, not a
crisis.

**Explicitly not in M0:** rendering, UI, output.

**T14 gate state:** local Windows numeric evidence supports retaining MediaPipe, but M0 is
still open. The test host exceeds Reference hardware, hosted CI has not established the
three-platform real-backend build requirement, and human pose approval is outstanding.
[ADR-0024](adr/0024-m0-windows-evidence-and-open-gate.md) records why a local numeric pass is
not a milestone pass. The Phase 5 platform schedule does not waive this exit criterion;
resolve the evidence gap or explicitly replan the gate before rendering starts.

---

## M1 — Vertical slice

**Question:** does the core effect read as convincing at all?

**Deliverables**

- GLB import at runtime; one hard-coded avatar.
- Face frame estimation (homography), head-region rotation (cut mode), `flat` decal mode.
- Framing (model/head fit, zoom, offset) and the asymmetric-marker mirroring test.
- Output gate: the avatar or a blank frame, with the no-passthrough test.
- Timestamp-paired camera ring.
- 60 fps render into a `SubViewport`, preview window, window-capture output.
- Synthetic face source, the first golden-image test, and the first diagnostic codes.

**Exit criteria**

| Criterion | Threshold |
|---|---|
| Face stays attached at ±20° yaw | Blind review of 3 clips, 5 reviewers, no "sliding" reports |
| Head-region rotation | The body does not move across ±30° yaw; the neck does not gap |
| Framing stable | Avatar stays inside the viewport across ±30° yaw, ±20° pitch/roll; no drift |
| Blank on loss | Blank within 150 ms; no camera pixels in the output |
| 1080p60 sustained | p99 frame time ≤ 16.6 ms on Reference |
| Frame pacing | p99 interval ≤ 16.6 ms on 60/120/144 Hz displays, no bursts |
| App-side latency | p50 ≤ 70 ms |
| Setup with a prepared model | ≤ 10 min with a developer present |

**Retires:** "is the effect achievable with our math", "does timestamp pairing fix swimming".

---

## M2 — Quality and editor

**Question:** is it good enough that a non-technical user can achieve the result alone?

**Deliverables**

- `surface` and `shaded surface` modes; region anchors; colour matching.
- Full editor: face calibration gizmo, head-region gizmo (cut/node), blink test, yaw sweep, framing,
  props, anchors, appearance.
- Face matte: chroma detection and software segmentation, with the fallback path.
- Blank-state UX: preview says why, output blanks, no frozen face.
- `.avatar` package load/save/validate/migrate; named FaceProfiles; managed library; outfits; shows.
- Four environment presets with key-light control; three quality presets.
- Three CC0 example avatars (flat, spherical, high-curvature) with provenance manifests.
- Golden images across the three archetypes, three lighting setups, and framing/mirroring cases.
- Real-model corpus: unmodified downloaded GLBs import and calibrate without a re-export.

**Exit criteria**

| Criterion | Threshold |
|---|---|
| New user, own model, unassisted | 5 of 5 participants live within 5 minutes |
| Face quality at ±30° yaw | No "floating sticker" in blind A/B against a reference clip |
| Colour mismatch | Reviewers cannot identify the face as a separate photograph in 4 of 5 scenes |
| Profile reuse | One profile, three avatars, no recalibration; quality unchanged |
| Framing | No clipping or drift across model/head fit and zoom extremes |
| Library | 20 imports, duplicate id, delete; originals untouched |
| Existing-model corpus | 10 typical downloaded GLBs import; 8 calibrate within 10 minutes without re-export |
| Matte | No coloured fringe on green or blue; the software path adds ≤ 2 ms and degrades gracefully |
| Package round-trip | 100 fixture packages load, save, reload identically |
| No regression | All M1 criteria still met |

**Retires:** "does it look right to someone who did not build it", "can users calibrate it".

This is the milestone where the product either exists or does not. Schedule buffer belongs here.

---

## M3 — OBS transports

**Question:** can a streamer actually put this over gameplay, with alpha?

**Deliverables**

- Spout2 (Windows), Syphon (macOS), PipeWire (Linux) backends behind `IVideoOutput`.
- Async readback, per-transport drop-oldest queues, idle suppression.
- Alpha modes + checkerboard/fringe test patterns.
- Transport access control: DACL, app-group entitlement, session nonce, versioned header.
- OBS onboarding card and window-capture fallback.
- Transport status in Live mode; consumer connect/disconnect handling.

**Exit criteria**

| Criterion | Threshold |
|---|---|
| Alpha correct in OBS on all three platforms | No black halo or bright fringe on the test card |
| Dropped frames | 0 over 10 minutes at 1080p60 |
| Latency added by transport | ≤ 4 ms |
| No passthrough | The output gate holds on every transport; a forced loss yields a blank frame |
| Idle cost with no consumer | GPU readback count = 0 |

**Retires:** "can we deliver frames without a second render", "is CPU readback fast enough".

**Non-goal in M3:** zero-copy GPU sharing. It is a research item with its own spike, and shipping it
late must not block the milestone.

---

## M4 — Virtual cameras

**Question:** can a user join a call with the avatar and no OBS?

**Deliverables**

- Windows: media source COM DLL, `HKLM` registration, shared-memory ring, sync object, no-signal
  frame, installer integration, repair flow.
- macOS: CMIOExtension target, app-group Mach service, XPC + `IOSurface` transport, install/approve
  flow, removal action, notarisation.
- Linux: PipeWire `Video/Source` node with `media.role=Camera`; `v4l2loopback` fallback.
- Cross-process frame transport with a versioned shared-memory header and access control.
- Single-instance lock and activation; a second instance cannot register the same device.
- Installer manifest and uninstall-residue test.
- Suspend/resume and session-lock handling.

**Exit criteria**

| Criterion | Threshold |
|---|---|
| Visible and working in Discord, Zoom, Teams, Chrome | 4 of 4 on each platform |
| Install → first frame | ≤ 60 s including OS approval |
| Crash recovery | Device reappears or is repairable within one app restart |
| Second instance | Activates the running window; no device or ring conflict |
| Suspend / lock | No-signal while suspended or locked; capture resumes on wake/unlock |
| Uninstall | No device, no files, no registry residue (Windows), extension removed (macOS) |
| No-signal behaviour | Consumers show a placeholder, never hang or black-screen |

**Retires:** "does the cross-process design hold", "can we survive two very different OS extension
models".

---

## M5 — Release hardening

**Question:** can we ship this to strangers?

**Deliverables**

- Signed installers/packages, notarised macOS build, reproducible CI.
- Licence audit complete; `NOTICE` generated; asset manifests verified.
- Security review against [`security-privacy.md`](security-privacy.md); fuzzing and sanitizers green
  for 14 days.
- Docs: user guide, modelling guide, troubleshooting, privacy page.
- Accessibility pass; i18n pipeline with one pseudo-locale.
- Manual QA checklist executed on all three tiers; Windows 10 best-effort smoke if hardware exists.

**Exit criteria**

| Criterion | Threshold |
|---|---|
| Crash-free sessions | ≥ 99.5% over a 2-week beta |
| S1/S2 bugs open | 0 |
| Perf gates | All met on Reference, usable on Minimum |
| Clean-machine install | Works with no dev tools, no Visual C++ redistributable prompts, no Python |

---

## Consolidated open questions

Each is owned by a milestone and has a decision point. None may drift past its milestone.

| # | Question | Owner | If wrong |
|---|---|---|---|
| Q1 | Is MediaPipe viable on all three platforms? | M0 | Supersede ADR-0003 with ONNX Runtime |
| Q2 | Is planar face projection sufficient, or is cylinder unwrap needed for 1.0? | M1 | Ship both; extra editor control |
| Q3 | Does timestamp pairing eliminate sliding in practice? | M1 | Revisit capture/tracking rate strategy |
| Q4 | Can users calibrate without help? | M2 | More automation, fewer knobs |
| Q5 | Is 30 Hz face content acceptable next to 60 Hz rotation? | M2 | Prefer 720p60 capture; consider landmark warp |
| Q6 | Does colour matching work across skin tones and lighting? | M2 | Manual gain as primary, auto as suggestion |
| Q7 | Is CPU readback fast enough on Minimum hardware? | M3 | Prioritise zero-copy spike |
| Q8 | Is the OBS PipeWire camera source reliable across versions? | M3 | Document v4l2loopback as the Linux OBS path |
| Q9 | Can Windows register the virtual camera without a reboot after a crash? | M4 | New CLSID per session, cleanup on start |
| Q10 | Does the macOS extension survive app upgrades without re-approval? | M4 | Explicit re-approval flow in-app |
| Q11 | Is `HKLM` COM registration avoidable via MSIX packaged COM? | M4 | Stay per-machine; document the prompt |
| Q12 | Does lens distortion visibly hurt the face at close range? | M2 | Accept and document; optional calibration post-1.0 |
| Q13 | Is 60 Hz output pacing stable on 120/144 Hz displays? | M1 | Timer-paced output; accept preview tearing |
| Q14 | Does the managed library stay responsive at 100+ avatars? | M2 | Rebuildable index plus lazy thumbnails |
| Q15 | How much Windows 10 parity do we actually get without CI? | M5 | Best-effort policy; community reports |
| Q16 | Does the neck-cut deformation hold at ±30° on one-piece models, or does it need a mesh split? | M1 | Encourage a separate head node; add a split helper |
| Q17 | Is the software matte good enough without a green screen? | M2 | Recommend a green screen; keep chroma as the quality path |
| Q18 | Does blanking on feature loss flicker in normal use? | M2 | Widen the hold or add per-feature hysteresis |

---

## Risk register (release view)

| Risk | Trigger to act | Mitigation already in the design |
|---|---|---|
| Tracker not viable | M0 numbers miss by > 2× | C-ABI seam, ONNX fallback planned |
| Face quality plateau | M2 blind review fails twice | Surface+shading modes, per-model calibration, modelling guide |
| Neck-cut artifact | Visible stretch or gap at ±30° on one-piece models | Tune the falloff band; encourage a separate head node; Q16 |
| Matte flicker or fringe | Green edge or a pulsing silhouette | Temporal smoothing, chroma path, region-mask fallback; Q17 |
| Blanking looks broken | Users read a blank frame as a crash | Preview states the reason; 150 ms hold; panic button; Q18 |
| Perf on low-end GPUs | M3 gates fail on Minimum | Quality presets, 720p output, reduced tracking rate |
| macOS approval friction | > 30% of beta users fail to install | In-app guided flow, detection + repair, docs |
| Windows installer friction | > 20% abandon at admin prompt | Portable build without virtual camera; OBS fallback |
| OS API churn | Any platform breaks in a point release | Integration tests per platform, nightly |
| Windows 10 best-effort | A supported platform has no blocking coverage | Smoke test when hardware is available; community reports; document the policy |
| Scope creep | Any milestone adds a non-goal | Non-goals are explicit; changes require an ADR |

---

## Post-1.0 candidates

Not scheduled; listed so that they are not smuggled into 1.0.

1. Zero-copy GPU transport (per-platform spikes).
2. Auto-update with signed deltas.
3. Rigged characters / blendshape-driven expressions (the tracker already provides 52 coefficients).
4. Fitted clothing.
5. Multi-face / guest avatars.
6. VRM import.
7. HDR output.
8. Community avatar library (would introduce the first network feature, and with it a review burden).
9. Full 6-DOF framing or a damped translation gain.
10. Preloaded avatar switching while live.
11. Optional per-camera lens distortion calibration.
12. ARM64 Windows/Linux builds.
13. Encrypted FaceProfile export/import.

Each requires a new ADR before work starts.
