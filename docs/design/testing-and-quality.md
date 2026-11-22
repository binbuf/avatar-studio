# Testing and Quality

**Status:** proposal; the deterministic source set, scripted tracker, replay fixtures, golden
comparator, foundation performance harness (T05), the CI/format/licence gates (T06), the tracker
C ABI contract harness (T09), the real Windows tracker contracts (T11), the landmark-map/profile
tests (T12), the headless M0 harness with its 30-minute soak (T13), the Windows-scoped M0 contract
audit (T14/T15), the one-viewport runtime with 60 Hz pacing (T16), the runtime GLB import with
its fixture set (T17), FaceRig numerical/CPU gates (T18), head-region cut/node geometry and goldens
(T19), and the flat decal pass with its analytic-reference goldens (T20) are implemented. The
10-minute app soak and the central golden-image suite remain `Planned`. ·
**Last updated:** 2026-11-22
**Decision record:** [ADR-0014](adr/0014-deterministic-sources-and-golden-comparator.md),
[ADR-0015](adr/0015-performance-harness-and-local-baseline.md),
[ADR-0016](adr/0016-ci-quality-gates.md),
[ADR-0023](adr/0023-m0-headless-harness-and-thresholds.md),
[ADR-0025](adr/0025-m0-windows-scope-and-deferred-platform-evidence.md)

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
| Shell smoke | Godot headless: project settings, transparent 1920x1080 SubViewport, GDExtension class load, C++ frame counter; T16 runtime config and a 10-second paced run; T17 GLB import, fixtures and app-scene wiring | Every commit, < 15 s | Yes |
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

### Runtime viewport and pacing (T16)

The Windows runtime holds a scoped 1 ms timer-resolution request, including in
headless verification, so deadline sleeps do not round to the default ~15.6 ms tick.
The request is released when the runtime is destroyed. Pacing thresholds are unchanged.

`app/tests/runtime.gd` is the headless runtime test, run by `scripts/smoke.ps1 -Runtime` for T16 and
later tasks. It asserts the realised configuration rather than the design text:

- one 1920x1080 transparent `SubViewport` with `render_target_update_mode = ALWAYS`;
- the preview `TextureRect` is a `ViewportTexture` with the same RID as the viewport, the scene
  owns exactly one `SubViewport`, the preview render count is zero, and the avatar viewport render
  count advances;
- the fixed `Presentation` transform is a 180° yaw;
- `Engine.max_fps = 60`, `OS.low_processor_usage_mode = false`, Forward+ and an explicit pacing mode;
- the C++ HUD is hidden by default, toggles, and carries frame time, interval, drops, viewport size,
  renderer and pacing mode;
- a connected consumer keeps the runtime processing while the tree is paused (the hidden-window
  proxy) and App Nap is reported disabled;
- a 10-second paced run: frame-time p99 ≤ 16.6 ms, interval-jitter p99 ≤ 16.6 ms, no interval
  longer than two missed frames, and no catch-up burst longer than two frames; the raw interval
  p99 is reported for diagnosis, not gated (16.667 ms is the cadence, not the budget).

The JSON summary is written to `.tools/T16-runtime.json`; `tests/gates/T16.ps1` re-checks it with
the static configuration and the design documents. The nominal interval is 16.667 ms, so the gate
uses frame-time p99 and the absolute deviation of the interval from the target for the 16.6 ms
limit; the raw interval p99 is reported but not gated below its own nominal value. `core::FramePacer`
is unit-tested with `ManualTimeSource` for 60/120/144/240 Hz mode selection, the absolute deadline
grid, early waits, and dropped-not-replayed slots. The windowed run on the T16 host used a 240 Hz
display (4x 60, vsync mode); the headless run used the deadline scheduler. Results are in the local
`docs/progress/T16.md` workspace.

### Runtime GLB import (T17)

`app/tests/model_import.gd` is the headless import test, run by `scripts/smoke.ps1 -Model` for T17
and later tasks. It imports `assets/models/avocado_half.glb` and the tracked fixtures under
`tests/fixtures/models/` through `AvatarModel`, then asserts:

- mesh and material counts are non-zero, the triangle count is plausible, and the node tree is
  extracted; the avocado AABB is finite, has volume and a height near 15.5 cm;
- no `AnimationPlayer`, `Skeleton3D`, skin or morph target is instantiated; an animated, skinned
  fixture renders its bind-pose AABB at the origin instead of the animated offset;
- a feet-origin fixture keeps `min.y = 0` and its authored size; an axis-variant Z-up/+Y-forward
  fixture is converted to 0.4 x 1.8 x 0.2 m, Y up and −Z forward;
- `face_submesh` reduces the decal triangle set without hiding the rest of the model, and the
  decal positions/normals/indices are internally consistent;
- an invalid GLB fails with `AVT-102`, leaves the node unloaded and creates no partial scene;
- the app scene attaches `AvatarModel` under `Presentation`, keeps the two mesh instances and their
  materials, has a current `AvatarCamera` in front of the avatar, key/fill lights, and exactly one
  `SubViewport`.

The JSON summary is written to `.tools/T17-model-import.json`; `tests/gates/T17.ps1` re-checks the
counts, the AVT-102 result, the measured import time/peak static memory, the static implementation
and the design documents. Import is one-time; the decal pass reads the persistent mesh and arrays,
not a per-frame copy. `app/tests/avatar_shot.gd` is manual windowed evidence: on the T17 host it
captured 397,552 opaque and 393,225 non-black pixels at 1920x1080, showing the avocado and pit with
their materials. It is not part of the headless verify path.

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

### Implemented sources (T04)

The sources live in `native/capture/` and `native/tracking/` and are framework-free. They are part
of the application tree, not `tests/`, because T10 and T11 implement the real backends against the
same interfaces.

| Item | Implementation |
|---|---|
| Frame ownership | `FrameRing` owns fixed slots (default 4). `ICameraSource::ring()` returns the producer's ring; a consumer holds a `FrameLease` (pin) while reading and the producer never overwrites a pinned slot. `VideoFrame::planes` point into slot storage, so there is no hidden copy. |
| Clock | Every source takes an injected `FrameClock`. `produce()` publishes at most one frame and returns false when the clock has not advanced or every slot is pinned. |
| `TestPatternSource` | 1280×720 and 1920×1080, NV12 and BGRA8. Colour bars, a marker whose rectangle is a pure function of elapsed time (`marker_rect_at`), a decimal timestamp burn-in and an alpha checkerboard. |
| `SyntheticFaceSource` | Deterministic CPU render of a canonical face through the exact plane-induced homography for a scripted 6-DOF path. Emits `camera_from_head` and 23 canonical correspondences with both pinhole (`ideal_*`) and post-distortion (`x/y`) image points. A `barrel_k1` variant applies the same radial model to the render and the ground truth. |
| `ScriptedTracker` | Six scenarios: valid tracking, dropouts, seeded jitter, outliers, missing mouth, left/right swap. `submit()` stamps each result with the input frame's timestamp. |
| `ReplaySource` | YUV4MPEG2 4:2:0/4:2:2/4:4:4 reader plus a versioned JSON sidecar. The sidecar supplies the frame timestamp and ground truth; the injected clock supplies `arrival_ns`. |

Component tests: `avatar_capture_unit` (labels `fast;task_T04`) covers the ring, both synthetic
sources, the scripted tracker, the replay fixture and the comparator library. `golden_compare_cli`
runs the comparator's `--self-test` through the real command line.

### Tracker contract harness (T09)

`avatar_tracking_unit` (labels `fast;task_T09`) is the contract suite for the tracker seam. It runs
`ScriptedTracker` through both interfaces and asserts the properties the rig depends on:

- `submit` returns without waiting for inference (10,000 frames, bounded wall time);
- `latest`/`poll_result` return only the newest result, and a second poll without a push returns
  nothing;
- `source_timestamp_ns` is the input frame's timestamp, including duplicate and non-monotonic input;
- `valid = false` and `valid = true, features_valid = false` stay distinct;
- null-safe destroy, invalid frames, ABI-version mismatch and invalid config are rejected;
- a deliberately throwing backend is converted to `AVATAR_TRACKER_ERROR_INTERNAL`; no exception
  crosses the C boundary.

`avatar_tracker_c_abi_compile` compiles `tracker_c_api.h` as C11 with `/W4 /WX` (MSVC) or
`-Wall -Wextra -Wpedantic -Werror` (GCC/Clang), and its `_Static_assert`-style checks pin the
documented offsets and sizes. The C++ translation unit includes the same header, so a field move
fails both sides.

`core::LatencyHistogram<>` is shared by `TrackerStats` and `avatar_bench`. It is a fixed-bucket
histogram (100 microsecond buckets, 102.4 ms window, exact max, saturating overflow), so a percentile
in the debug HUD and a percentile in the performance JSON use the same definition. The contract suite
covers the empty, percentile, overflow and saturating-counter cases.

### Capture suite and smoke (T10)

`avatar_camera_unit` (labels `fast;task_T10`) drives the platform-neutral `CameraSource` through a
fake `ICameraDeviceBackend`, so it runs on every platform without a camera:

- enumeration, first-device selection and the negotiated mode;
- exact preference matching, step-down after an advertised-mode miss and after a backend
  `invalid_format` rejection (`CAP-003`);
- busy and permission-denied open failures (`CAP-002`, `CAP-006`) and "no camera" (`CAP-008`);
- source-timestamp mapping to `FrameClock`, `arrival_ns` preservation, and the one-per-episode
  `CAP-005` re-anchor for backwards/duplicate timestamps;
- the observed-interval watchdog (restart once, then step down/report, `CAP-004`);
- the 2-second reconnect poll after `CAP-001`, a failed reopen (`CAP-007`), and recovery;
- a mid-stream preemption as `CAP-002` with recovery;
- suspend/resume timestamp-base reset;
- ring saturation (four pinned slots) and the callback-to-publish latency histogram.

`avatar_list_cameras_smoke` runs `avatar_list_cameras --list-cameras`, which enumerates without
opening a device and exits 0 when none is present. The manual real-camera check is
`avatar_list_cameras --capture 60 [--mode WxH@FPS] [--format nv12|yuy2|bgra8]`; it reports frame
count, ring drops, restarts, timestamp resets, monotonicity, shape and `publish_latency()`.

### Replay sidecar format

The sidecar is JSON, version 1, next to the `.y4m` (same stem by default). Width and height must
match the Y4M header. `frames` is ordered; `timestamp_ns` must be non-decreasing.

```json
{
  "version": 1,
  "width": 64,
  "height": 48,
  "color_space": "bt601_limited",
  "frames": [
    {
      "timestamp_ns": 0,
      "features_valid": true,
      "pose": [1,0,0,0, 0,1,0,0, 0,0,1,-0.6, 0,0,0,1],
      "correspondences": [
        {"name": "eye_outer_r", "u": 0.70, "v": 0.0, "x": 0.40, "y": 0.45,
         "ideal_x": 0.40, "ideal_y": 0.45}
      ]
    }
  ]
}
```

`pose` is row-major `camera_from_head`. Unknown correspondence names are ignored, matching the
runtime `LandmarkMap` rule. The checked-in fixture is `tests/fixtures/replay/tiny.{y4m,json}`; large
clips are generated with `scripts/gen_fixtures.ps1 -Large` into the gitignored
`tests/fixtures/generated/`.

### Golden comparator

`tests/tools/golden_compare` is both a library and a command:

```powershell
golden_compare --actual build/actual.ppm --baseline tests/golden/frame.ppm `
  --artifacts build/golden-artifacts --name frame
```

It compares 8-bit RGB PPM (P6) images with global SSIM over 8x8 luma windows (default minimum
0.995) and a maximum per-channel delta guard (default 8). On request it writes
`<name>.actual.ppm`, `<name>.baseline.ppm` and `<name>.diff.ppm`; the diff is the amplified absolute
difference tinted red. Exit codes are 0 for pass, 1 for a threshold failure and 2 for usage or I/O
errors, so CTest can treat it as a normal test and a later golden task can attach the three
artifacts. `--self-test <dir>` exercises the pass, fail, max-delta and artifact paths and is
registered as the `golden_compare_cli` test.

PPM is chosen because it needs no third-party decoder and is trivial to inspect; T26 can add a PNG
converter if CI viewers need one. The guard is global in T04; the decal-bounding-box region mask is
T26 work.

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

## Real tracker feasibility checks (T11)

Windows `verify -Fast -Task T11` builds the separately pinned MediaPipe SDK and runs both the
T09 scripted tests and the real DLL contracts. `avatar_real_tracker_contract` covers async input
ownership, latest-only polling, nonblocking busy drops, timestamp preservation with duplicate and
backwards input, frame/layout validation, reconfiguration, fault containment, letterbox mapping,
colour conversion and the eye/iris/mouth predicate. `avatar_tracker_model_integrity` alters the
actual bundled model and proves graph initialization rejects it. `tracker_sdk_integrity` parses the
actual PE export table and checks its five symbols plus the bundled model SHA256.

`avatar_record_tracker_clip` records a bounded, local-only BT.601 limited NV12 camera clip;
`avatar_tracker_bench` runs that clip through the same real worker and reports prepared and model
tensor sizes separately, warm-up count, p50/p95 graph inference and preprocessing, exact busy drops
and confidence bins. 192/256 refer to prepared images; the upstream landmark network remains a
fixed 256 tensor. These are feasibility observations, not a replacement for T13's 30-minute soak,
T14's M0 gate or real-corpus pose/occlusion validation. The app HUD has no real-statistics query in
ABI v1; it remains planned.

## Landmark map and profile correspondences (T12)

`avatar_tracking_unit` also carries the T12 cases under `fast;task_T12`:

- every documented `LandmarkName` resolves through both pinned tables, and unknown names
  return `nullopt` instead of a guessed index;
- the critical subject-left/right indices are pinned, and a table with duplicate indices
  reports them;
- the asymmetric-marker test places a dot at canonical `(0.62, -0.25)` on the synthetic
  face, asserts it is left of the eye midpoint and has positive canonical u, and asserts
  the same check fails with a swapped `_l`/`_r` table;
- the canonical frame maps +u toward the subject's right eye and +v toward the forehead,
  including a rolled eye line;
- profile correspondences require eight usable points, fall back to the available subset
  with `RIG-003` when a backend cannot provide a named point, report duplicate indices,
  ignore unknown names, and round-trip the named `(u,v)` array through JSON.

The tests use the deterministic synthetic face source for geometry and the real pinned
tables for resolution; they do not require a camera or the MediaPipe DLL. The T11 recorded
clip independently confirmed the viewer-left/viewer-right side of the critical indices.
Homography estimation and the post-RANSAC inlier gate are implemented in T18 below.

## M0 headless harness and 30-minute soak (T13)

`avatar_m0` (`tests/m0/m0.cpp`) is the feasibility harness: a capture source (test pattern or
an `AVCLIP1` replay) feeds either `ScriptedTracker` or the real `libavatartracker.dll` through the
T09 C ABI, and every measured result is written to a JSON report. It does not use Godot, the
renderer, the capture state machine, or a camera; M0 runs before those exist. The real DLL is
loaded dynamically, so the target never links MediaPipe. The T11 recorded 640x480 NV12 clip is the
default real source because it is deterministic and contains an actual face; the test pattern is
the synthetic fallback.

The report is `schema: avatar_m0.result.v1` and carries:

- `metrics.inference_ms`: caller-visible submit-to-poll latency (p50/p95/p99/mean/max/min), not a
  vendor-internal graph timer.
- `metrics.frame_age_ms`: poll time minus the source timestamp, i.e. the age a consumer would see.
- `metrics.dropped`: total and per-reason counts (`superseded`, `stalled`, `submit_error`), plus
  `source_missed` for capture-side misses.
- `metrics.confidence`: ten bins, samples and mean, including no-face zeros.
- `metrics.rss` and `metrics.leak`: sampled working set and handle count after warm-up, with the
  5% growth and monotonic-handle verdicts.
- `metrics.cpu`: process CPU as a fraction of one core plus a per-thread CPU snapshot.
- `metrics.pose_stability`: frame-to-frame quaternion delta and yaw/pitch/roll ranges.
- `artifacts`: the pose CSV, SVG chart and BMP review clip paths.

`tests/m0/run.ps1` has two modes. `short` is the 2-minute CI run (60 s per size, 5 s warm-up, 5 s
RSS samples) and records but does not fail on a leak trend. `full` is the 30-minute soak (15 min
per size, 60 s warm-up, 60 s RSS samples) and fails on more than 5% RSS growth after warm-up or a
monotonically growing handle count. The longer warm-up moves the baseline past TFLite/XNNPACK's
lazy first allocations, so the verdict is about the steady state, not startup. Both run 192x192 and 256x256 prepared input. Results are
written to the gitignored `tests/performance/results/`; the human summary is
`docs/progress/T13-m0-results.md` for the soak and `docs/progress/T13-m0-short.md` for the short
run. Each size also records a pose trace, an SVG chart and a short BMP clip for the T14 human
review.

The M0 thresholds are applied by `tests/gates/T14.ps1`, not by the harness: inference p95 ≤ 15 ms
(reference) / 30 ms (minimum), p99 ≤ 25/45 ms; frame age p95 ≤ 50/80 ms, p99 ≤ 100/150 ms;
dropped frames ≤ 2%; RSS growth ≤ 5%; handles not monotonic; ≥ 1800 s total and ≥ 600 s per size.
The gate requires a real tracker and a real replay source. A scripted run is labelled
`synthetic: true` and can prove the harness in T13, but it is not M0 tracker evidence. CTest
registers `avatar_m0_selftest` under `fast;task_T13`, `avatar_m0_short` under `fast;task_T13`, and
`avatar_m0_soak` under `perf;task_T13`; `verify -Fast -Task T13` runs the first two, and
`verify -Full -Task T13` adds the soak. [ADR-0023](adr/0023-m0-headless-harness-and-thresholds.md)
records the measurement definitions and the threshold split.

T15 audited the harness evidence and the contracts the renderer consumes, and added
`tests/gates/T15.ps1` as the regression check for the audit's contract and documentation claims.
The short run is the fast-suite evidence; the full soak remains local evidence because the clip and
results are gitignored. The audit's gap list and M1 readiness statement are in the local
`docs/progress/T15.md` workspace. [ADR-0025](adr/0025-m0-windows-scope-and-deferred-platform-evidence.md)
closes M0 for the Windows MVP and defers cross-platform and hardware-tier evidence.

## FaceRig numerical and local CPU gate (T18)

`avatar_rig_unit` (`fast;task_T18`) uses `SyntheticFaceSource` ground truth and its
analytic camera pose at 1280x720 measurement scale. Static snapshots cover yaw ±30°,
pitch ±10°, roll ±20°, and translation. Tolerances: static pose <0.05°, inverse
translation residual <1e-6 m, and canonical point reprojection <1 px. A slow motion
path allows <2° intentional filter lag and <1 px source-image reprojection.
Additional cases cover seeded landmark noise, rest jitter RMS ratio <0.35, translation
RMS <0.6 mm, exact region alpha/clamp, named-subset/iris fallback, two landmark
outliers, pose rejection/rebaseline, failed-solve damping, malformed/degenerate
input, duplicate/older timestamps, 50 ms prediction cap and the 150 ms hold boundary.

`avatar_rig_perf` (`perf;task_T18`) times only `FaceRig::update`, including named
joins, H estimation, region and pose updates: 120 warmup plus 1200 measured samples,
separate all-inlier and one-outlier scenarios. Each scenario asserts **mean <0.1 ms**
on this machine and consumes output to prevent optimisation from removing the work.
The T18 task gate explicitly runs both numerical and performance tests even for
`verify -Fast -Task T18`; ordinary fast CI excludes the hardware-dependent `perf`
label. This is a local budget measurement, not Reference/Minimum hardware evidence.
Exact constants, measurements and host details are recorded in the local progress
workspace; [ADR-0028](adr/0028-facerig-estimation-and-temporal-contract.md) is permanent.

## Performance harness

| Metric | Method | Gate (Reference) |
|---|---|---|
| App-side latency | Timestamp at capture callback, timestamp at transport submit, same frame | p50 ≤ 70 ms, p95 ≤ 100 ms |
| Frame time | Render thread, 10-minute run | p99 ≤ 16.6 ms |
| Frame interval jitter | Render timestamps, 10-minute run | p99 ≤ 16.6 ms, no burst > 2 frames || Frame pacing across displays | Manual + scripted on 60/120/144 Hz | 60 fps output, no cumulative drift |
| Dropped output frames | Transport counters | 0 per 10 min |
| Dropped tracking frames | Tracker counters | ≤ 2% |
| CPU usage | OS counters, idle vs live | ≤ 25% of one core at 1080p60 (excl. tracking), ≤ 60% incl. |
| GPU usage | Vendor counters where available | ≤ 50% on Reference |
| Memory | RSS after 30 min | Stable ±5%, no growth |
| Startup | Process start → first live frame | ≤ 4 s |

T16 applies the frame-time and interval-jitter rows to a 10-second runtime run (headless in the
verify path, windowed on the host display when available); T26 applies them to the 10-minute app
soak with real avatar content. The 16.6 ms limit is the render budget, not the nominal 16.667 ms
interval; interval jitter is measured as the absolute deviation from the target.

Runs are scripted so results are comparable across machines and stored as JSON
with hardware fingerprints. A regression beyond 10% on any metric fails the
nightly.

### Implemented harness (T05)

`avatar_bench` (`tests/performance/avatar_bench.cpp`) drives the deterministic
foundation pipeline: test pattern source → `FrameRing` publish → pin/acquire →
`ScriptedTracker` → bench-local rig smoothing/matrix compose. It is synchronous;
the real render thread, capture callbacks and transport queues are T10/T11/T26/T46,
so `end_to_end_latency_ms` is `null` with a reason and `frame_time_ms` equals
`app_latency_ms` until those threads exist. This is deliberate: the harness
measures the pipeline, not a synthetic loop.

| Scenario | Stages |
|---|---|
| `capture` | source render + ring publish |
| `ring` | capture + acquire/pin + sampled pixel read + release |
| `rig` | ring + scripted tracker + rig stage (the full foundation pipeline) |
| `render` | rig + CPU BGRA→RGBA compositing/readback proxy; real GPU render is T26 |

Switches: `--scenario`, `--tracker scripted|real` (`real` reports unavailable
until T11), `--frames`, `--warmup`, `--width`, `--height`, `--format`, `--fps`
(`0` = unpaced), `--output`, `--quiet`, `--self-test`.

Every scenario emits `schema: "avatar_bench.result.v1"` with:

- `build`: type, git commit, dirty flag, compiler.
- `hardware`: OS, arch, CPU brand, physical/logical cores, RAM, GPU name and a
  stable `fnv1a64:` fingerprint over that tuple.
- `metrics.frame_time_ms`, `metrics.frame_interval_ms`,
  `metrics.app_latency_ms`: `p50`, `p95`, `p99`, `mean`, `max`, `samples`.
- `metrics.dropped_frames`, `metrics.dropped_tracking_frames`,
  `metrics.frames.{requested,warmup,produced,measured,missed}`.
- `metrics.rss_bytes`, `metrics.cpu_percent_one_core` (100 = one full core),
  `metrics.gpu_percent` (null when the OS counter is unavailable).
- `metrics.stage_ms.{capture,ring,tracker,rig,render}` for stage isolation.

Run it with `tests/performance/run.ps1` (Windows) or `run.sh` (macOS/Linux);
both build RelWithDebInfo, pin process priority to Normal, discard the warmup
frames and write to the gitignored `tests/performance/results/`. Example:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/performance/run.ps1 `
  -Scenario rig -Frames 300 -Warmup 30
node tests/performance/check.mjs `
  --baseline tests/performance/baselines/foundation-local.json `
  --result tests/performance/results/latest-rig.json
```

`check.mjs` compares explicit `{metric, operator, target, allowance}` entries and
prints the failing metric with the result, target and allowed values. The
checked-in `foundation-local.json` is a local baseline: marked `kind: "local"`
and `release_gate: false`, it records this machine's hardware fingerprint and is
a regression guard for local work, not a release gate. T06 owns the nightly
runner, T26/T46 the real render/transport gates.

CTest registers `avatar_bench_selftest` under `fast;task_T05` (tiny scenarios,
JSON contract only) and `avatar_bench_perf` under `perf;task_T05`. `verify -Fast`
excludes `perf` with `-LE perf`; `verify -Full` runs it. The `tests/gates/TNN.ps1`
convention is unchanged: `verify -Task TNN` runs the gate when present, and the
`AVATAR_VERIFY_MODE` environment variable tells the gate whether the run is fast
or full. T14/T27/T45/T50/T64 can add gates without changing the verifier.

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

**Status:** implemented (T06). `.github/workflows/ci.yml` runs the fast matrix on every push and pull
request; `.github/workflows/nightly.yml` owns the slow, GPU-dependent and sanitizer suites. The local
mirror is `scripts/ci.ps1` (Windows) / `scripts/ci.sh` (macOS, Linux), which runs the same format,
licence, diagnostics, design-link, configure, build, `ctest -L fast` and smoke sequence.
`scripts/verify.ps1|sh` remains the task-level objective contract and adds `tests/gates/TNN.ps1`.

| Job | OS | Trigger | Notes |
|---|---|---|---|
| Unit + component | Win, macOS, Linux | Every push | No GPU required (software renderer for shader compile checks) |
| Golden images | Win (D3D12), macOS (Metal), Linux (Vulkan/lavapipe) | Every push | Lavapipe for Linux to keep runners GPU-free |
| Native build (MediaPipe) | All three | Nightly + on tracker changes | Slowest job; cached aggressively |
| Integration (transports) | All three | Nightly | Uses virtual devices; no physical camera |
| Performance | Reference hardware (self-hosted) | Nightly | Not on shared cloud runners |
| ASan + UBSan | Linux, macOS | Nightly | Unit/component only; no GPU |
| TSan | Linux | Nightly | Ring buffers, tracking handoff, output queues |
| Static analysis | Any | Nightly | clang-tidy plus the warning policy |
| Fuzzing | Linux | Nightly | glTF, JSON, shared-memory header, archive paths |
| Licence audit | Any | Every push | Fails on manifest drift |

A green build requires all blocking jobs. Performance and integration are blocking for release
branches, advisory on feature branches (with a warning comment). The performance job stays advisory
until the self-hosted reference runner exists.

### Implemented workflow jobs (T06)

| Job | Workflow | Blocking | What runs |
|---|---|---|---|
| `format` | `ci.yml` | Yes | clang-format 18.1.8 via `scripts/format-check.sh --require`; `.clang-format` is the contract |
| `licence` | `ci.yml` | Yes | `node scripts/check-licenses.mjs`; the diagnostics scan runs advisory; `node scripts/check-design-links.mjs` checks links, anchors and ADR indexing |
| `build-windows-msvc`, `build-macos-arm64`, `build-linux-gcc` | `ci.yml` | Yes | bootstrap, configure, RelWithDebInfo build, `ctest -L fast -LE perf`, headless smoke, failure artifacts |
| `full-suite-*` | `nightly.yml` | Yes | complete CTest suite on all three platforms |
| `sanitizers-asan-ubsan-tsan` | `nightly.yml` | Yes | ASan+UBSan and TSan over the fast label |
| `clang-tidy` | `nightly.yml` | Yes | `.clang-tidy`, `CMAKE_CXX_CLANG_TIDY`, GDExtension disabled to keep the run bounded |
| `golden-lavapipe` | `nightly.yml` | Yes after T26 | software Vulkan; no golden test is registered yet (`--no-tests=ignore`) |
| `performance-advisory` | `nightly.yml` | No until the reference runner | `avatar_bench` plus `tests/performance/check.mjs`; uploads results |

Formatting drift, a third-party include without a `native/licenses.json` entry, a denied licence, a
missing manifest field, a broken design link or ADR index, and a broken fast test all fail a blocking
job. Jobs whose name contains `(release-blocking)` are required for a green build; the exact list and
the branch-protection expectations are recorded below and in [ADR-0016](adr/0016-ci-quality-gates.md).
Action versions are pinned by major tag today; commit-SHA pinning is a T06 follow-up because each SHA
must be verified upstream.

### Branch protection (required checks)

Configure on `main` and any release branch:

1. Require a pull request with at least one approval; dismiss stale approvals on new commits.
2. Require the `ci.yml` checks `format (release-blocking)`, `licence manifest (release-blocking)`,
   `build-windows-msvc (release-blocking)`, `build-macos-arm64 (release-blocking)` and
   `build-linux-gcc (release-blocking)`.
3. Require branches to be up to date before merging.
4. Require linear history; squash merges are the project convention (`AGENTS.md`).
5. Block force-pushes and deletion of `main`.
6. A release tag needs a green `nightly.yml` run on the release commit; nightly jobs are not required
   for every pull request.

Renaming a job is a branch-protection change, not a cosmetic edit.

---

## Sanitizers, static analysis, and fuzzing

The data plane is threaded C++ with a binary cross-process contract, so "it passed on my machine" is
not enough.

| Tool | Scope | Gate |
|---|---|---|
| ASan + UBSan | Unit and component tests on Linux/macOS | Nightly; a finding is a release blocker |
| TSan | Ring buffers, tracking handoff, output queues | Nightly |
| MSVC ASan | Windows unit tests | Nightly; advisory until the toolchain is stable |
| clang-tidy | `native/` with a pinned config | Nightly; new findings fail |
| Warning policy | `-Wall -Wextra -Werror`, MSVC `/W4 /WX` | Every build |
| libFuzzer | glTF importer, JSON loader, shared-memory header parser, archive path handling | Nightly, 30 min per target |

### ThreadSanitizer job (T06)

The ring and latest-slot contracts are only credible under TSan. The nightly `sanitizers` job adds a
Linux TSan build (clang or GCC; MSVC has no equivalent), plus an ASan+UBSan build of the same fast
suite:

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

### Formatting and static analysis (T06)

- `.clang-format` is pinned to clang-format 18.1.8. `scripts/format-check.ps1` (Windows) and
  `scripts/format-check.sh` (macOS/Linux) report drift; `-Fix`/`--fix` rewrites. The CI `format` job
  installs 18.1.8 and runs with `AVATAR_REQUIRE_FORMAT=1`, so a missing tool is a failure there but a
  skip on a developer machine.
- `.clang-tidy` is pinned to clang-tidy 18 and runs in the nightly `clang-tidy` job with
  `CMAKE_CXX_CLANG_TIDY`. The check set is narrow and `bugprone-*`/`performance-*` findings are
  errors. Formatting is not clang-tidy's job; style checks that fight `.clang-format` are disabled.
- The compiler warning policy is unchanged and blocking in every build: MSVC `/W4 /WX`,
  GCC/Clang `-Wall -Wextra -Wpedantic -Werror`.

### Licence and dependency manifest (T06)

`native/licenses.json` is the source of truth for third-party components: name, version, SPDX
licence, upstream URL, modified flag, shipping status and include prefixes. `scripts/check-licenses.mjs`
is a build gate, not a report:

- an angle-bracket include from an unknown third-party prefix fails (quoted includes are project-local;
  standard and platform headers are allow-listed in the script),
- a manifest entry missing name/version/licence/url/modified fails,
- a GPL/AGPL/SSPL/LGPL/non-commercial/proprietary licence fails unless the component has a reviewed
  exception (reason plus ADR) in `policy.exceptions`.

The current entries are nlohmann/json, Godot and godot-cpp (active) and Eigen, MediaPipe, ONNX Runtime,
Spout2 and Syphon (planned). CTest registers `license_manifest_check` under `fast;task_T06`.

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

## Clean-checkout audit (T08)

The foundation is proven from a clean checkout, not from incremental local state. On 2026-11-20 a
detached worktree of the T07 commit was bootstrapped and verified on Windows:

| Command | Result |
|---|---|
| `scripts/bootstrap.ps1 -Fast` | PASS — downloaded and SHA256-verified nlohmann/json 3.11.3, Godot 4.7.2 and godot-cpp 10.0.0-stable into a fresh `.tools/` |
| `scripts/verify.ps1 -Fast -Task T08` | PASS — configure, RelWithDebInfo build, 6/6 `fast` CTest tests, headless smoke |
| `scripts/ci.ps1` | PASS — format, licence, advisory diagnostics, design links, bootstrap, configure, build, fast CTest, smoke |
| `scripts/format-check.ps1 -Require` | PASS — clang-format 18.1.8, 50 files |
| `node scripts/check-licenses.mjs` (+ `--self-test`) | PASS — 8 components (3 active) |
| `node scripts/check-diagnostics.mjs` (+ `--self-test`) | PASS — 69 codes, 32 files |
| `node scripts/check-design-links.mjs --require-tasks` | PASS — 34 tracked design files (35 including the gitignored ADR template), 239 links, 18 ADRs |
| `tests/performance/run.ps1 -Scenario rig -Frames 300 -Warmup 30` | PASS — 300 measured frames, 0 drops |
| `node tests/performance/check.mjs --baseline ... --result ...` | PASS — 7/7 checks against `foundation-local.json` |

Rules that follow from this audit:

- **A task gate must not require a gitignored workspace file.** `docs/progress/`, `docs/tasks/`,
  `docs/ROADMAP.md`, `docs/PROGRESS.md` and `docs/INDEX.md` are local planning artifacts. Gates may
  read them when present but must skip them in a clean checkout. `tests/gates/T06.ps1` was corrected
  after the audit failed on the absent `docs/progress/T06.md`.
- **A map row is `Implemented` only when its code path exists and a test or gate exercises it.**
  Correct the map rather than the code when a claim is premature. CI-only rows are verified as
  workflow definitions; their first hosted run remains a residual risk until it is observed.
- **The clean checkout is the acceptance environment for the foundation.** A green incremental build
  is not proof.

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


### T19 head-region regression gate

`ctest --test-dir build -C RelWithDebInfo -R ^avatar_head_unit$ --output-on-failure` checks rest-space
smoothstep, face-surface pivot derivation and shortest-arc rotation. `tests/gates/T19.ps1` also runs
`app/tests/head_region.gd` headlessly and through a hidden real OpenGL Compatibility window using the
pinned Windows Godot. Dummy rendering cannot satisfy the image gate. Engine checks cover atomic
AVT-117 rejection, RIG-006 derivation, cut/node mode switching, unchanged body vertices, coincident
neck seams, normal lengths and nonuniform nested transforms. Six angles per mode (yaw ±30°, pitch
and roll ±20°) compare the live CPU-updated mesh/decal pair to independently generated Quaternion.slerp
reference geometry and committed 320×320 PNGs in `tests/golden/head-region/`. RGB mean absolute error
must be <0.002; the rest-space decal must be visibly present. These synthetic fixtures prove continuity
and attachment, not T43 corpus quality.

Regenerate GLBs with `python scripts/gen_head_fixtures.py`. To regenerate image references intentionally,
run the script in a real render process with `-- --render --update-goldens`; that flag renders the
independent reference rather than blessing the live implementation. Inspect the PNGs before committing.
The gate never updates references. Cut and node benchmarks each use 20k triangles and report CPU plus
buffer submission median/p95 in `.tools/T19-head-region-render.json` (300 measured updates, 30 warmup,
one update per rendered frame). GPU completion and other draw cost are excluded. ADR-0029 explicitly
records the cut path's overrun of 0.15 ms; the gate does not pretend that the budget was met.

### T20 flat-decal regression gate

`app/tests/face_decal_flat.gd` runs headlessly for the component contract (three quads parented to the
head, placement on the face plane, every required uniform, camera/homography propagation, validation
and rotation) and through a hidden real OpenGL Compatibility window for images. The test scene
`app/tests/scenes/face_decal_flat.tscn` imports `assets/models/avocado_half.glb` and supplies a
generated 256×256 synthetic camera texture; no camera or third-party image is used.

Four 320×320 references live in `tests/golden/face-decal-flat/`: mask coverage, edge clamp, mirroring
and colour space. They are produced from an **independent analytic reference**, not from the live
shader: the reference computes the region signed-distance mask, the homography sample, the sRGB→linear
conversion, gain/lift and alpha, then composites over a model-only render. RGB mean absolute error
must be <0.01. The black-camera component test renders the same scene twice and asserts every changed
pixel is inside the analytic region masks (T20 measured 2,565 changed pixels and 0 outside). The
edge-clamp case shifts the homography so part of an eye and the mouth sample outside `[0,1]`; no
changed pixel may sample outside the camera frame. A half-opacity mouth render must match the same
analytic composite. The mirroring case puts a magenta marker at image
`(0.30, 0.45)`; it must land on the subject's right eye, which is on the viewer's left, and not on the
other eye. The colour-space case hides the model and asserts a 0.5 sRGB texel renders at 0.214 linear;
0.5 would mean the conversion was skipped and 0.038 would mean it was applied twice. The
Compatibility `SubViewport` capture is linear; final sRGB encoding is the output path's job (T23).

To regenerate the references intentionally, run the windowed script with `-- --render
--update-goldens`, inspect the PNGs, and commit them with an explanation. The gate never updates
references. `tests/gates/T20.ps1` runs both modes and rejects `SCRIPT ERROR`, `SHADER ERROR`, `ERROR:`
and `FAIL:` in the render log. The flat mode's yaw limit and lack of self-occlusion are documented in
[`face-compositing.md`](face-compositing.md#mode-1--flat) and [ADR-0030](adr/0030-flat-decal-quad-pass.md);
the goldens do not claim surface-mode quality.

### T21 surface-decal regression gate

`tests/gates/T21.ps1` runs `app/tests/face_decal_surface.gd` headlessly for component checks,
then in hidden real Compatibility **and Forward+** windows. The oracle in
`app/tests/face_decal_surface_oracle.gd` independently deforms rest vertices with Quaternion.slerp,
rasterizes triangles with a CPU depth buffer, interpolates rest attributes and posed normals, and
composites the analytic mask/homography sample over a model-only render. It never uses native posed
vertex output or the production shader to create the decal reference. Forward+ capture is sRGB,
so its model-only image is decoded before blending and the composite encoded afterwards;
Compatibility capture stays linear. Separate committed references live under
`tests/golden/face-decal-surface/{gl_compatibility,forward_plus}/`.

Twenty-two 320×320 cases per renderer cover the avocado and original flat, spherical and
sculpted-nose GLBs (`python scripts/gen_surface_fixtures.py`), each at rest, yaw ±30°, combined
cut deformation and a rigid 180° rear view. The sculpted nose must occlude >10 far-eye pixels at
each yaw (local references: 310/307). Rear views must have **zero** changed decal pixels while the
model remains visible. The rear case rotates all vertices rigidly; a 180° cut pose folds the neck
band through itself and is not a valid rear-only view. Four repeated front frames per model must
be identical (no coplanar flicker). A sampling variant checks rotation, anchor, partial opacity,
gain/lift and off-frame discard; a `head_node` case checks subtree deformation with authored
`face_submesh`. Headless checks also cover pass replacement/cleanup, all-mesh fallback, atomic
validation and immutable CUSTOM0/1 attributes under nested nonuniform transforms. T19's reference
mesh builder preserves both custom-channel format flags.

Live-versus-oracle mean maximum-channel RGB error must be <0.003 and fewer than 150 pixels may
differ by >0.1. Committed golden mean error must be <0.003. Regenerate only intentionally with the
pinned engine and each renderer using `--script res://tests/face_decal_surface.gd -- --render
--update-goldens`, inspect images, then commit; the gate never rewrites them. Synthetic tests cover
geometry, not human review or real-model corpus quality.

The performance phase uses the real Forward+ renderer, 1920×1080 and T19's 20k-triangle mesh.
It confirms visible decal pixels and exactly one additional draw for three regions. Vsync and the
frame cap are disabled; GPU viewport timestamps exclude CPU deformation, readback and presentation.
Six alternating off/on blocks each discard 30 warmup frames and retain 60 samples (180 per state).
`.tools/T21-surface-perf.json` records adapter/API, baseline/combined median and p95, signed median
delta and `max(0, p95_on - p05_off)` spread. Both delta and spread must be <0.7 ms; the spread is a
conservative noise check, not a statistical confidence interval. Verified local RTX 4080/Vulkan
measurement: 0.036 ms base, 0.048 ms combined, **0.012 ms delta**, 0.021 ms spread. Negative signed
deltas are retained as unresolved timing noise and never described as physical speedups. These
figures do not certify the documented hardware tiers. [ADR-0031](adr/0031-surface-decal-shared-pass.md).

### T22 presentation and framing gate

`tests/unit/test_framing.cpp` owns the framework-free math: the documented distance formula,
zoom/margin clamps and non-finite rejection, the `view_from_model` offset and 180° yaw direction,
head-sweep containment, AABB-inside-frustum at ±45° yaw/pitch/roll, head fit, extreme zoom/margin
containment, the deliberate `zoom > 1` crop, frustum plane/corner agreement and the final-frame
mirror. It is registered as `avatar_framing_unit` under the `fast;task_T22` labels.

`tests/gates/T22.ps1` then runs `app/tests/framing.gd` headlessly and in a hidden real **Forward+**
window. The headless phase checks the adapter contract, clamping and atomic AVT-113 rejection,
extreme containment, head fit, the preview display-only flip, output mirroring and the bounds
overlay (including that reading it adds no node to the output `SubViewport`). The render phase uses
an independent GDScript re-implementation of the swept bound, fit, distance and projection: it
projects a model-space magenta marker and the model AABB, then requires the live marker centroid to
be within 2.5 px and the silhouette to lie inside the projected AABB (height within 25% when the
AABB is fully in frame). Eight 320×320 goldens live in `tests/golden/framing/`: model fit, tight and
loose margin, zoom 0.5 and 2.0, offset, head fit and the mirrored final frame. The goldens are
regression baselines; the independent oracle is what proves the framing math. Regenerate only
intentionally with `--script res://tests/framing.gd -- --render --update-goldens`, inspect the
images, then commit; the gate never rewrites them.

The gate asserts containment for model fit at zoom 0.5/1.0 and margins 0–0.5, records
`model-zoom-in` as the documented crop, and checks that positive x offset moves the silhouette
right. Head fit deliberately crops the body. `zoom > 1` is not a fit failure; see
[ADR-0032](adr/0032-presentation-framing-and-mirroring.md).
