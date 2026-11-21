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

OS matrix: Windows 11 22H2+ is the only blocking Windows platform; Windows 10 22H2+ is best-effort
and not in CI. macOS CI runs the current and previous major on a 13+ floor. Linux CI runs Ubuntu
24.04 LTS and Fedora current, on X11 and Wayland. The policy is in
[`architecture.md`](architecture.md#supported-platforms).

---

## Test pyramid

| Layer | What | Runs | Blocking |
|---|---|---|---|
| Unit | Homography, filters, schema/migration, ring buffers, config | Every commit, < 30 s | Yes |
| Shell smoke | Godot headless: project settings, transparent 1920x1080 SubViewport, GDExtension class load, C++ frame counter | Every commit, < 5 s | Yes |
| Static analysis / sanitizers | clang-tidy, ASan/UBSan/TSan, warning policy | Every commit | Yes |
| Component | Capture → ring → tracker → rig with synthetic input | Every commit, < 2 min | Yes |
| Golden image | Rendered frames vs. stored references, perceptual diff | Every commit on one OS, all OS on release | Yes (threshold) |
| Integration | Real transport ↔ real consumer on each OS | Nightly | Yes for release |
| Performance | Latency, frame time, memory, dropped frames | Nightly + release | Yes for release |
| Manual QA | Subjective quality, usability, edge hardware | Per release | Yes |

### Headless shell smoke

`app/tests/smoke.gd` runs under `godot --headless` and is part of the verification contract from T02.
It asserts the Forward+ project settings, the fixed 1920x1080 transparent `SubViewport` with
`render_target_update_mode = ALWAYS`, that the pinned GDExtension loaded and registered
`AvatarRuntime`, that `get_version()` is non-empty, and that the C++ frame counter advances. It exits
non-zero on any failure. No GPU is required; golden rendering is a later layer.

`scripts/verify.*` runs the smoke test for `-Task T02` and later and fails if the engine or adapter
is missing. `-Task T01` remains native-only so the framework-free core is testable without Godot.
The 4.7.2 headless editor import crashes on shutdown after writing `.godot/extension_list.cfg`, so
the verify script seeds that one-line list directly; the smoke test is the authoritative load check.

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
  (still, yaw sweep, fast nod), two lighting setups, and one asymmetric marker (a dot on the
  subject's right cheek) to catch left/right and mirroring regressions.
- Framing cases: model fit, head fit, zoom/offset extremes; assert the avatar stays inside the
  viewport and the decal stays attached.
- Head-region cases: `cut` and `node` modes at ±30° yaw and ±20° pitch/roll; assert the body does not
  move, the neck blends without a gap, and the decal stays attached.
- Blank state: no face, missing mouth, and tracker timeout each produce a fully blank frame.
- One barrel-distorted source, to quantify the accepted lens error.
- Comparison: per-pixel with a perceptual metric (SSIM ≥ 0.995, plus a max-delta guard on the decal
  bounding box) rather than exact equality, because GPU rasterisation differs across vendors.
- Failures attach the reference, the actual, and a difference image to the CI artifact.
- Reference images are regenerated only with a reviewed commit that explains the visual change.

Golden tests catch the class of bug that is invisible in code review: a shader uniform bound to the
wrong space, alpha premultiplied twice, colour space regression.

---

## Output invariant

The rule is tested, not trusted: no camera pixel may reach the output except through the decal mask.

- Feed a high-contrast synthetic camera pattern (a face, a checkerboard, and a green screen) and a
  model whose region masks are known. Diff the output against the same scene with a black camera
  texture; every changed pixel must lie inside the union of the region masks, within one pixel of the
  feather.
- Set `features_valid = false` by dropping the mouth landmarks and assert the output is fully blank
  within 150 ms, with no frame containing the avatar or the camera.
- Force a tracker timeout and assert the same.
- Run the check on window output, Spout/Syphon/PipeWire, and the virtual camera. The gate lives in
  the output manager, so a transport that reintroduces a frame is a blocking bug.

## Matte tests

- Synthetic green and blue screens: the key succeeds and the decal edge has no coloured fringe.
- No-green background: the software matte reduces background leakage; assert the region edge stays
  below an agreed background-pixel share.
- Matte unavailable: fall back to the region mask and log `TRK-009` once.
- Spill: a green-lit cheek does not turn green after keying.
- Performance: the matte adds no more than 2 ms at 1080p on Reference.

---

## Real-model corpus

Synthetic faces prove the math; real models prove the claim that no special model is needed. The
corpus is a set of unmodified, downloaded glTF characters: an anime bust, a realistic bust, a
stylised animal, a rigged T-pose character, a painted-face model, a model whose origin is at the
feet, and one with glasses.

For each model the test imports it, runs auto-placement, and asserts that calibration converges, or
that the warning names the specific incompatibility. A model in the corpus is never re-exported or
edited; that would hide the problem the corpus exists to catch.

It is a manual M2 gate. Once the fixtures are licensed for internal use it also becomes a nightly
smoke, with the pass condition recorded per model rather than as a single boolean.

---

## Performance harness

| Metric | Method | Gate (Reference) |
|---|---|---|
| App-side latency | Timestamp at capture callback, timestamp at transport submit, same frame | p50 ≤ 70 ms, p95 ≤ 100 ms |
| Frame time | Render thread, 10-minute run | p99 ≤ 16.6 ms |
| Frame interval jitter | Render timestamps, 10-minute run | p99 ≤ 16.6 ms, no burst > 2 frames |
| Frame pacing across displays | Manual + scripted on 60/120/144 Hz | 60 fps output, no cumulative drift |
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
| Library import/delete round-trip, original untouched | ✔ | ✔ | ✔ |
| Profile create, migrate, corrupt fallback | ✔ | ✔ | ✔ |
| Session lock stops capture and output | ✔ | ✔ | ✔ |
| Second instance activates the first | ✔ | ✔ | ✔ |
| Foreign process cannot open the shared-memory ring | ✔ | ✔ | n/a (PipeWire policy) |

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
| ASan + UBSan | Linux, macOS | Every push | Unit/component only; no GPU |
| TSan | Linux | Every push | Ring buffers, tracking handoff, output queues |
| Static analysis | Any | Every push | clang-tidy plus the warning policy |
| Fuzzing | Linux | Nightly | glTF, JSON, shared-memory header, archive paths |
| Licence audit | Any | Every push | Fails on manifest drift |

A green build requires all blocking jobs. Performance and integration are blocking for release
branches, advisory on feature branches (with a warning comment).

---

## Sanitizers, static analysis, and fuzzing

The data plane is threaded C++ with a binary cross-process contract, so "it passed on my machine" is
not enough.

| Tool | Scope | Gate |
|---|---|---|
| ASan + UBSan | Unit and component tests on Linux/macOS | Every push; a finding is a release blocker |
| TSan | Ring buffers, tracking handoff, output queues | Every push |
| MSVC ASan | Windows unit tests | Nightly; advisory until the toolchain is stable |
| clang-tidy | `native/` with a pinned config | Every push on changed files; new findings fail |
| Warning policy | `-Wall -Wextra -Werror`, MSVC `/W4 /WX` | Every build |
| libFuzzer | glTF importer, JSON loader, shared-memory header parser, archive path handling | Nightly, 30 min per target |

### ThreadSanitizer job (T06)

The ring and latest-slot contracts are only credible under TSan. T06 adds a Linux job
(clang or GCC; MSVC has no equivalent):

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan --parallel
ctest --test-dir build-tsan -C RelWithDebInfo -L fast --output-on-failure
```

`avatar_core_unit` contains the two-thread 1,000,000-item `SpscRing` stress test and the
concurrent `LatestSlot` torn-read test, so the `fast` label is the whole TSan surface for the
primitives. The same job also covers the tracking handoff and output queues once those land. T03
ran the stress test locally on Windows; the Linux TSan job is the blocking gate from T06.

The shared-memory header is a versioned binary interface shared with a process we do not control. It
gets the same fuzzing treatment as a file parser, including truncated, oversized, and
version-mismatched inputs. A crash there is a `VCAM-005`/`SEC-004` class bug, not a cosmetic one.

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
11. **Framing**: model fit, head fit, zoom/offset extremes, mirror output; avatar never clips or drifts.
12. **Profiles**: create two, switch, delete, corrupt one; no cross-contamination.
13. **Library**: import, duplicate id, delete; the original is untouched.
14. **Suspend and lock**: sleep/wake, lock/unlock, minimise/restore; capture stops on lock and resumes.
15. **OBS onboarding**: plugin absent → setup card; window capture fallback works.
16. **Windows 10 smoke** (if hardware is available): capture, preview, OBS output; no native camera.
17. **Existing model**: import a downloaded glTF, auto-place, check the model-check panel and head region.
18. **Head region**: cut and node modes; the body stays still at ±30° yaw, the neck does not gap.
19. **Blank output**: cover the mouth, look away, unplug the camera; output blanks and never shows a frozen face.
20. **Matte**: green screen and no green screen; no fringe; software fallback logs `TRK-009`.
21. **No passthrough**: screen-record the output while the preview shows the camera overlay; no camera pixels appear in the output.

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
4. Its failure modes have registered codes in [`diagnostics.md`](diagnostics.md) and appear in the log.
5. Its documentation in `docs/design/` reflects what was built, including deviations.
6. It does not regress any performance gate.

Point 5 is the one that is usually skipped. It is the one that keeps this document set honest.
