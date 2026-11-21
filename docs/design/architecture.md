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

**Single instance.** The application takes a per-user named lock at startup (named mutex on Windows,
a lock file on macOS/Linux). A second launch forwards an `activate` message, optionally with an
avatar or show path, to the running instance and exits. `--new-instance` is a testing escape hatch:
a second instance uses a distinct shared-memory name and cannot register the same virtual camera, so
it runs with the virtual camera disabled. Two instances competing for one camera produce the normal
`CAP-002` busy state, not a crash.

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
    Transform3D head_from_camera;    // full pose; 1.0 applies rotation only, to the head region
    Mat3      canonical_to_image;    // homography: face-plane UV -> camera image UV
    std::array<RegionAnchor, 3> regions;  // eye_l, eye_r, mouth (canonical-space offsets)
    float     confidence;            // 0 = invalid; the output gate blanks
    bool      features_valid;        // both eyes and the mouth are tracked
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
   the homography for *that* frame. A stale result is held for at most 150 ms to bridge a dropped
   frame; beyond that the frame is marked invalid and the output blanks. It never decays to a neutral
   pose, because a neutral pose is still a face.
4. **Render** draws the avatar with the head rotation applied to the head region only, inside a fixed
   presentation transform (`view_from_model`, 180° yaw plus fit) that places the whole model. The body
   stays still. Head translation is measured but not applied in 1.0. The decal pass then samples the
   camera texture ring at the index matching the tracking result's timestamp, using
   `canonical_to_image`, multiplied by the face matte. Pose and pixels therefore come from the same
   instant — this is what stops the face from sliding across the model during fast motion.
5. **Readback** is submitted asynchronously from the render thread
   (`RenderingDevice.texture_get_data_async`), with a completion callback keyed by instance ID so a
   freed viewport is safe.
6. **Output gate** submits the composited frame only when `features_valid` is true; otherwise it
   submits a blank frame. There is no path that samples the camera into the output outside the decal
   mask.
7. **Output** workers pop from a drop-oldest ring and push to transports. A slow consumer loses
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
- **Head motion.** Rotation-only in 1.0, applied to the head region (a node or a neck cut) so the
  body stays still. The whole model is placed by a fixed presentation transform (180° yaw plus fit);
  the camera is perspective at 35° vertical FOV. Fit, zoom, offset and margin are per-avatar with
  Show overrides ([`face-compositing.md`](face-compositing.md#head-motion-and-placement)).
- **Output gate.** The output is the rendered avatar or a blank frame. No passthrough, no frozen
  face, no camera fallback; the invariant is tested
  ([`testing-and-quality.md`](testing-and-quality.md#output-invariant)).
- **Face matte.** Chroma key when a green or blue screen is detected, otherwise a software
  segmentation matte, applied only inside the decal mask
  ([`face-compositing.md`](face-compositing.md#face-matte)).
- **Frame pacing.** The output cadence is fixed (default 60 Hz) and independent of the preview
  window's refresh rate. `Engine.max_fps` is pinned to the output rate; vsync is enabled only when
  the display refresh is an integer multiple, otherwise a monotonic deadline scheduler paces the
  output and the preview may tear. A late frame is dropped, never doubled. Rendering continues while
  a consumer is connected even if the window is hidden; Godot's low-processor mode and macOS App Nap
  are disabled while live. The exact engine knobs are validated in M1; the requirement is the
  cadence, not the mechanism.
- **Lighting.** Four environment presets plus key-light overrides
  ([`avatar-package-format.md`](avatar-package-format.md#appearance-and-lighting)). The same key
  direction and intensity feed the decal's diffuse term, so model and face are lit together.

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
| Camera unplugged | Capture thread error + device list change | Output blanks; preview shows "camera lost"; auto-reconnect on device return |
| Camera busy (another app) | Open failure | Clear message naming the likely owner; retry button |
| Face lost | Confidence below threshold beyond the 150 ms hold | Output blanks; preview shows the state. No drift to neutral, no frozen face (`TRK-002`) |
| Eyes or mouth missing | `features_valid` false | Output blanks; the gate requires both eyes and the mouth (`TRK-008`) |
| Software matte unavailable | Segmentation load failure | Region mask only; log once (`TRK-009`) |
| Head region missing | Validation / calibration | Derive a default and warn; editing continues (`RIG-006`, `AVT-117`) |
| Tracking slower than camera | Queue depth | Drop frames before inference; never accumulate |
| Transport consumer disconnects | Backend reports zero consumers | Stop readback entirely; resume on connect (saves GPU bandwidth) |
| Readback failure | RD error | Fall back to synchronous readback at reduced rate; log once |
| GPU device lost | Renderer error | Suspend output, rebuild viewport, resume; if repeated, disable decals and continue |
| Virtual camera still registered after crash | Startup check | Offer "repair camera registration"; unregister on clean exit always |
| OS suspend / resume | Session and device notifications | Reset the frame ring, tracker timestamp base and output queues; reopen the camera; re-register the virtual camera if needed; consumers see no-signal meanwhile |
| Session locked | OS session notification | Stop capture and output, emit no-signal, resume on unlock (`SEC-002`). A locked screen must not keep a live camera feed |
| Window hidden or occluded | Window state + consumer state | Keep rendering while a consumer is connected; disable low-processor/App Nap. With no consumer, throttle and keep capture alive |
| Second instance launched | Named lock held | Forward `activate` and exit; never contend for the camera or virtual camera |
| Settings, profile, or library index corrupt | Parse/validation | Rebuild from defaults, backup, or directory; keep a `.bad` copy (`CFG-001`, `CFG-005`) |
| Library import copy fails | I/O error | Remove `.incoming-*`; report the path and free space (`AVT-110`) |
| Diagnostics export fails | I/O error | Keep the bundle local; name the path (`SEC-003`) |
| Disk full while saving preset | Write error | Atomic write to temp + rename; never leave a half-written package (`CFG-002`) |

Every failure above has a user-visible string and a support code in the log. The registry is
[`diagnostics.md`](diagnostics.md); codes are stable and never reused. Silent degradation is a
defect.

## Repository layout

```
avatar-studio/
├── app/                       # Godot project (scenes, UI, shaders, bundled assets)
│   ├── project.godot
│   ├── scenes/                # main, editor, live, settings
│   ├── bin/                   # built GDExtension + avatar_studio.gdextension
│   ├── tests/                 # headless shell smoke (smoke.gd)
│   ├── ui/
│   ├── shaders/               # face_decal_*.gdshader, yuv_convert.gdshader
│   └── assets/
├── native/
│   ├── core/                  # frame types, clock, ring buffers, logging, diagnostics, config
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

## User data

The managed library, FaceProfiles, shows, settings, and logs live outside the repository and outside
the `.avatar` package. The layout, permissions, migration, and deletion rules are in
[`user-data.md`](user-data.md). The app never writes into its install directory and never modifies
the original source package; edits go to the managed library copy or to a package the user
explicitly saves. Every write uses an atomic replace.

## Build and distribution

### Supported platforms

| Platform | Support | Blocking QA | Native virtual camera |
|---|---|---|---|
| Windows 11 22H2+ (build 22621+) | Yes | Yes | Yes (build 22000+) |
| Windows 10 22H2+ | Best-effort | No | No; OBS Virtual Camera fallback |
| macOS 13+ (Intel, Apple Silicon) | Yes | Current + previous major | Yes (system extension) |
| Linux x86-64, glibc 2.35+, PipeWire 0.3.50+ | Yes | Ubuntu 24.04 LTS, Fedora current | Yes (PipeWire node); `v4l2loopback` fallback |

Windows 10 is supported for capture, preview, window capture and OBS output. It is not in the
blocking matrix: Windows 11 is the only Windows platform in CI and manual QA, and a Windows 10 bug
that cannot be reproduced there may be closed as best-effort. This is an explicit policy, not a claim
of parity. macOS 13 is the floor; blocking QA runs the current and previous major, and older versions
are best-effort. Linux is tested on the current Ubuntu LTS and Fedora; other distributions are
best-effort. No 32-bit, ARM64 Windows/Linux, mobile, or web targets in 1.0; ARM64 is post-1.0.

### Toolchain and artifacts

| Platform | Toolchain | Artifact |
|---|---|---|
| Windows 11 x64 (Windows 10 best-effort) | MSVC 2022 + CMake + Ninja; MediaPipe via Bazel | Signed per-machine MSI; portable ZIP without the virtual camera |
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
- **Eigen is confined to `native/rig`** ([ADR-0008](adr/0008-eigen-for-rig-math.md)). It is
  header-only, pinned, uses double precision for estimation, and never appears in the GDExtension
  boundary or the tracker C ABI.
- **Dependency licenses are audited at build time.** A CI step fails the build if a new dependency
  appears without an entry in the manifest.

### Local development build

The native core builds without Godot; the shell adds the engine when it is bootstrapped:

- `scripts/bootstrap.ps1` (Windows) and `scripts/bootstrap.sh` (macOS/Linux) download every pinned
  dependency into `.tools/`, by exact URL and SHA256. Both scripts are idempotent, verify the
  installed copy, and print the resulting paths; `.tools/` is gitignored. The current pins are
  nlohmann/json 3.11.3, **Godot 4.7.2 stable**, and **godot-cpp 10.0.0-stable** built with
  `GODOTCPP_API_VERSION=4.7`. godot-cpp 10.x is versioned independently of Godot; the API version
  is the compatibility contract ([ADR-0012](adr/0012-godot-shell-bootstrap.md)).
- Bootstrap installs the engine at `.tools/godot/` and the bindings at `.tools/godot-cpp/`.
  `bootstrap -Fast` (default) fetches the whole pinned set; `bootstrap -NativeOnly` skips the engine
  and bindings for native-only work.
- CMake consumes `.tools/` and fails configuration with an actionable message when a pinned
  dependency is missing. Configure and build never touch the network.
- Targets: `avatar_core` (static library, C++20, framework-free), `avatar_core_tests`, and
  `avatar_gdextension` (shared library, added only when `.tools/godot-cpp` exists). The adapter is
  copied to `app/bin/avatar_studio.<os>.<arch>.<ext>` for Godot to load. Options: `AVATAR_TESTS`
  (default ON), `AVATAR_GODEXTENSION` (default ON), and `AVATAR_WARNINGS_AS_ERRORS` (default ON;
  MSVC `/W4 /WX`, GCC/Clang `-Wall -Wextra -Wpedantic -Werror`). godot-cpp headers are marked as
  system headers so the warning policy applies to our code, not generated bindings.
- Windows uses the default Visual Studio generator; Linux/macOS use the default CMake generator.
  Ninja is not required.
- `scripts/verify.ps1` / `scripts/verify.sh` are the verification contract. `-Fast` bootstraps,
  configures, builds RelWithDebInfo and runs CTest label `fast`; `-Full` adds the complete native
  suite and `tests/gates/TNN.ps1`; `-Task TNN` selects the task label and gate. From T02 the
  contract also runs `app/tests/smoke.gd` headlessly and requires the engine and adapter for T02 and
  later. `-Task T01` uses the native-only bootstrap and remains usable without Godot.
- The shell baseline (`app/project.godot`, `app/scenes/main.tscn`, `app/bin/avatar_studio.gdextension`)
  is Forward+ with a fixed 1920x1080 transparent `SubViewport` using `render_target_update_mode =
  ALWAYS`; `AvatarRuntime` is the only GDExtension class and exposes a version string and a frame
  counter. The preview and pacing are T16; no per-frame GDScript is part of the shell.
- The core reads time only through `FrameClock` over an injected `TimeSource`; tests drive a fake
  source and no core code reads wall-clock time.

See [ADR-0011](adr/0011-pinned-local-dependencies.md) and
[ADR-0012](adr/0012-godot-shell-bootstrap.md).


### Install and uninstall contract

The installer's application-specific writes are exactly this; standard OS bookkeeping (MSI installer
keys, quarantine attributes) is not listed. The list is the contract QA snapshots before install and
after uninstall ([`testing-and-quality.md`](testing-and-quality.md#manual-qa-checklist-per-release)).

**Windows (per-machine MSI):**

| Path or key | Content |
|---|---|
| `%ProgramFiles%\Avatar Studio\` | `AvatarStudio.exe`, GDExtension and tracker DLLs, models, examples, `LICENSE`, `NOTICE` |
| `%ProgramFiles%\Avatar Studio\vcam\AvatarStudioVCam.dll` | Signed Media Foundation media source (COM) |
| `HKLM\SOFTWARE\Classes\CLSID\{GUID}\InprocServer32` | Media source path, `ThreadingModel=Both` |
| `HKLM\SOFTWARE\Classes\CLSID\{GUID}` | Friendly name `Avatar Studio Camera` |
| Frame-server store | Virtual camera registration through `MFCreateVirtualCamera` |
| `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{GUID}` | MSI uninstall entry |
| Start Menu | Shortcut |
| — | No service, no driver, no firewall rule, no per-user COM registration |

**macOS:**

| Path | Content |
|---|---|
| `/Applications/Avatar Studio.app` | App, GDExtension, tracker, models, examples |
| `Contents/Library/SystemExtensions/<bundle>` | CMIOExtension |
| App group `group.<team>.avatarstudio` | Shared container for the ring/IOSurface |
| `~/Library/Application Support/AvatarStudio/` | User data |
| `~/Library/Logs/AvatarStudio/` | Logs |
| — | No LaunchAgent, no LaunchDaemon, no kernel extension |

**Linux:**

| Path | Content |
|---|---|
| `/opt/avatar-studio/` | App, libraries, models, examples |
| `/usr/bin/avatar-studio` | Symlink |
| `/usr/share/applications/avatar-studio.desktop` | Desktop entry |
| `/usr/share/icons/hicolor/…` | Icons |
| `/usr/share/doc/avatar-studio/` | `LICENSE`, `NOTICE` |
| — | No service, no udev rule, no kernel module; `v4l2loopback` is optional and user-installed |

Uninstall removes exactly these entries. User data is left in place unless the Windows installer's
*Remove user data* checkbox is selected. The app is fully functional without the virtual-camera
component; a portable ZIP/AppImage omits it entirely.

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
| Windows 10 is supported but not in CI | Regressions reach a supported platform unnoticed | Best-effort policy; community reports; no blocking gate; Windows 11 is the reference | continuous |
| Scope creep into a general 3D editor | Schedule | High | Non-goals are explicit; editor edits configuration only | continuous |

## Verification notes

Checked against upstream sources on 2026-11-20:

- Godot 4.7.2 stable (2026-08-18); 4.8 in beta.
- godot-cpp 10.0.0-stable (2026-09-15) is the current bindings release. Since v10 it is versioned
  independently of Godot and targets 4.3+ through `GODOTCPP_API_VERSION`; the 4.7 API JSON ships in
  the release.
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
