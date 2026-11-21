# Face Tracking

**Status:** Windows CPU backend implemented and measured (T11); the named landmark map, the
asymmetric-marker gate and profile correspondences are implemented (T12); the headless M0
harness and its 30-minute soak are implemented (T13); M0 is closed for the Windows MVP and the
renderer-facing tracking contract is audited (T14/T15, [ADR-0025](adr/0025-m0-windows-scope-and-deferred-platform-evidence.md)).
Filtering, the app integration, app-facing statistics and wider quality/platform validation remain
planned. **Last updated:** 2026-11-21
**Decision records:** [ADR-0003](adr/0003-mediapipe-as-tracking-backend.md),
[ADR-0019](adr/0019-tracker-c-abi-v1.md),
[ADR-0021](adr/0021-real-mediapipe-windows-backend.md),
[ADR-0022](adr/0022-named-landmark-map-and-asymmetric-marker.md),
[ADR-0023](adr/0023-m0-headless-harness-and-thresholds.md)


Tracking turns a camera frame into a head pose and 478 landmarks. It is the only component with a
large external dependency and the only one whose feasibility is not yet proven on all three
platforms. It is therefore the first thing built and the first thing gated.

---

## Contract

The rest of the system depends on exactly this:

```cpp
struct TrackingResult {
    uint64_t   source_timestamp_ns;   // timestamp of the frame that produced this result
    bool       valid;
    bool       features_valid;        // eyes and mouth both tracked; gates the output
    float      confidence;            // 0..1, monotonic with trustworthiness
    HeadPose   pose;                  // rotation (quaternion) + translation, camera space
    LandmarkSet landmarks;            // 478 points, normalised image coords + relative depth
    BlendshapeSet blendshapes;        // 52 coefficients; captured but unused in 1.0
};

class ITrackingBackend {
public:
    virtual ~ITrackingBackend() = default;
    virtual bool initialize(const TrackerConfig&) = 0;
    virtual void submit(const VideoFrame&) = 0;    // returns immediately
    virtual std::optional<TrackingResult> latest() = 0;
    virtual TrackerStats stats() const = 0;
    virtual void shutdown() = 0;
};
```

`valid` means a face is present. `features_valid` is stricter: the eye centres and mouth are present
well enough to build the decal, and the output gate requires it. A head pose without eyes or mouth is
still useful to the preview, but it must not put a face on screen.

Properties the contract guarantees:

- `submit` never blocks and never throws. If the backend is busy, the frame is dropped inside the
  backend, not queued by the caller.
- `latest` returns the newest result or nothing. There is no history; the rig does not need it.
- `source_timestamp_ns` always refers to the input frame, never to the time inference finished. This
  is what makes pose/pixel pairing possible.
- Results are published atomically; a reader sees a complete result or the previous one.

The C++ side of this contract is `native/tracking/include/avatar/tracking/itracking_backend.h`
(`ITrackingBackend`, `TrackerConfig`). `ScriptedTracker` implements it. The T11 real DLL implements the C ABI below; connecting the
Godot shell and its `ITrackingBackend` facade is app integration work (T16), with no vendor headers
outside the wrapper. ABI v1 does not expose real-worker statistics to that future facade.

### C ABI v1 (T09)

The real backend is a separate shared library behind
`native/tracking/include/avatar/tracker/tracker_c_api.h`. The header is the contract; the application
never includes a vendor header. It is C11-compatible, includes only `<stdint.h>` and `<stddef.h>`, and
carries no C++ standard-library type.

The ABI is exactly five functions plus a version macro:

| Function | Behaviour |
|---|---|
| `avatar_tracker_create(config, out_error)` | Allocates a backend. A `config` whose `abi_version` is not `AVATAR_TRACKER_ABI_VERSION` fails with `AVATAR_TRACKER_ERROR_ABI_MISMATCH`. A null config uses `AVATAR_TRACKER_CONFIG_INIT`. |
| `avatar_tracker_configure(handle, config)` | Replaces the configuration, rewinds the backend and discards an unconsumed result. |
| `avatar_tracker_push_frame(handle, frame)` | Never blocks on inference and never queues. A busy/exhausted backend drops the frame and still returns `AVATAR_TRACKER_OK`; the drop is counted in `TrackerStats`. |
| `avatar_tracker_poll_result(handle, out)` | Copies the newest unconsumed result and returns `AVATAR_TRACKER_OK`, or returns `AVATAR_TRACKER_NO_RESULT` and zeroes the output. Two polls without a push return one result. |
| `avatar_tracker_destroy(handle)` | Null-safe teardown. The handle is invalid afterwards; double destroy is undefined. |

`AVATAR_TRACKER_ABI_VERSION` is `1`. Errors are negative and statuses are positive:
`AVATAR_TRACKER_OK` (0), `AVATAR_TRACKER_NO_RESULT` (1), then
`INVALID_ARGUMENT`, `ABI_MISMATCH`, `INVALID_CONFIG`, `NOT_INITIALIZED`, `BUSY`, `FRAME_TOO_LARGE`,
`INTERNAL`. `BUSY` is reserved: `push_frame` drops rather than making the caller retry.

The result is a fixed, pointer-free POD layout. `avatar_tracker_result` is 5992 bytes with 8-byte
alignment on both 32-bit and 64-bit targets:

| Offset | Field | Size |
|---:|---|---:|
| 0 | `source_timestamp_ns` | 8 |
| 8 | `valid` (0/1) | 1 |
| 9 | `features_valid` (0/1) | 1 |
| 10 | reserved, zero | 2 |
| 12 | `confidence` (0..1) | 4 |
| 16 | `pose` (`rotation[4]` x,y,z,w; `translation[3]`, metres) | 28 |
| 44 | `landmark_count` (≤ 478) | 4 |
| 48 | `landmarks[478]` (`x,y,z` each) | 5736 |
| 5784 | `blendshapes[52]` | 208 |

Entries at or beyond `landmark_count` are zero; all 52 blendshapes are present and zero when a backend
has no expression output. `avatar_tracker_frame` carries the timestamp, sequence, dimensions, format,
stride and up to three borrowed plane pointers. The pixel pointers are valid only for the duration of
`push_frame`; a backend that needs them afterwards copies them.

The `ScriptedTracker` C ABI implementation serializes its calls with a mutex, so a capture thread and
a render thread can share one handle. The real backend owns its inference thread and must preserve the
same external contract. The `simulate_fault` config field exists only so the contract suite can prove
that a backend exception is converted to `AVATAR_TRACKER_ERROR_INTERNAL`; production callers leave it
at `AVATAR_TRACKER_FAULT_NONE`.

`tests/unit/tracker_c_api_c_compile.c` compiles the header as C11 with warnings-as-errors and asserts
the layout table above. `tests/unit/test_tracker_contract.cpp` exercises the C ABI and the C++
interface: submit never blocks, latest-only polling, timestamp preservation, duplicate/non-monotonic
timestamps, `valid` versus `features_valid`, null-safe destroy, invalid frames, and exception
containment.

### Scripted tracker (T04)

`native/tracking/include/avatar/tracking/scripted_tracker.h` implements the contract without
inference. It implements both `ITrackingBackend` and the C ABI above, so the same object can drive a
C++ component test or a C host. It pre-builds a deterministic sequence and `submit(frame)` stamps each
result with that frame's `timestamp_ns`; `latest()` returns the most recent result. `configure`
rebuilds the sequence and rewinds; `shutdown` stops accepting frames until `initialize` resumes. The
scenarios cover the failure modes the rig and the output gate must survive:

| Scenario | Behaviour |
|---|---|
| `valid_tracking` | Valid pose and landmarks, smooth yaw/pitch/roll path |
| `dropouts` | `valid = false` and `features_valid = false` for a contiguous window |
| `jitter` | Seeded LCG noise on pose and landmarks, bounded so a test can assert the envelope |
| `outliers` | Periodic 60° pose jumps and shifted landmarks with low confidence |
| `missing_mouth` | `valid = true`, `features_valid = false`, mouth landmarks zeroed |
| `left_right_swap` | Landmark x mirrored and yaw negated for a segment, to catch mirroring bugs |

The first `kScriptedLandmarkCount` landmarks have a documented order (eye corners, iris centres,
nose, mouth, chin, jaw, temples, cheeks, `marker_right_cheek`) so a test can address them without the
runtime `LandmarkMap`; T12 resolves the names to backend indices. The sequence is seeded, so two
trackers with the same config produce byte-identical results.

## Landmark map

The rig never hard-codes backend indices. `LandmarkMap` is the named correspondence set: a stable
anatomical name per point, resolved by the active backend adapter. A `FaceProfile` stores names and
canonical `(u,v)`, so it survives a backend swap
([`user-data.md`](user-data.md#faceprofile)).

The correspondence set used for the homography excludes eyelids, inner lips and eyebrows because
they move with expression:

| Name | Anatomical point |
|---|---|
| `eye_outer_r`, `eye_inner_r` | Subject's right eye corners |
| `eye_outer_l`, `eye_inner_l` | Subject's left eye corners |
| `iris_center_r`, `iris_center_l` | Iris centres, when the backend provides them |
| `nose_bridge`, `nose_tip` | Nose midline |
| `nose_left`, `nose_right` | Nose alae |
| `mouth_corner_r`, `mouth_corner_l` | Mouth corners |
| `mouth_upper_center`, `mouth_lower_center` | Lip midline |
| `chin` | Chin point |
| `jaw_left`, `jaw_right` | Jaw at the widest stable point |
| `temple_l`, `temple_r` | Temple |
| `cheek_l`, `cheek_r` | Cheekbone |
| `forehead_center` | Forehead midline |

`_l`/`_r` are the subject's left and right, never the viewer's. The MediaPipe adapter maps each name
to its pinned model index; the index table is backend data, not design. The M0 harness verifies the
mapping with an asymmetric marker (a dot on the subject's right cheek) and fails if the names are
swapped.

Requirements:

- Every name resolves to a distinct index, or the adapter reports it as unavailable.
- The solver requires at least 8 usable correspondences before RANSAC; T18's post-estimation inlier
  gate uses `RIG-002`. A backend that provides fewer than 8 of these names is unsupported for
  sampling; it may still drive rotation.
- Unknown names are ignored. A profile missing names falls back to the available subset and raises
  `RIG-003`.
- Adding a name is additive: existing profiles lack it and the solver uses the subset.
- The package's `landmark_anchor` values (`eye_l_centroid`, `eye_r_centroid`, `mouth_centroid`) are
  computed from this set, never stored as indices.

### Realised map and marker gate (T12)

`native/tracking/include/avatar/tracking/landmark_map.h` implements the stable
`LandmarkName` set above and `LandmarkMap`, which resolves a name to the active backend's
index. `kMediaPipeFaceLandmarkerTable` is pinned to MediaPipe 0.10.21 with the
`face_landmarker/face_landmarker/float16/1` bundle; `kOnnxFaceMeshV2Table` is a separate
pinned table for the evaluated `senty-au/face_landmarks_detector-ONNX` conversion
(`337d58218b5b1cc597ca3c67360880b920f6ce7b`, 478 points). The ONNX table is not used by a shipping backend yet, and the
tables are deliberately separate so a future adapter opts into the topology it actually
runs.

`resolve` returns `nullopt` for an unknown name or a name the backend cannot provide; the
rig treats that as unavailable and never guesses an index. A table that binds two names to
one index reports the duplicate and ignores the later binding. `LandmarkMap::for_backend`
is the adapter-facing selector.

`canonical_face_frame.h` implements the canonical plane as a 2-D similarity: origin at the
eye midpoint, +u toward the subject's right eye, +v toward the forehead, inter-ocular
distance 1.0, image x scaled by `width / height`. The frame follows head roll instead of
assuming a level eye line. `check_handedness` is the asymmetric-marker assertion: the
subject-right cheek must be left of the eye midpoint and have positive canonical u. The
test places a marker at canonical `(0.62, -0.25)` on a synthetic face, passes with both
real tables, and fails with a table whose `_l`/`_r` indices are swapped. On the T11
recorded C920 clip, indices `33/133/468`, `129`, `50`, `234`, `127` and `61` appeared on
the viewer's left, while `263/362/473`, `358`, `280`, `454`, `356` and `291` appeared on
the viewer's right, which is the expected subject-right/viewer-left mapping.

`profile_correspondence.h` joins a `FaceProfile` landmark array to a live result by name.
At least 8 usable correspondences are required for sampling; T18's RANSAC inlier gate is a
separate post-estimation check. A profile name this backend cannot provide falls back to
the remaining subset and raises `RIG-003`; unknown names are ignored for forward
compatibility; duplicate backend indices are reported and used once. Profiles serialize
and parse the named `(u,v)` array through JSON without losing names, so a backend swap
does not rewrite the profile. The full profile document, capture and storage remain T31.

## Why MediaPipe first

MediaPipe Face Landmarker is the only maintained, permissively licensed option that provides all
three things the compositor needs in one pass: dense landmarks, a metric head pose, and expression
coefficients. Alternatives provide one or two.

But it is not free of risk, and the design must survive it being replaced:

| Concern | Reality (verified 2026-11-20) | Response |
|---|---|---|
| Windows support | Officially "experimental": MSYS2 + Bazel + a pinned OpenCV, MSVC-only, non-trivial environment | M0 spike; CI builds it; C-ABI isolation |
| GPU inference | Not supported on Windows or macOS desktop; Linux GPU path exists but is not worth the variance | Assume CPU-only in the design and the performance budget |
| Build system | Bazel, with its own Abseil/Protobuf/OpenCV tree | Build once per platform as a shared library behind a C ABI; never mix into the app's CMake graph |
| Model weights | Distributed separately from the source; license must be audited per file | Checksummed, recorded in the dependency manifest |
| API stability | Tasks C++ API has changed across releases | Pin the version; wrap, do not sprinkle calls |

**Isolation rule:** MediaPipe is compiled into `libavatartracker.so/.dylib/.dll` exposing a
five-function C API (`create`, `configure`, `push_frame`, `poll_result`, `destroy`). The application
never includes a MediaPipe header. This is not stylistic: MediaPipe's vendored Abseil and Protobuf
will collide with any other copy in the process, and symbol leakage across a GDExtension boundary is
extremely hard to debug.

## Realised Windows backend (T11)

`native/tracking/mediapipe/` implements `libavatartracker.dll` with exactly the five T09 exports.
`ScriptedTracker` remains a separate static test implementation; the real DLL never uses scripted
scenarios or substitutes synthetic landmarks when inference fails. `num_faces = 1`, CPU execution,
478 landmarks, 52 blendshapes and the facial transformation matrix are enabled. MediaPipe chooses
one detected face and tracks it; selection is not a guaranteed largest-face or identity policy.

The pinned build is MediaPipe **0.10.21**, commit
`cad7f3ab99ebf175947e40c5252c642612aae927`, Bazel **6.5.0**, hermetic Python **3.12**, and
OpenCV **3.4.10** (`opencv_world3410.dll`). MSYS2 bash and an installed MSVC/Windows SDK are
prerequisites. T11 used MSVC 14.51.36231 / Windows SDK 10.0.26100.0. The build script downloads and
hash-checks archives inside `.tools/`, points the vendor WORKSPACE at that local OpenCV, applies
one MSVC preprocessor fix and a glog static-export patch, and defines `TFL_STATIC_LIBRARY_BUILD`.
Patch files are LF-pinned; the build normalizes the patched header so clean extractions and older
caches both apply correctly. The last custom-op compilation takes several minutes. CMake consumes the SDK without importing
Bazel, Abseil, Protobuf, Eigen or OpenCV into the application's compile/link graph.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-tracker.ps1 -Jobs 6
cmake -S . -B build -DAVATAR_REAL_TRACKER=ON -DAVATAR_TESTS=ON -DAVATAR_BENCHMARKS=ON
cmake --build build --config RelWithDebInfo --parallel
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify.ps1 -Fast -Task T11
```

`verify -Task T11` invokes the tracker build itself. Other fast tasks can build without this SDK;
`AVATAR_REAL_TRACKER=ON` fails explicitly if it is incomplete. The SDK and configured executable
directory contain `libavatartracker.dll`, `opencv_world3410.dll`, the private measurement executable
`avatar_tracker_bench.exe`, and `tracker-models/face_landmarker.task`. The DLL resolves the model
relative to its own module, checks its size and SHA256, and passes the verified bytes to MediaPipe.
CMake also stages the DLL, OpenCV, model and licence manifest/texts into `app/bin/` for offline
application bundling. Missing/corrupt models fail `create` with `INTERNAL`; execution performs no
model download.

### Model provenance

| Item | Pin / permission |
|---|---|
| Google Face Landmarker bundle | `face_landmarker/face_landmarker/float16/1/face_landmarker.task` |
| Bundle SHA256 | `64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff` |
| Detector / landmarks / expressions | BlazeFace short-range / Face Mesh V2 (478) / Blendshape V2 (52) |
| Weight licence | Apache-2.0, explicitly stated in each Google-authored model card |
| Audit | `native/licenses.json`: exact download URL, individual bundle-member hashes, model-card URLs/hashes and evaluated ONNX alternatives |

ONNX Runtime 1.20.1 and two permissively licensed ONNX conversions were evaluated locally and are
recorded as unshipped. The 478-point ONNX model also has a fixed 256 tensor; changing its declared
shape to 192 fails at its final convolution. No fallback DLL is shipped, and ADR-0003 is retained.

## Pipeline

```text
Borrowed NV12 / YUY2 / BGRA / RGBA frame
  -> validate layout and copy to one owned slot; busy input is dropped, never queued
  -> worker: convert colour + bilinear downscale + edge-replicated square letterbox
  -> FaceLandmarker::DetectForVideo(image, strictly increasing timestamp_ms), CPU
  -> invert letterbox coordinates, convert pose centimetres to ABI metres
  -> atomically replace newest unconsumed POD result; preserve source_timestamp_ns exactly
```

The worker owns the synchronous VIDEO-mode graph rather than using a second, vendor-internal
LIVE_STREAM queue. Push never waits for inference; poll consumes only the latest complete result.
Worker failures latch `INTERNAL`, clear pending output, and require successful reconfiguration.
Reconfigure replaces the graph, clears results and resets its internal timestamp base. Teardown joins
the worker. Frames up to 8192 per dimension are validated before allocating/copying their declared
layout. MJPEG and metadata-only input are rejected; unsupported input never produces fake output.

`config.input_width/input_height` select an equal square prepared image, 64..1024; zero chooses
256. The other scenario/sequence fields exist for the scripted ABI harness and do not control the
real graph. T11 compares **192x192 and 256x256 prepared images**. These are not alternate landmark
network architectures: MediaPipe internally resizes detected face crops to its fixed **256x256**
landmark tensor and uses a **128x128** detector. Benchmarks report all three resolutions explicitly.

- Internal graph time is `max(source_ns / 1,000,000, previous_ms + 1)`. Duplicate and backwards
  inputs are counted; the public result retains the original nanosecond timestamp. No other epoch
  or integer-unit substitution is applied.
- Letterbox preserves aspect ratio and maps coordinates back to the original frame. A wide 16:9
  input receives top/bottom padding; a tall input receives side padding. Edges are replicated.
- ABI v1 carries one stride and no colour-space field. NV12 requires equal Y/UV strides, even
  width/height and BT.601 limited input; YUY2 likewise assumes BT.601 limited. Other camera colour
  spaces must be converted to RGBA/BGRA before calling this ABI. The T11 recorder explicitly checks
  BT.601 and records the real 640x480 NV12 camera frames.
- Pose is the upstream metric transform converted to a normalized quaternion and metres; it is
  not camera-calibrated rig math. Pose filtering and synthetic accuracy tests are implemented in T18; real-corpus quality remains T26/T27.
- Tracking uses reduced pixels; eventual decal sampling still uses the full-resolution source.
  Lens distortion assumptions remain in [face-compositing](face-compositing.md#lens-distortion).

### Confidence and feature validity

MediaPipe Tasks C++ does not expose the detector/presence logits. The implemented confidence is
therefore an explicit **geometric trust proxy**: the fraction of the 478 finite landmarks lying
inside the original image, after the graph's detection/presence/tracking thresholds of 0.5. A lost
face produces zero confidence, `valid = false`, zero landmarks and zero blendshapes. It is not a
calibrated probability or the planned reprojection/pose-plausibility score.

`features_valid` additionally requires both eye-corner pairs, both iris centres, mouth corners and
lip centres to be finite, inside the source image and geometrically non-degenerate. Any available
upstream visibility/presence flag below 0.5 also invalidates features. A present face can therefore
have `valid = true` and `features_valid = false`. Face Mesh does not reliably supply per-feature
occlusion probabilities; sunglasses/hand/profile rejection remains a real-corpus quality gate,
not a capability implied by this geometric check. Output continues to require `features_valid`.

## Filtering and motion

**Implemented in T18** in `native/rig`; T11 continues to return upstream pose and landmarks.
The engine integration remains T24. See [ADR-0028](adr/0028-facerig-estimation-and-temporal-contract.md).

Raw landmarks and pose jitter at rest and lag under motion. The filter stack:

| Stage | Technique | Parameters |
|---|---|---|
| Outlier rejection | Reject a result whose pose differs from the prediction by more than a threshold; require N consecutive rejects before accepting a new baseline | 35°/frame, N=3 |
| Rotation smoothing | One Euro filter per Euler channel (or quaternion slerp with adaptive alpha) | min cutoff 1.0 Hz, beta 0.02 |
| Translation smoothing | One Euro, higher cutoff | min cutoff 2.0 Hz |
| Deadband | Ignore changes below a threshold at low speed to kill shimmer | 0.15° |
| Prediction | Extrapolate pose to render time by `(t_render − t_source)`, clamped | max 50 ms |
| Loss handling | Bridge a dropped frame for at most 150 ms, then mark invalid and blank the output. Never decay to neutral | timeout 150 ms |

One Euro is chosen over a fixed low-pass because the artifact profile is asymmetric: at rest users
notice jitter, in motion they notice lag. A fixed filter forces a single bad compromise.

**Prediction is bounded on purpose.** Extrapolating a head turn through a 200 ms tracking gap
produces overshoot that looks worse than a freeze. Clamp, then blank.

T18 uses independent unwrapped Euler channels in `Rz(roll) * Ry(yaw) * Rx(pitch)`
order, measured in **degrees**. For each One Euro channel, raw derivative is
`(x-x_previous)/dt`, derivative cutoff is **1 Hz**, and signal cutoff is
`min_cutoff + beta*abs(filtered_derivative)`. Rotation has min cutoff **1 Hz**,
beta **0.02 per degree/s**; translation has min cutoff **2 Hz**, beta **0.02 per m/s**.
Both use `alpha = 1/(1+1/(2*pi*cutoff*dt))`. Filter dt is clamped to **[1 us,50 ms]**;
initial state is the first measured sample. A rotation channel holds when its
filtered increment is below **0.15 degrees** and derivative below **3 degrees/s**.
Euler angle unwrapping avoids the ±180-degree discontinuity; ±90-degree yaw
singularities are outside the frontal-face operating range.

Pose rejection compares quaternion distance against the prediction (**35 degrees**)
or translation distance (**0.25 m**). Three rejected poses within **5 degrees** of
each other, consecutively, allow a new baseline through the same filters. An invalid
input interrupts that sequence. Render prediction uses velocity between filtered
samples divided by the actual source interval, with a horizon clamped to **[0,50 ms]**.
The filter dt cap must not amplify prediction velocity after a long gap. It does
not predict H while H is solving successfully. The output pose is the inverse
camera-from-head transform; T19/T24 must apply the intended rotation convention.

A successful pose/H/region sample starts the **150 ms** hold. Invalid face/features,
rejected samples, failed solves, and repeated/older timestamps retain that deadline.
Through exactly 150 ms, the last state can bridge a dropped frame or blink. After
150 ms, confidence is zero and `features_valid=false`; internal pose never decays
to neutral. Configure/reset is required for an epoch change. Reacquisition retains
the filters with the 50 ms dt cap; a separate fixed-duration 150 ms recovery blend
and output/UI transitions are not implemented in this core task.

### Tracking rate vs render rate

Rendering runs at the output rate (60 Hz). Tracking runs at the camera rate (30 Hz typical,
60 Hz when the camera supports it at the chosen resolution). Consequences:

- Head rotation is smooth at 60 Hz because it is interpolated, not because it is measured.
- Face *content* updates at 30 Hz. This matches how real cameras look and is generally not
  noticeable, but it can read as "smooth head, steppy eyes" on fast motion.
- Mitigation options, in order of cost: prefer 720p60 capture when available; accept it; warp the
  decal by the inter-frame landmark delta (deferred, complexity not justified yet).

## Segmentation

**Planned.** T11 does not load a segmentation model or emit a matte.

The software matte ([`face-compositing.md`](face-compositing.md#face-matte)) is an optional output of
the same runtime where it exists: MediaPipe Selfie Segmentation at 256×256, one pass, CPU or GPU
delegate. The provider returns a low-resolution mask aligned with the camera frame; the compositor
upsamples it with a luma-guided filter. It is behind `ISegmentationProvider` so a backend without it
still satisfies the tracking contract.

Segmentation is best effort by design. It never blocks tracking, it uses the same source frame as the
landmarks, and a slow mask is dropped rather than delayed. If the provider is absent or fails to
load, `TRK-009` is logged once and the decal falls back to the region mask.

## Failure behaviour

This is the intended application policy. T11 implements no-face results, geometric feature gating
and backend errors; the app output gate, recovery blending and UI diagnostics remain later tasks.

| Situation | Detection | Behaviour |
|---|---|---|
| No face in frame | Presence score below threshold | Tracker `valid = false`; rig bridges at most 150 ms from the last complete sample, then output blanks; UI shows a hint |
| Face leaves and returns | Re-detection | T18 retains filter state and caps the reacquisition dt at 50 ms. A fixed 150 ms recovery blend remains T24 integration work; output is blank after the loss hold |
| Missing eyes or mouth | Landmark subset incomplete | Tracker `features_valid = false`; rig bridges at most 150 ms, then its output validity clears. Sunglasses, a hand, or a profile view is the usual cause |
| Tracking stalls | No result for > 150 ms | Mark invalid; output blanks; keep rendering the preview |
| Tracker crashes | C-ABI call returns error | Disable tracking; output blanks (never a static face); surface a support code |
| Camera format change | Frame dimensions change | Reconfigure input; do not assume a fixed input size |
| OS resume | Timestamp base changed | Reset the graph and timestamp base; re-acquire; never feed a stale timestamp |
| Multiple faces | Second face appears | Ignore; keep tracking the current identity until loss |

Every transition is logged with a stable support code (`TRK-001` … ) so that user reports are
actionable without a screen recording.

## Alternative backends

### T14 feasibility decision

[ADR-0024](adr/0024-m0-windows-evidence-and-open-gate.md) retains the pinned Windows MediaPipe
backend without declaring M0 complete. T14's fresh real replay soak passed the local numeric
thresholds (p95 10.012 ms at 192x192 and 11.510 ms at 256x256). The Ryzen 9 7900X3D / RTX 4080 /
63.2 GiB host exceeds the documented Reference tier; the automatic `reference` label merely
selects the stricter 15 ms limit. These measurements do not certify slower Reference or
Minimum hardware, and the two prepared input sizes still use the same fixed graph tensors.

Valid poses were returned for 16,730/27,000 frames at 192x192 and 16,969/27,000 at
256x256. Zero dropped submissions does not mean continuous valid tracking. This short,
unground-truthed replay leaves a material tracking-loss risk for the planned blank-on-loss
policy; it cannot establish quality across a deliberate stationary and motion corpus.

Real macOS/Linux backends and successful three-platform real-backend CI remain unproven; they are
T52/T55 acceptance under [ADR-0025](adr/0025-m0-windows-scope-and-deferred-platform-evidence.md).
The numeric gate does not automate the subjective rest-jitter, motion-swimming or handedness
judgment. A contiguous, timestamp-paired diagnostic clip is required for human review; the
48 BMPs sampled across each 15-minute soak are a contact sheet, not a normal-speed video.
Loop-boundary pose jumps, reacquisition and no-face intervals must be labelled rather than
mistaken for stationary jitter or silently removed. Repeated replay phases can help detect
long-term change, but do not replace stationary ground truth or a human stability decision.
ADR-0025 closes M0 for the Windows MVP on the measured repeatability plus the sampled-frame review;
T27 re-judges motion swimming on rendered clips. T15 audited the contract below and found no
blocker to starting the renderer, with two limits recorded rather than waived: ABI v1 exposes no
statistics query, and ABI v1 has no distinct model-integrity error.

### T15 renderer-facing contract audit

| Contract | Realised behaviour | Evidence |
|---|---|---|
| `source_timestamp_ns` | Exactly the input frame's `timestamp_ns`; the public result is never restamped with inference-completion time. Duplicate/backwards inputs are counted and the internal graph millisecond is advanced by 1, while the public nanosecond value is preserved. | T09 contract suite, T11 real-backend tests, M0 timestamp pairing |
| `features_valid` | Stricter than `valid`: both eye-corner pairs, both iris centres, mouth corners and lip centres must be finite, inside the source image and non-degenerate. A present face can be `valid = true` and `features_valid = false`; the output gate requires the latter. | `test_real_tracker.cpp`, T09 contract suite, T23 owns the gate |
| Pose | Camera-space unit quaternion `[x, y, z, w]` and translation in metres, converted from the upstream metric transform. It is not camera-calibrated rig math; filtering and the homography are T18. | T11/T12, T14 repeatability, T18 filtering |
| Landmarks | 478 normalised image coordinates plus relative depth; names resolve through the pinned T12 tables. | T12 asymmetric-marker gate |
| Stats | `TrackerStats` is the C++/scripted instrumentation. The real DLL keeps private counters and a bounded 4096-sample reservoir, but ABI v1 has no query, so the app HUD's real statistics are T26 work. | `tracker_c_api.h`, T11 `avatar_tracker_bench` |
| Failure codes | The DLL returns the ABI error enum (`INTERNAL`, `NOT_INITIALIZED`, ...) and does not emit `TRK-*` diagnostics itself. The T16 facade maps the enum to the registry; T23 blanks the output on any non-OK or invalid result. A missing/corrupt model and another internal failure both return `INTERNAL`, so the app verifies the staged model before `create`. | `tracker_c_api.h`, `test_real_tracker.cpp`, T16/T23/T26 |

### Backend choices

The seam exists so this table can be reordered by measurement, not by preference:

| Backend | License | Provides | Trade-off |
|---|---|---|---|
| MediaPipe Face Landmarker (default) | Apache-2.0 code, weights audited separately | Landmarks + pose + blendshapes, one pass | Build fragility, CPU-only on two platforms |
| ONNX Runtime + landmark model | MIT runtime; model licence per model | Landmarks; pose solved in-house | Extra model work; more control; best fallback |
| Apple Vision (`VNDetectFaceLandmarksRequest`) | OS API | 76 landmarks + pose on macOS | macOS only, no blendshapes, coarse mouth detail |
| OpenSeeFace | BSD-style | Landmarks + pose, tuned for VTubing | Older model stack; separate runtime |
| Custom 3DDFA-style regressor | Varies | Dense mesh + pose | Training and maintenance burden |

The fallback plan is concrete: **ONNX Runtime plus a landmark model, with pose solved by
`FaceRig`'s homography/PnP**. It is not a research project, it is a smaller model and the same
post-processing. It is scheduled only if M0 fails.

## Instrumentation

The tracker exposes, per session:

- inference time (p50/p95, per platform),
- input resolution actually used,
- dropped-frame count and reason,
- confidence distribution,
- landmark reprojection error (proxy for stability).

These are shown in a debug HUD (hidden behind a flag) and included in opt-in diagnostics. Without
them, "the face feels laggy" is unactionable.

`TrackerStats` (`native/tracking/include/avatar/tracking/tracking_result.h`) implements the
scripted C++ instrumentation: submitted/produced/drop-reason counters, resolution, confidence bins
and the shared latency histogram. The real DLL's private `Metrics` records exact submitted,
produced and busy-drop counters, timestamp corrections, ten confidence bins, and at most 4096
preprocessing/inference samples. Counters cover the whole session; the bounded sample reservoir
covers its first 4096 results. `avatar_tracker_bench` reports exact sorted p50/p95 after warm-up.
It uses the same worker/engine/preprocessing implementation as the DLL, compiled into a separate
measurement executable; no extra instrumentation symbol is exported from the DLL.

The M0 harness (`avatar_m0`, `tests/m0/`) is the app-facing measurement that T11 did not provide.
It loads the real DLL through the five-function ABI and records **submit-to-poll** latency (the
time from `push_frame` to the poll that returns that frame's result) and **frame age** (poll time
minus the source timestamp). The first is not a vendor graph timer and includes worker scheduling
and at most 1 ms of poll granularity; the second is what a consumer would experience. Frames that
produce no result are classified after the run as `superseded` (a newer result arrived first),
`stalled` (no later result) or `submit_error`; capture-side misses are reported separately. The
harness also records the ten confidence bins, RSS and handle samples, thread CPU, and a pose
trace/chart/clip for human review. `tests/m0/run.ps1 -Mode full` is the 30-minute soak and
`tests/gates/T14.ps1` applies the M0 thresholds. A run with `ScriptedTracker` or the test pattern
is labelled `synthetic`; it proves the harness, not tracker feasibility. ABI v1 still has no
statistics query, so the app HUD's real-statistics interface remains T26 work.

ABI v1 has no statistics query. Real counters are **not yet wired to the app's future debug HUD**;
that requires a separate interface decision in T13/T26. Reprojection error and the normalized
cross-backend confidence score also remain planned. CTest checks the real and scripted ABI
contracts, letterbox/unprojection, supported colour conversion, feature predicates, model tamper
rejection, and the actual five-symbol PE export table. A local eight-second camera clip supplies
real inference measurements; the task findings remain in the gitignored progress workspace.

## Open questions

| Question | Resolution path |
|---|---|
| CPU inference at 256² and 192² prepared image sizes? | Windows measured in T11; graph tensors stay fixed. Other platforms remain planned |
| Does MediaPipe's Windows build survive MSVC + C++20 in one process? | Proven by T11 real DLL contract tests; preserve isolated symbols |
| Is the facial transformation matrix stable enough to drive rotation directly, or does it need the PnP/homography cross-check? | M1 A/B on recorded clips |
| Do we need 60 Hz tracking for fast head turns, or is 30 Hz plus prediction sufficient? | M1 usability test |
| Is the MediaPipe landmark index mapping stable across the pinned model versions? | M0 harness with an asymmetric marker is implemented in T12; rerun the marker test and review the pinned table whenever the model bundle changes |
