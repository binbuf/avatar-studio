# Testing and Quality

**Status:** proposal · **Last updated:** 2026-11-20

An avatar app fails in ways unit tests cannot see: the face slides at 20° of yaw, the colour is
slightly wrong, the stream stutters every few seconds. This document defines what is measured, how it
is measured without a human in front of a camera, and what blocks a release.

---

## Reference hardware

Quality claims need a baseline. Three tiers, all x86-64 unless stated:

| Tier | Windows | macOS | Linux |
|---|---|---|---|
| **Minimum** | 4-core CPU, integrated GPU (Intel Iris Xe), 8 GB | M1, 8 GB | 4-core CPU, Intel UHD, 8 GB |
| **Reference** | 6-core CPU, RTX 3060-class, 16 GB | M1 Pro, 16 GB | 6-core CPU, RX 6600, 16 GB |
| **Low-end probe** | 2-core, 4 GB, 720p output | — | — |

All performance gates are defined on **Reference**. Minimum must be *usable* (30 fps output
acceptable, tracking may drop to 15 Hz); low-end probe exists to catch regressions that make the app
unusable, not to set targets.

---

## Test pyramid

| Layer | What | Runs | Blocking |
|---|---|---|---|
| Unit | Homography, filters, schema/migration, ring buffers, config | Every commit, < 30 s | Yes |
| Component | Capture → ring → tracker → rig with synthetic input | Every commit, < 2 min | Yes |
| Golden image | Rendered frames vs. stored references, perceptual diff | Every commit on one OS, all OS on release | Yes (threshold) |
| Integration | Real transport ↔ real consumer on each OS | Nightly | Yes for release |
| Performance | Latency, frame time, memory, dropped frames | Nightly + release | Yes for release |
| Manual QA | Subjective quality, usability, edge hardware | Per release | Yes |

---

## Determinism: the enabling decision

Nothing in the pipeline may read wall-clock time or uninitialised memory. All timing comes from
`FrameClock`, which is injectable: tests drive it manually. All randomness (there is almost none) is
seeded. All inputs are either generated or replayed.

This is what makes the following possible without a camera, a person, or a GPU with a specific
driver.

### Synthetic sources

| Source | Produces | Used for |
|---|---|---|
| `TestPatternSource` | Colour bars, timestamp burn-in, alpha checkerboard, moving marker | Capture, transport, alpha correctness |
| `SyntheticFaceSource` | A rendered face image warped along a scripted 6-DOF path, with ground-truth pose | Rig accuracy, decal stability, regression |
| `ReplaySource` | `.y4m` clip + sidecar pose/landmark track | Tracker and full-pipeline replay |
| `ScriptedTracker` | Canned `TrackingResult` sequences (including dropouts, jitter, outliers) | Rig and filter behaviour without inference |

`SyntheticFaceSource` is the centrepiece: it renders a face with known geometry, moves it on a
known path, and lets a test assert that the estimated pose matches ground truth within tolerance and
that a canonical point lands on the expected pixel. A regression in the homography or the filter
shows up as a numeric failure, not as "it looked weird".

---

## Golden-image tests

- Reference scenes: three avatars (flat-faced, spherical, high-curvature), three camera motions
  (still, yaw sweep, fast nod), two lighting setups.
- Comparison: per-pixel with a perceptual metric (SSIM ≥ 0.995, plus a max-delta guard on the decal
  bounding box) rather than exact equality, because GPU rasterisation differs across vendors.
- Failures attach the reference, the actual, and a difference image to the CI artifact.
- Reference images are regenerated only with a reviewed commit that explains the visual change.

Golden tests catch the class of bug that is invisible in code review: a shader uniform bound to the
wrong space, alpha premultiplied twice, colour space regression.

---

## Performance harness

| Metric | Method | Gate (Reference) |
|---|---|---|
| App-side latency | Timestamp at capture callback, timestamp at transport submit, same frame | p50 ≤ 70 ms, p95 ≤ 100 ms |
| Frame time | Render thread, 10-minute run | p99 ≤ 16.6 ms |
| Dropped output frames | Transport counters | 0 per 10 min |
| Dropped tracking frames | Tracker counters | ≤ 2% |
| CPU usage | OS counters, idle vs live | ≤ 25% of one core at 1080p60 (excl. tracking), ≤ 60% incl. |
| GPU usage | Vendor counters where available | ≤ 50% on Reference |
| Memory | RSS after 30 min | Stable ±5%, no growth |
| Startup | Process start → first live frame | ≤ 4 s |

Runs are scripted (`tests/performance/run_*.ps1|.sh`) so that results are comparable across machines,
and results are stored as JSON with hardware fingerprints. A regression beyond 10% on any metric
fails the nightly.

---

## Integration tests per platform

| Test | Windows | macOS | Linux |
|---|---|---|---|
| Spout2 sender → Spout receiver sample | ✔ | — | — |
| Syphon server → Syphon client sample | — | ✔ | — |
| PipeWire node → `gst-launch pipewiresrc` | — | — | ✔ |
| Virtual camera → `ffmpeg -f dshow` / AVFoundation / V4L2 | ✔ | ✔ | ✔ |
| Virtual camera → browser `getUserMedia` | ✔ | ✔ | ✔ (PipeWire camera) |
| Alpha correctness (checkerboard) | ✔ | ✔ | ✔ (where supported) |
| Install/uninstall leaves no residue | ✔ | ✔ | ✔ |
| Crash → device recovers or repairs | ✔ | ✔ | n/a |

The "virtual camera → consumer" tests are the ones that catch integration rot in OS updates, which
is the most common long-term maintenance failure for this class of app.

---

## CI matrix

| Job | OS | Trigger | Notes |
|---|---|---|---|
| Unit + component | Win, macOS, Linux | Every push | No GPU required (software renderer for shader compile checks) |
| Golden images | Win (D3D12), macOS (Metal), Linux (Vulkan/lavapipe) | Every push | Lavapipe for Linux to keep runners GPU-free |
| Native build (MediaPipe) | All three | Nightly + on tracker changes | Slowest job; cached aggressively |
| Integration (transports) | All three | Nightly | Uses virtual devices; no physical camera |
| Performance | Reference hardware (self-hosted) | Nightly | Not on shared cloud runners |
| Fuzzing | Linux | Nightly | glTF + JSON loaders |
| Licence audit | Any | Every push | Fails on manifest drift |

A green build requires all blocking jobs. Performance and integration are blocking for release
branches, advisory on feature branches (with a warning comment).

---

## Manual QA checklist (per release)

Quality is subjective; the checklist makes it comparable.

1. **Face quality**: yaw ±30°, pitch ±20°, roll ±20°. No sliding, no floating, no visible seam.
2. **Fast motion**: shake head; no rubber-banding, no overshoot.
3. **Blink and speech**: eyes and mouth move naturally; no lag beyond one frame.
4. **Lighting**: dim room, window backlight, coloured LED. Face remains plausible; no auto-WB pops.
5. **Long session**: 30 minutes live; memory stable; no drift in calibration.
6. **Recovery**: unplug camera, replug; kill the consumer app; sleep/wake the machine.
7. **Every avatar archetype**: flat, spherical, high-curvature, stylised.
8. **Every transport** on its platform, with alpha and without.
9. **Accessibility pass**: keyboard-only, 200% UI scale, screen reader announces primary controls.
10. **Uninstall**: no leftover device, no leftover files outside the documented list.

Results are recorded per build; a failing item is a release blocker unless explicitly waived with a
written reason.

---

## Bug triage

| Severity | Definition | Response |
|---|---|---|
| S1 | Data loss, crash on launch, security issue, virtual camera that cannot be removed | Hotfix |
| S2 | Face unusable (slides, invisible, wrong colour), stream unusable, no recovery | Next release, blocking |
| S3 | Visible artifact in a specific model/lighting, workaround exists | Scheduled |
| S4 | Cosmetic, copy, minor UX | Backlog |

Every bug report template asks for the diagnostics archive, which is why the diagnostics design
([`security-privacy.md`](security-privacy.md#logging-and-diagnostics)) matters to QA as much as to
privacy.

---

## Definition of done (feature)

A feature is done when:

1. It has unit or component tests covering its contract.
2. It has at least one integration or golden test if it touches rendering or output.
3. Its failure modes are implemented and have user-facing copy.
4. It appears in the diagnostics/log output with a stable code.
5. Its documentation in `docs/design/` reflects what was built, including deviations.
6. It does not regress any performance gate.

Point 5 is the one that is usually skipped. It is the one that keeps this document set honest.
