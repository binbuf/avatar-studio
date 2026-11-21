# Face Tracking

**Status:** proposal; the tracker C ABI v1, `ITrackingBackend` and the `ScriptedTracker` contract
harness are implemented (T09). The real MediaPipe backend remains `Planned` (T11). · **Last
updated:** 2026-11-20
**Decision record:** [ADR-0003](adr/0003-mediapipe-as-tracking-backend.md),
[ADR-0019](adr/0019-tracker-c-abi-v1.md)

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
(`ITrackingBackend`, `TrackerConfig`). `ScriptedTracker` implements it; T11's MediaPipe adapter will
implement it on top of the C ABI rather than exposing vendor types.

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
- The homography requires at least 8 inliers. A backend that provides fewer than 8 of these names is
  unsupported for sampling; it may still drive rotation.
- Unknown names are ignored. A profile missing names falls back to the available subset and raises
  `RIG-003`.
- Adding a name is additive: existing profiles lack it and the solver uses the subset.
- The package's `landmark_anchor` values (`eye_l_centroid`, `eye_r_centroid`, `mouth_centroid`) are
  computed from this set, never stored as indices.

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

## Pipeline

```
VideoFrame (NV12/BGRA, 1920×1080)
   │
   ├─▶ downscale + letterbox to model input (default 256×256, configurable)
   │     └─ YUV→RGB, aspect preserved, padding filled with edge pixels
   │
   ├─▶ FaceLandmarker::detect_async(image, timestamp_ms)
   │     ├─ runs on the tracking thread
   │     ├─ LIVE_STREAM mode: internally drops stale frames
   │     └─ outputs: 478 landmarks, 4×4 facial transform, 52 blendshapes
   │
   └─▶ TrackingResult (source_timestamp_ns = input frame timestamp)
```

Details that are easy to get wrong:

- **Timestamps must be strictly monotonic** in milliseconds. MediaPipe rejects or misbehaves on
  duplicates and non-monotonic input. The adapter converts from `FrameClock` nanoseconds and, on a
  duplicate (camera reset, clock reuse), increments by 1 ms and logs once. A camera reconnect must
  not silently corrupt the graph.
- **Letterbox, do not stretch.** A stretched face changes the apparent inter-ocular distance and
  the pose solver sees a systematically wrong head.
- **Prefer 16:9 → square padding on the sides**, not a centre crop: cropping cuts the chin at close
  range and the pose estimator loses the jaw.
- **Run inference on a downscaled frame, sample pixels from the full-resolution frame.** Landmark
  precision at 256² is ample for a homography; the decal needs the full-resolution texture.
- **No undistortion in 1.0.** Tracking and decal sampling use the same distorted frame; the
  assumption and its limits are in [`face-compositing.md`](face-compositing.md#lens-distortion).
- **One face.** `num_faces = 1`; the largest/closest face wins. Multi-face is deferred.

### Confidence

A single number derived from: detection presence score, landmark reprojection error from the
previous frame's fit, and pose plausibility (no 180° jumps). It is used by the rig for hold/decay
behaviour and by the UI for the "face lost" indicator. It is *not* exposed as a raw model score,
because those are not comparable across backends.

## Filtering and motion

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

### Tracking rate vs render rate

Rendering runs at the output rate (60 Hz). Tracking runs at the camera rate (30 Hz typical,
60 Hz when the camera supports it at the chosen resolution). Consequences:

- Head rotation is smooth at 60 Hz because it is interpolated, not because it is measured.
- Face *content* updates at 30 Hz. This matches how real cameras look and is generally not
  noticeable, but it can read as "smooth head, steppy eyes" on fast motion.
- Mitigation options, in order of cost: prefer 720p60 capture when available; accept it; warp the
  decal by the inter-frame landmark delta (deferred, complexity not justified yet).

## Segmentation

The software matte ([`face-compositing.md`](face-compositing.md#face-matte)) is an optional output of
the same runtime where it exists: MediaPipe Selfie Segmentation at 256×256, one pass, CPU or GPU
delegate. The provider returns a low-resolution mask aligned with the camera frame; the compositor
upsamples it with a luma-guided filter. It is behind `ISegmentationProvider` so a backend without it
still satisfies the tracking contract.

Segmentation is best effort by design. It never blocks tracking, it uses the same source frame as the
landmarks, and a slow mask is dropped rather than delayed. If the provider is absent or fails to
load, `TRK-009` is logged once and the decal falls back to the region mask.

## Failure behaviour

| Situation | Detection | Behaviour |
|---|---|---|
| No face in frame | Presence score below threshold | `valid = false`; output blanks; UI shows a hint |
| Face leaves and returns | Re-detection | Re-acquire without a pose jump: blend from the last pose over 150 ms. Output is blank during the gap |
| Missing eyes or mouth | Landmark subset incomplete | `features_valid = false`; output blanks. Sunglasses, a hand, or a profile view is the usual cause |
| Tracking stalls | No result for > 150 ms | Mark invalid; output blanks; keep rendering the preview |
| Tracker crashes | C-ABI call returns error | Disable tracking; output blanks (never a static face); surface a support code |
| Camera format change | Frame dimensions change | Reconfigure input; do not assume a fixed input size |
| OS resume | Timestamp base changed | Reset the graph and timestamp base; re-acquire; never feed a stale timestamp |
| Multiple faces | Second face appears | Ignore; keep tracking the current identity until loss |

Every transition is logged with a stable support code (`TRK-001` … ) so that user reports are
actionable without a screen recording.

## Alternative backends

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

`TrackerStats` (`native/tracking/include/avatar/tracking/tracking_result.h`) implements the first four:
`submitted`, `produced`, `dropped` plus a per-reason counter (`script_exhausted`, `backend_busy`,
`invalid_frame`, `not_initialized`, `shutdown`) and `last_drop_reason`; `input_width`/`input_height`;
a ten-bin `ConfidenceDistribution` with mean; and `inference_ns`, a
`core::LatencyHistogram<>` with p50/p95/p99. The scripted tracker leaves `inference_ns` empty because
it does no inference; T11 records one sample per produced result.

The histogram is the shared utility in `native/core/include/avatar/core/latency_histogram.h`
(100 microsecond buckets over a 102.4 ms window, exact max, saturating overflow bucket). The
performance harness uses the same type, so a percentile means the same thing in `TrackerStats` and in
`avatar_bench`'s JSON. Landmark reprojection error remains a T13/T26 measurement.

## Open questions

| Question | Resolution path |
|---|---|
| Achievable CPU inference time on the reference hardware at 256² and 192²? | M0 benchmark on all three platforms |
| Does MediaPipe's Windows build survive MSVC + our C++20 code in the same process? | M0; the C-ABI isolation is designed for this |
| Is the facial transformation matrix stable enough to drive rotation directly, or does it need the PnP/homography cross-check? | M1 A/B on recorded clips |
| Do we need 60 Hz tracking for fast head turns, or is 30 Hz plus prediction sufficient? | M1 usability test |
| Is the MediaPipe landmark index mapping stable across the pinned model versions? | M0 harness with an asymmetric marker; fail the build on drift |
