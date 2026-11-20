# System Architecture

**Status:** proposal · **Last updated:** 2026-11-20

Decision records for the choices below: [ADR-0001](adr/0001-engine-and-language.md) (engine and
language), [ADR-0002](adr/0002-native-camera-capture.md) (capture), [ADR-0003](adr/0003-mediapipe-as-tracking-backend.md)
(tracking), [ADR-0004](adr/0004-face-decal-projection.md) (compositing),
[ADR-0005](adr/0005-output-transports.md) (output).

---

## Context and constraints

| Constraint | Consequence |
|---|---|
| Windows, macOS, Linux, one codebase | Engine must export to all three; platform code isolated behind interfaces |
| 60 fps at 1080p with a live camera | No per-frame CPU work on full-resolution frames; no synchronous GPU readback |
| Must be usable without OBS | OS-level virtual camera output is required, not optional |
| Must be usable without Blender | Runtime glTF loading and an in-app calibration editor |
| Offline by default | No service dependencies, no model downloads at runtime |
| Permissive license, commercial use allowed | No GPL or non-commercial dependencies in the shipped binary |
| Rigid models only | No skeletal animation pipeline; pose is a single transform on the root |

## System map

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ Avatar Studio process (single application process)                           │
│                                                                              │
│  ┌────────────┐   frames    ┌──────────────┐   pose+landmarks  ┌───────────┐ │
│  │  Capture   │────────────▶│   Tracking   │──────────────────▶│  FaceRig  │ │
│  │  (native)  │             │  (MediaPipe) │                   │ (C++ math)│ │
│  └─────┬──────┘             └──────────────┘                   └─────┬─────┘ │
│        │ camera textures (ring, timestamped)                        │       │
│        ▼                                                            ▼       │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ Godot render thread: avatar scene + GPU face-decal compositor        │   │
│  │   SubViewport (RGBA, transparent) ──▶ preview (main window)          │   │
│  └──────────────────────────────────┬───────────────────────────────────┘   │
│                                     │ ViewportTexture RID                   │
│                                     ▼                                       │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ Output Manager (C++, worker threads)                                 │   │
│  │   async readback ─▶ frame queue (drop-oldest) ─▶ transports          │   │
│  └───┬───────────────┬────────────────┬─────────────────┬───────────────┘   │
└──────┼───────────────┼────────────────┼─────────────────┼───────────────────┘
       │               │                │                 │
   Spout2 (Win)    Syphon (mac)    PipeWire (Linux)   OS virtual camera
       │               │                │                 │
     OBS            OBS / VJ        OBS / GStreamer   ┌────┴─────────────────┐
                                                      │ Windows: MF media    │
                                                      │  source DLL (loaded  │
                                                      │  by consumer apps)   │
                                                      │ macOS: CMIOExtension │
                                                      │  system-extension    │
                                                      │  process             │
                                                      │ Linux: PipeWire node │
                                                      │  or v4l2loopback     │
                                                      └──────────────────────┘
```

Two properties matter more than the boxes:

1. **The camera and tracking pipeline does not depend on Godot.** It is a plain C++ library with a
   thin GDExtension adapter on top. This keeps it testable headlessly and replaceable.
2. **Virtual cameras are cross-process by nature.** On Windows the media source is a COM object
   loaded into whichever application opens the camera; on macOS the extension is a sandboxed
   system-extension process. "Render and hand the frame to a sink" is insufficient — the frame must
   cross a process boundary with explicit lifetime and sync semantics
   ([`output-and-virtual-camera.md`](output-and-virtual-camera.md#cross-process-frame-transport)).

## Process and thread model

| Execution context | Responsibility | Rules |
|---|---|---|
| Godot main thread | UI, scene graph, input, preset application | May block on user actions; never on camera or output |
| Godot render thread | Scene render, decal compositing, GPU readback submission | No allocations per frame; no locks held across frames |
| Capture thread (1) | Device I/O, format conversion, publish into texture ring | Never blocks on consumers; drops if the ring is full |
| Tracking thread (1) | MediaPipe inference on the newest frame | Reads the ring, never the capture callback; publishes latest-only |
| Rig thread (or main) | Filtering, pose integration, anchor smoothing | Fixed-rate, cheap; may run on the main thread at 60 Hz |
| Output worker (1 per transport) | Readback consumption, conversion, transport push | Bounded queue, drop-oldest, never back-pressures the render thread |

Synchronisation primitives:

- **Latest-value slots** for pose and tracking results. Losing an intermediate pose is correct;
  queuing it is not.
- **Bounded ring buffers** (capacity 2–4) for camera frames and output frames. Policy is always
  drop-oldest, never block.
- **No mutex on the render path.** Publishers write into a pre-allocated slot guarded by an atomic
  sequence number; the consumer copies a consistent snapshot.
- **Godot thread affinity is respected.** Workers never touch scene nodes or the rendering server
  directly; results are marshalled with `call_deferred` or consumed at a frame boundary.

The alternative — one thread per stage with unbounded queues — was rejected: it converts transient
slowdowns into permanent latency, which is the worst failure mode for a live avatar.

## Subsystem contracts

Interfaces are narrow and framework-free so that each subsystem is unit-testable without a window,
a camera, or a GPU.

### Capture

```cpp
struct VideoFrame {
    uint64_t      timestamp_ns;   // FrameClock, at first byte available to us
    uint32_t      sequence;
    uint32_t      width, height;
    PixelFormat   format;         // NV12, YUY2, MJPEG, BGRA8
    ColorSpace    color_space;    // BT.601 / BT.709, full / limited range
    bool          mirrored;
    FrameStorage  storage;        // CPU planes or GPU handles
    // planes / strides / native handles omitted
};

class ICameraSource {
public:
    virtual ~ICameraSource() = default;
    virtual std::vector<CameraDevice> enumerate() = 0;
    virtual bool open(const CameraConfig&) = 0;      // resolution, fps, format preference
    virtual bool start(FrameCallback) = 0;           // called on the capture thread
    virtual void stop() = 0;
    virtual CameraCapabilities capabilities() const = 0;
};
```

Detail: [`camera-capture.md`](camera-capture.md).

### Tracking

```cpp
struct TrackingResult {
    uint64_t   source_timestamp_ns;  // timestamp of the frame that produced this
    bool       valid;
    float      confidence;
    HeadPose   pose;                 // rotation quaternion + translation, camera space
    std::array<Landmark, 478> landmarks;  // normalised image coords + relative depth
    std::array<float, 52> blendshapes;    // reserved; unused in 1.0
};

class ITrackingBackend {
public:
    virtual ~ITrackingBackend() = default;
    virtual bool initialize(const TrackerConfig&) = 0;
    virtual void submit(const VideoFrame&) = 0;   // async, non-blocking
    virtual std::optional<TrackingResult> latest() = 0;
    virtual void shutdown() = 0;
};
```

Detail: [`tracking.md`](tracking.md).

### Face rig

Pure math, no I/O, no GPU. Consumes `TrackingResult` and avatar calibration; produces the
per-frame uniforms for the compositor.

```cpp
struct FaceRigOutput {
    uint64_t  source_timestamp_ns;
    Transform3D head_from_camera;    // avatar root transform
    Mat3      canonical_to_image;    // homography: face-plane UV -> camera image UV
    std::array<RegionAnchor, 3> regions;  // eye_l, eye_r, mouth (canonical-space offsets)
    float     confidence;            // 0 = hold last good pose
};

class FaceRig {
public:
    void    configure(const FaceCalibration&);
    FaceRigOutput update(const TrackingResult&, uint64_t render_time_ns);
};
```

The math is the heart of the product: [`face-compositing.md`](face-compositing.md).

### Output

```cpp
class IVideoOutput {
public:
    virtual ~IVideoOutput() = default;
    virtual bool initialize(const OutputConfig&) = 0;
    virtual bool submit(const OutputFrame&) = 0;   // non-blocking, may drop
    virtual void shutdown() = 0;
    virtual OutputStats stats() const = 0;
};
```

Detail: [`output-and-virtual-camera.md`](output-and-virtual-camera.md).

## Frame lifecycle

One rendered frame, end to end:

1. **Capture** publishes a `VideoFrame` into the camera ring with `timestamp_ns`.
2. **Tracking** takes the newest frame, runs inference (CPU, ~8–12 ms), and publishes a
   `TrackingResult` carrying the *source frame's* timestamp. Results older than the last published
   one are discarded, not queued.
3. **FaceRig** runs at render rate. It picks the tracking result whose `source_timestamp_ns` is
   newest-but-not-after the render time, integrates the pose with a One Euro filter, and computes
   the homography for *that* frame. If the newest result is stale (> `tracking_timeout`), it holds
   the last pose and lowers `confidence`.
4. **Render** draws the avatar with `head_from_camera` applied to the root, then the decal pass
   samples the camera texture ring at the index matching the tracking result's timestamp, using
   `canonical_to_image`. Pose and pixels therefore come from the same instant — this is what stops
   the face from sliding across the model during fast motion.
5. **Readback** is submitted asynchronously from the render thread
   (`RenderingDevice.texture_get_data_async`), with a completion callback keyed by instance ID so a
   freed viewport is safe.
6. **Output** workers pop from a drop-oldest ring and push to transports. A slow consumer loses
   frames; it never delays the next render.

**Why the timestamp pairing matters.** The naive implementation applies the newest pose to the
newest pixels. Under motion those are 1–3 frames apart, and because the decal's sample position is
derived from the pose, the error appears as the face sliding against the model — the single most
noticeable artifact in this class of application. Pairing by timestamp costs a 2–4 frame texture
ring and removes the artifact at its source.

## GDExtension binding rules

The GDExtension boundary is cheap per call and expensive per marshalled value. Rules:

1. **The data plane never crosses the boundary.** Frames, landmarks and decal uniforms stay in C++
   and reach shaders as uniforms set by C++ or as RD resources. No `Array[Vector2]` per frame, no
   `PackedByteArray` of pixels.
2. **GDScript owns presentation, not logic.** Menus, dialogs, list views, settings forms. Any loop
   that runs per frame or per vertex is C++.
3. **One object per subsystem**, exposed as a `Node` only where scene-tree lifetime helps
   (`AvatarRuntime`, `CaptureService`, `OutputService`). Everything else is a `RefCounted` handle.
4. **C++ owns the state machine.** Godot observes; it does not drive capture/tracking state
   transitions, because those must also work headlessly in tests.
5. **No exceptions across the boundary.** The C++ core may throw internally; the adapter converts to
   error codes and Godot `push_error`.

## Rendering strategy

- **One avatar viewport.** A `SubViewport` at output resolution (default 1920×1080), transparent
  background, `render_target_update_mode = ALWAYS`. The preview window shows a scaled copy; it is
  not a second render.
- **Renderer:** Forward+ (Vulkan on Windows/Linux, Metal on macOS). The Compatibility (OpenGL)
  renderer is unsupported because GPU readback and RD resource access are required. This is a
  documented limitation, surfaced in the UI rather than as a silent black stream.
- **Camera as texture.** YUV formats are converted to RGB in a shader (BT.601/709 + range aware)
  rather than on the CPU. Full-resolution frames are never touched by the CPU in the steady state.
- **Decal pass.** Drawn after opaque geometry with a depth-bounds test; see
  [`face-compositing.md`](face-compositing.md#projection-modes).
- **Resolution independence.** The avatar viewport resolution, the camera resolution and the output
  resolution are independent. Tracking runs on a downscaled copy (typically 480×480 or the model's
  input size); compositing samples the full-resolution camera texture.

### Colour and alpha

| Stage | Space | Notes |
|---|---|---|
| Camera texture | sRGB-encoded, YUV or BGRA | Decoded in shader, converted to linear |
| Working space | Linear, premultiplied alpha | All blending in linear |
| OBS output | sRGB-encoded, alpha configurable | Default premultiplied; straight-alpha option because Spout2/Syphon/OBS disagree |
| Virtual camera | sRGB-encoded, **opaque** | Background composited before handoff; no transport carries alpha reliably |

Consequences:

- The decal shader must convert camera sRGB to linear *before* blending, or the face is visibly
  washed out against a linear-lit model.
- Alpha handling is per-transport and configurable, because a mismatch shows up as either a black
  halo or a bright fringe over gameplay. A built-in checkerboard test pattern exists to diagnose it.
- HDR output is `Deferred`. Godot 4.7 supports HDR on desktop, but no OBS or virtual-camera path in
  scope consumes it; shipping SDR only avoids a colour-management rabbit hole in 1.0.

## Latency budget

"Latency" is defined twice, because one definition is unachievable and the other is measurable:

| Definition | Start | End | Target |
|---|---|---|---|
| **App-side latency** | Frame arrives in the capture callback | Corresponding frame is handed to the transport | ≤ 70 ms median, ≤ 100 ms p95 |
| **End-to-end latency** | Photon hits the sensor | Frame displayed by the consumer | Measured and reported, no target |

Camera and ISP latency (typically 30–60 ms, sometimes more with auto-exposure and noise reduction)
happens before the app sees a frame and is outside our control. Reporting only the end-to-end number
would make the app look worse than it is; reporting only the app-side number would hide the user's
real experience. Both are instrumented.

App-side budget at 1080p60 on the reference hardware:

| Stage | Budget | Notes |
|---|---|---|
| Capture callback → ring publish | 1 ms | Format conversion deferred to GPU |
| Tracking inference (CPU, 480×480) | 8–12 ms | Dominant cost; runs on its own thread, overlapped |
| Pose filter + rig | < 0.5 ms | Fixed-rate math |
| Render (scene + decals) | 6 ms | 16.6 ms frame budget leaves headroom |
| Async readback submit | 0.3 ms | Submission only; completion is pipelined |
| Readback completion → transport | 2–4 ms | Includes one-frame queue depth |
| **Total added latency** | **~20–25 ms** | Plus camera latency and up to one tracking interval |

The pipeline is deliberately **not** pipelined to minimise latency at the cost of throughput; the
target is smoothness plus responsiveness, and the tracking interval (33 ms at 30 fps camera) sets a
floor on how fresh the pose can be.

## Failure handling

| Failure | Detection | Behaviour |
|---|---|---|
| Camera unplugged | Capture thread error + device list change | Hold last frame with a "camera lost" overlay on preview; auto-reconnect on device return |
| Camera busy (another app) | Open failure | Clear message naming the likely owner; retry button |
| Face lost | Confidence below threshold for N frames | Hold pose with a slow drift to neutral; face content freezes rather than jumps |
| Tracking slower than camera | Queue depth | Drop frames before inference; never accumulate |
| Transport consumer disconnects | Backend reports zero consumers | Stop readback entirely; resume on connect (saves GPU bandwidth) |
| Readback failure | RD error | Fall back to synchronous readback at reduced rate; log once |
| GPU device lost | Renderer error | Suspend output, rebuild viewport, resume; if repeated, disable decals and continue |
| Virtual camera still registered after crash | Startup check | Offer "repair camera registration"; unregister on clean exit always |
| Disk full while saving preset | Write error | Atomic write to temp + rename; never leave a half-written package |

Every failure above has a user-visible string and a support-code in the log. Silent degradation is
a defect.

## Repository layout

```
avatar-studio/
├── app/                       # Godot project (scenes, UI, shaders, bundled assets)
│   ├── project.godot
│   ├── scenes/                # main, editor, live, settings
│   ├── ui/
│   ├── shaders/               # face_decal_*.gdshader, yuv_convert.gdshader
│   └── assets/
├── native/
│   ├── core/                  # frame types, clock, ring buffers, logging, config
│   ├── capture/               # windows/ macos/ linux/ + test_pattern/
│   ├── tracking/              # itracking_backend.h, mediapipe/ (C-ABI wrapper)
│   ├── rig/                   # pose filter, face frame, homography, calibration
│   ├── avatar/                # package load/save/validate, anchors, presets
│   ├── output/                # manager, queue, transports/{spout,syphon,pipewire,mf,cmio,v4l2}
│   └── gdextension/           # bindings only; no business logic
├── platform/
│   ├── windows/vcam/          # MF media source COM DLL + registrar
│   ├── macos/vcam/            # CMIOExtension target + host app glue
│   └── linux/vcam/            # PipeWire node helper / v4l2loopback glue
├── models/                    # tracker model files + checksums (no weights in git LFS unless audited)
├── examples/                  # CC0 avatars and props with provenance manifests
├── tests/                     # unit/, integration/, golden/, performance/, fixtures/
├── scripts/                   # build, package, sign, notarize, model-fetch
├── docs/design/               # this document set
├── CMakeLists.txt
└── LICENSE
```

The `platform/*/vcam` directories are the only components that may run outside the main process.
They are small, separately versioned, and communicate through a documented shared-memory contract.

## Build and distribution

| Platform | Toolchain | Artifact |
|---|---|---|
| Windows 10/11 x64 | MSVC 2022 + CMake + Ninja; MediaPipe via Bazel | Signed installer (MSI or EXE), per-machine for virtual-camera registration |
| macOS 13+ universal | Xcode + CMake; Bazel for MediaPipe | Signed, notarised `.app` bundle containing the CMIOExtension; DMG |
| Linux x86_64 | GCC/Clang + CMake; Bazel for MediaPipe | AppImage + DEB/RPM; PipeWire node needs no privileges |

Build rules:

- **One pinned toolchain per platform**, recorded in CI images. MediaPipe's Bazel build is the least
  reproducible part of the stack and must not float.
- **MediaPipe is built as a separate shared library behind a narrow C ABI.** This isolates its
  bundled Abseil/Protobuf/OpenCV from the rest of the binary, prevents ODR/ABI conflicts, and makes
  the backend replaceable. It also means the main binary does not need to be rebuilt when the
  tracker changes.
- **godot-cpp is pinned to the engine version** (4.7 at time of writing) and upgraded only together
  with the engine.
- **Dependency licenses are audited at build time.** A CI step fails the build if a new dependency
  appears without an entry in the manifest.

## Extensibility

Three seams are designed for replacement, and each is exercised by at least one non-default
implementation so that it cannot rot:

| Seam | Default | Alternate implementation used in tests |
|---|---|---|
| `ICameraSource` | Native OS capture | `TestPatternSource`, `ReplaySource` (recorded clip) |
| `ITrackingBackend` | MediaPipe Face Landmarker | `ScriptedTracker` (canned landmark tracks) |
| `IVideoOutput` | Spout2 / Syphon / PipeWire | `NullOutput`, `FileOutput` (writes a PNG sequence) |

If a seam has only one implementation, it is not an abstraction, it is indirection.

## Risks

| Risk | Impact | Likelihood | Mitigation | Gate |
|---|---|---|---|---|
| MediaPipe C++ builds are unmaintainable on Windows/macOS | Blocks the entire tracking plan | Medium | M0 spike builds all three platforms before anything else; C-ABI wrapper; fallback tracker (ONNX Runtime + landmark model) | M0 exit |
| CPU-only inference too slow on low-end hardware | Cannot hit 60 fps | Medium | Reduced input resolution, tracking at 30 Hz while rendering at 60 Hz, quality presets | M1 |
| Zero-copy GPU sharing infeasible across Godot's renderer and the transports | Higher latency, higher CPU | High (assume CPU path) | Ship async CPU readback first; treat zero-copy as an optimisation with per-platform feasibility notes | M3 |
| Windows virtual camera requires per-machine COM registration | Installer friction, admin prompt | Certain | Per-machine installer; document exactly what is written; graceful fallback to OBS virtual camera | M4 |
| macOS extension approval friction | Users see a blocked extension | Certain | In-app install flow, clear System Settings instructions, detection + repair | M4 |
| Face looks wrong on curved or extreme geometry | Quality failure on some models | Medium | Surface-projection mode with per-model calibration; documented modelling guidance | M2 |
| Colour mismatch between face and model | Uncanny, low quality | High without care | Linear pipeline, per-region gain, calibration step, reference test scenes | M2 |
| Scope creep into a general 3D editor | Schedule | High | Non-goals are explicit; editor edits configuration only | continuous |

## Verification notes

Checked against upstream sources on 2026-11-20:

- Godot 4.7.2 stable (2026-08-18); 4.8 in beta.
- `CameraServer` implemented on Linux, Android, macOS, iOS only.
- MediaPipe desktop C++: Windows experimental (MSYS2/Bazel/OpenCV pinning); GPU inference not
  supported on Windows or macOS desktop.
- `MFCreateVirtualCamera`: Windows 11 build 22000+, `HKLM`-registered COM media source, packaged
  apps need the webcam capability, must not be invoked on the UI thread.
- CoreMediaIO camera extensions: system extension embedded in the app bundle, app-group Mach
  service, sandboxed extension process, install requires user approval; App Store delivery supported.
- PipeWire `Video/Source` + `media.role=Camera` is a viable user-space virtual camera; OBS ships a
  PipeWire camera source (beta).
- `SpectatorStreamExporter`: MIT, Godot 4.7, Forward+/Mobile only, CPU readback (~500 MB/s at
  1080p60), async readback with drop-oldest ring.

Not verified, and treated as assumptions until M0/M3: achievable MediaPipe inference latency per
platform; feasibility of zero-copy texture sharing per API pair; OBS PipeWire camera source
behaviour across versions; exact alpha expectations of each transport.
