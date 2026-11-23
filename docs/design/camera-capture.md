# Camera Capture

**Status:** proposal; the Windows Media Foundation backend is implemented (T10), the
renderer-facing capture contract is audited (T15), and the timestamp-paired GPU texture ring is
implemented (T24); macOS/Linux are planned · **Last updated:** 2026-11-22
**Decision records:** [ADR-0002](adr/0002-native-camera-capture.md) (native capture),
[ADR-0020](adr/0020-capture-state-machine-and-mf-backend.md) (state machine and backend seam),
[ADR-0034](adr/0034-timestamp-paired-camera-ring.md) (timestamp-paired texture ring)

Capture is deliberately not delegated to Godot's `CameraServer`, which is implemented only on
Linux, Android, macOS and iOS. Even where it exists, it does not give us source timestamps, format
control, or buffer access at the level the pipeline needs.

---

## Requirements

| Requirement | Why |
|---|---|
| Source timestamps, monotonic | Pose/pixel pairing; latency measurement |
| Explicit pixel format negotiation | Avoid hidden CPU conversions; NV12 is the common denominator |
| Full-resolution frames available to the GPU | Decals sample at camera resolution, not tracking resolution |
| Device selection with stable IDs | Survive reconnects and reordering |
| Hot-plug events | Recover without a restart |
| Exposure/WB lock where supported | Prevent mid-stream colour pops |
| No dependency on a window or GPU context | Headless tests must work |

**Lens distortion is not corrected in 1.0.** The pipeline assumes a pinhole camera and requires no
intrinsics; tracking and decal sampling share the same uncorrected frame. The error and its limits
are in [`face-compositing.md`](face-compositing.md#lens-distortion). Optional per-camera calibration
is `Deferred`.

## Frame model

```cpp
enum class PixelFormat { NV12, YUY2, MJPEG, BGRA8, RGBA8 };

struct VideoFrame {
    uint64_t    timestamp_ns;   // FrameClock; source timestamp when available
    uint64_t    arrival_ns;     // FrameClock at first byte available to us
    uint32_t    sequence;
    uint32_t    width, height;
    PixelFormat format;
    ColorSpace  color_space;    // BT.601/BT.709, full/limited range, sRGB transfer
    bool        mirrored;       // capture-layer mirroring, if any
    FrameStorage storage;       // CPU planes, or native GPU handle
    uint32_t    strides[3];
    const uint8_t* planes[3];   // CPU path
    void*       native_handle;  // D3D11 texture / CVPixelBuffer / dmabuf fd
};
```

Rules:

- Frames are **immutable** once published.
- Ownership is explicit: the producer retains the buffer until the consumer releases it, or the
  producer copies into a ring slot it owns. The chosen policy is **ring-owned slots** (capacity 4),
  which keeps lifetime trivial and bounds memory to `4 × frame size` (~33 MB at 1080p BGRA).
- `timestamp_ns` is the camera's own timestamp mapped onto `FrameClock` when the API provides one;
  `arrival_ns` is always available. Latency instrumentation reports both.
- `FrameRing` does not reorder or reject timestamps. A source must publish non-decreasing
  `timestamp_ns` values; `CameraSource` guarantees that mapping and re-anchors on a regression
  (`CAP-005`). T24's timestamp-paired ring may rely on this ordering, not on the ring sorting for it.

The T04 implementation is `native/capture/include/avatar/capture/frame_ring.h`. A `FrameRing` owns
`capacity` (default 4) fixed slots. The producer reserves a slot that is neither the newest nor
pinned, writes pixels directly into it, and publishes. A consumer calls
`try_acquire_latest()` and holds the returned `FrameLease` (a pin) for the duration of its read; the
lease destructor releases it. If every reusable slot is pinned, the producer drops the frame and
increments `dropped()`. `VideoFrame::planes` point into slot storage, so a frame is never copied
implicitly. The same interface is what T10's real Windows backend will implement.

## Format negotiation

Preference order, resolved against device capabilities:

1. `NV12` at target resolution — smallest bandwidth, GPU-convertible, universal on Windows/macOS.
2. `YUY2` — common on Linux/UVC.
3. `MJPEG` at 1080p60 — required by many cheap cameras that cannot do uncompressed 1080p60; decode
   is GPU-side where possible, otherwise one CPU decode per frame is accepted with a warning.
4. `BGRA8` — only if nothing else exists (some virtual cameras).

Resolution/rate policy:

| Priority | Mode | Rationale |
|---|---|---|
| 1 | 1280×720 @ 60 | Best motion fidelity for the face; cheapest bandwidth per second |
| 2 | 1920×1080 @ 30 | Sharper decals, steppier content |
| 3 | 1920×1080 @ 60 | Best of both, often unavailable over USB 2 |
| 4 | 640×480 @ 30 | Last resort; visibly soft |

The user can override in settings; the default is chosen from measured device capabilities rather
than assumed. A camera that advertises 1080p60 but delivers 12 fps will be demoted automatically
(after 3 s of sustained shortfall) and the change is shown, not silent.

### T26 app preference filter

The live app keeps this order but drops formats the T24 texture ring cannot
upload: `AvatarLivePipeline` builds its `CameraConfig::preferences` from
`default_camera_preferences()` and keeps only NV12 and BGRA8. YUY2 and MJPEG
remain valid capture formats for the `CameraSource` and the M0 harness, but they
have no shader path in the decal yet, so a throughput step-down must not land on
them. The C920 on the T26 host advertises 1280×720@10 YUY2 and 1920×1080@5 YUY2
but only 640×480@30 NV12 at a sustainable rate; the app therefore settles on
640×480@30 NV12 after the 1080p mode fails the 30 fps shortfall check. The
negotiated input format is recorded in the soak JSON; the 1080p60 claim is about
the output pipeline, not the camera sensor.

## Per-platform backends

### Windows — Media Foundation

- `IMFSourceReader` in async mode (`IMFSourceReaderCallback`) so the capture thread never blocks on
  a synchronous `ReadSample`.
- Preferred subtype `MFVideoFormat_NV12`; `MF_MT_FRAME_SIZE` and `MF_MT_FRAME_RATE` set explicitly
  rather than relying on `MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING`, which silently
  converts and adds latency.
- GPU path: `MF_SOURCE_READER_D3D_MANAGER` with our D3D11 device. Samples then arrive as
  `ID3D11Texture2D`, avoiding a CPU copy. Whether Godot's renderer can consume that texture
  directly is an M1 prototype question (see below).
- Timestamps: `IMFSample::GetSampleTime()` (100 ns units) mapped to `FrameClock`.
- Device identity: the device's symbolic link (`MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK`),
  which is stable across reconnects.
- Known friction: many cameras are exclusive-access; opening one already in use fails with a
  generic HRESULT. Translate to a message naming the likely culprit (Teams, Zoom, OBS, browser).
- Windows 10 vs 11: no difference for capture. (The virtual-camera side is a different story.)

#### Implemented Windows backend (T10)

The backend is split in two ([ADR-0020](adr/0020-capture-state-machine-and-mf-backend.md)):

- **`CameraSource`** (`native/capture/src/camera_source.cpp`) implements `ICameraSource` and owns all
  policy: device resolution, negotiation, the T04 ring slots, timestamp mapping, the watchdog, the
  2-second reconnect poll, suspend/resume and the CAP events.
- **`ICameraDeviceBackend`** (`native/capture/include/avatar/capture/camera_device.h`) is the
  platform seam. `native/capture/windows/mf_capture.cpp` implements it with an async
  `IMFSourceReader`; the unit tests use a fake backend; macOS and Linux will implement the same
  interface.

What is true today:

- `IMFSourceReader` is created with `MF_SOURCE_READER_ASYNC_CALLBACK`; `IMFSourceReaderCallback`
  receives samples on Media Foundation's work-queue thread, which copies directly into a ring slot.
  `MF_MT_FRAME_SIZE`, `MF_MT_FRAME_RATE` and the subtype are set explicitly.
  `MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING`, the legacy video processor and the D3D manager
  are deliberately not used, so a rejected type is a step-down, never a hidden conversion.
- Device ids are the symbolic link. `--list-cameras` enumerates without opening; `--modes` also
  activates each device to list its native types.
- Negotiation is format-first: every NV12 resolution in the policy order, then YUY2, then BGRA8.
  MJPEG is listed in capabilities but is not selected until a decoder exists (a later task): the
  ring owns fixed-size CPU formats only. A rejected mode emits `CAP-003`; the chosen mode is
  reported. The T24 texture ring accepts NV12 and BGRA8/RGBA8; YUY2 and MJPEG are rejected by the
  uploader until a packed-YUV shader or decoder lands.
- A mode that delivers less than 80% of its negotiated rate for 3 s is demoted to the next
  preference and the change is emitted as `CAP-003`. This is the advertised-versus-delivered rule;
  an exposure-limited camera can trigger it too, which is visible and reversible.
- The first source timestamp anchors to `FrameClock`; later deltas are added and the result is
  clamped to `arrival_ns`, because a driver clock that runs faster than wall time must not place a
  frame in the future. A backwards or duplicate source timestamp re-anchors to arrival, keeps the
  mapped timeline monotonic and emits `CAP-005` once per episode. The ring is **not** cleared on
  re-anchor: `FrameRing::reset()` is unsafe while a consumer holds a lease. Stale frames age out of
  the four-slot ring and the source state is the authority.
- The watchdog deadline is twice the **observed** frame interval, clamped to 1-5 s. A low-light
  camera delivering a few fps is not a stall. A stall restarts the stream once; a second stall steps
  down the mode and then reports `CAP-004`.
- A device error is classified from the HRESULT: exclusive access (`ERROR_GEN_FAILURE`,
  `ERROR_BUSY`, `MF_E_HW_MFT_FAILED_START_STREAMING`, `MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED`) is
  `CAP-002`; `E_ACCESSDENIED` is `CAP-006`; `MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED` and read
  failures are `CAP-001`; a format rejection is `CAP-003`. A lost or preempted device is polled every
  2 s and reopened with the previous configuration; a failed reopen emits `CAP-007` once per episode.
  A successful reopen is a `device_restored` event (a recovery, not a diagnostic condition).
- `CameraSource::suspend()`/`resume()` are the OS power hooks: suspend releases the device and resets
  the timestamp base; resume reopens the previous device and mode. The app's power-notification
  handler (T41/T46) calls them.
- `avatar_list_cameras --capture <seconds>` is the manual smoke. It reports the negotiated mode,
  frame count, ring drops, restarts, timestamp resets, monotonicity and the callback-to-publish
  latency (`publish_latency()`). Measured on a Logitech C920 at 640x480 NV12: p50 27 us, p95 40 us,
  max 103 us over 866 frames/60 s (see `docs/progress/T10.md`).

#### Manual smoke checklist (real camera or OBS virtual camera)

Run after a change to capture, and before the M1 gate and any release gate:

1. Close applications that may hold the camera (Teams, Zoom, OBS, browser tabs).
2. `avatar_list_cameras --list-cameras --modes`: the device appears with its symbolic-link id and
   NV12/YUY2 modes. No camera is a valid `CAP-008` result; record the manual check as pending, never
   as a pass.
3. `avatar_list_cameras --capture 60 --mode 1280x720@30 --format nv12` (or the OBS virtual camera).
   Expect frames > 0, `ring dropped: 0`, timestamps monotonic, shape matching, p95 < 1 ms.
4. Open the same camera in another app while it runs. Expect `CAP-002` (or `CAP-001` on a driver that
   reports invalidation) and a reopen after the other app releases it (`device_restored`).
5. Unplug the camera. Expect `CAP-001`, then `device_restored` within about 2 s of replugging.
   Replugging into a different mode renegotiates and emits `CAP-003` if it steps down.
6. Lock and unlock the Windows session once the app wires power/session notifications to
   `suspend()`/`resume()`; capture must stop while locked and resume on unlock.

### macOS — AVFoundation

- `AVCaptureSession` + `AVCaptureVideoDataOutput`, `alwaysDiscardsLateVideoFrames = true`,
  `kCVPixelFormatType_420YpCbCr8BiPlanarFullRange`.
- `CVPixelBuffer` backed by `IOSurface`, so a Metal texture can be created with
  `CVMetalTextureCache` without a copy. This is the cleanest GPU path of the three platforms.
- Timestamps: `CMSampleBufferGetPresentationTimeStamp`, converted with the host clock timebase.
- Device identity: `AVCaptureDevice.uniqueID`.
- Permissions: `NSCameraUsageDescription` plus TCC. A denied permission is a distinct, actionable
  state (deep link to System Settings), not a generic failure.
- Session reconfiguration (resolution change, device swap) must be wrapped in
  `beginConfiguration`/`commitConfiguration` and must not run on the main thread while the session
  is running.

### Linux — V4L2 first, PipeWire where required

Two entry points, one interface:

- **V4L2** (`/dev/video*`, `mmap` buffers, `VIDIOC_*` ioctls): the reliable default. Supports
  `V4L2_PIX_FMT_NV12`, `YUYV`, `MJPEG`; buffer timestamps via `v4l2_buffer.timestamp` with
  `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC`; `VIDIOC_EXPBUF` gives dmabuf handles for a future zero-copy
  path.
- **PipeWire** (camera portal or direct node): required inside Flatpak/Snap and increasingly the
  only path on Wayland desktops, where the portal mediates camera access. Format negotiation is a
  `SPA_PARAM_EnumFormat` exchange; frames arrive as buffers that may be dmabuf-backed.

Selection: try V4L2 for a directly accessible device; if the sandbox or desktop requires the
portal, use PipeWire. The choice is visible in the settings panel, because it changes which
permission prompt the user sees.

Device identity: `/dev/v4l/by-id/*` symlinks, falling back to the USB bus path.

## Permissions and privacy gates

| Platform | Gate | Handling |
|---|---|---|
| Windows 10/11 | Settings → Privacy → Camera, per-app toggle | Detect `E_ACCESSDENIED`, link to the settings page |
| macOS | TCC prompt, `NSCameraUsageDescription` | Explain before prompting; handle denied and "reset" states |
| Linux | Portal prompt, or `video` group membership for raw V4L2 | Explain which one is in play |

Camera access is requested only when the user starts a preview, never at launch. No frames are
written to disk unless the user explicitly records a debug clip, and that clip is deleted on exit
unless exported.

## Frame ring and GPU upload

```
capture thread                        render thread
──────────────                        ─────────────
fill slot[i] (owned)                  read slot[i] via atomic sequence
publish sequence i                    upload/convert to camera texture
                                      (shader YUV→RGB; no CPU pass)
```

- Four slots. Producer overwrites the oldest unpinned slot; if all are pinned (a slow consumer), it
  drops and increments a counter. This cannot happen in practice at 60 fps with four slots, but the
  counter exists so the debug HUD can prove it.
- Camera frames live in **two** GPU textures: full resolution for decals, downscaled for tracking.
  The downscale is a blit, not a CPU resize.
- YUV→RGB is a shader include (`app/shaders/yuv_convert.gdshaderinc`) with BT.601/709 and
  full/limited range as uniforms, driven by `ColorSpace` from the frame. Getting this wrong produces
  the classic green/magenta cast.

### Implemented camera texture ring (T24)

`native/capture/include/avatar/capture/camera_texture_ring.h` implements the GPU-facing half of
the ring. Four slots carry the source timestamp, arrival timestamp, sequence, width/height, pixel
format, colour space and a generation counter. `publish(const VideoFrame&)` uploads through the
`ICameraTextureUploader` seam; the Godot adapter in
`native/gdextension/src/avatar_camera_ring.cpp` uploads NV12 as an R8 luma texture plus an RG8
chroma texture, and BGRA8/RGBA8 unchanged as RGBA8 with a shader R/B swap. No CPU full-frame
colour conversion occurs.

The decal calls `select(result.source_timestamp_ns, render_time_ns)`. Only an exact timestamp match
is accepted (not in the future, not older than 150 ms); a miss makes the decal blank rather than
sampling different pixels. `select_for_tracking(render_time_ns)` returns the newest slot at or
before the render clock for inference. `TrackingFrameGate` admits one inference at a time and
drops every offer while it is busy, so frames never accumulate before inference.

Ring policy:

- the producer overwrites the oldest unpinned slot and counts the wrap;
- if every slot is pinned (a slow consumer), the frame is dropped and counted;
- a format or shape change bumps the generation and invalidates the old slots, but is deferred
  while a slot is pinned so a texture being sampled is never torn;
- `reset()` is only safe with no pins and no producer.

The shared `yuv_convert.gdshaderinc` supports BT.601 and BT.709, limited and full range. The flat
and surface decal shaders select the YUV or RGB path from `camera_is_yuv` and apply
`camera_matrix`/`camera_range`. The colour-bar component test covers all four combinations to
within 0.007 sRGB. Local Compatibility measurements (1280x720 NV12, RTX 4080) are upload p50
95–325 us / p95 225–425 us and a 320x180 GPU blit + readback p50 0.43 ms / p95 1.7–2.0 ms across
runs; the p95 includes driver scheduling and is not a Forward+ budget. The exact runs are recorded
in the local `docs/progress/T24.md` workspace.

## GPU zero-copy: what is actually possible

This is the largest unproven area and is treated as research, not a plan
([ADR-0005](adr/0005-output-transports.md)).

| Platform | Capture handle | Godot renderer | Feasible path | Spike |
|---|---|---|---|---|
| Windows | D3D11 texture | Vulkan or D3D12 | D3D12 backend + shared handle; or Vulkan external memory (`VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE`) | M3 |
| macOS | IOSurface | Metal | `CVMetalTextureCache` → Metal texture, wrap as RD texture | M1 (best odds) |
| Linux | dmabuf | Vulkan | `VK_EXT_external_memory_dma_buf` import | M3 |

The v1 implementation uploads from CPU (`glTexSubImage`-equivalent through `ImageTexture` or a
`RenderingDevice` copy) and measures the cost. At 1080p NV12 that is ~3 MB per frame, ~186 MB/s at
60 fps — acceptable, and far cheaper than the 8 MB/frame RGBA output readback.

## Test sources

Capture must be testable without hardware, so the synthetic and replay sources are first-class:

| Source | Purpose | Determinism |
|---|---|---|
| `TestPatternSource` | Colour bars, moving square, timestamp burn-in, alpha checkerboard | Fully deterministic |
| `SyntheticFaceSource` | A rendered face that translates/rotates on a scripted path, with a known ground-truth pose | Fully deterministic |
| `SyntheticFaceSource` (distorted) | The same face with a synthetic barrel distortion, to quantify the accepted lens error | Fully deterministic |
| `ReplaySource` | Recorded `.y4m` clip with a sidecar pose track (`.mkv` is deferred) | Frame-exact |

`SyntheticFaceSource` is what makes CI possible: it feeds the real tracker (or the scripted tracker)
and lets golden-frame tests assert both pose accuracy and decal placement without a person in front
of a camera.

### Implemented test sources (T04)

The capture sources implement `native/capture/include/avatar/capture/icamera_source.h`, own their
`FrameRing`, and take an injected `FrameClock`; none reads a wall clock or uses unseeded randomness.
`ScriptedTracker` implements the tracking contract instead and lives in `native/tracking/`.

- **`TestPatternSource`** supports 1280×720 and 1920×1080 in NV12 and BGRA8. The card combines colour
  bars, a moving marker, a decimal timestamp burn-in and an alpha checkerboard. The marker rectangle
  is a pure function of elapsed time (`marker_rect_at()`), so a test can assert the exact pixel.
- **`SyntheticFaceSource`** renders a canonical face plane through the exact plane-induced homography
  for a scripted 6-DOF path. It emits a row-major `camera_from_head` 4x4 and 23 canonical
  correspondences with pinhole and post-distortion image points. The barrel variant applies a radial
  `k1` model to both the render and the ground truth.
- **`ReplaySource`** reads YUV4MPEG2 4:2:0/4:2:2/4:4:4 and a versioned JSON sidecar (see
  [`testing-and-quality.md`](testing-and-quality.md#replay-sidecar-format)). The sidecar supplies the
  frame timestamp and ground truth; the clock supplies `arrival_ns`.
- **`ScriptedTracker`** (`native/tracking/`) is the canned `TrackingResult` source used when a test
  needs pose/filter behaviour without inference.

The Y4M reader deliberately rejects compressed or unknown chroma rather than guessing. Large clips
are not tracked; `scripts/gen_fixtures.ps1 -Large` generates them into the gitignored
`tests/fixtures/generated/`.

## Failure handling

| Failure | Behaviour |
|---|---|
| Device disappears | Stop, emit `CAP-001`, keep the last frames readable, poll for return every 2 s |
| Device reappears | Reopen with the previous config and mode, renegotiate, emit a `device_restored` event |
| Device preempted mid-stream | Classify as busy, emit `CAP-002` naming likely owners, poll every 2 s and reopen if it returns |
| Open fails (busy) | `CAP-002`, name likely owners, offer retry |
| Format rejected | Step down the preference list, emit `CAP-003` with the rejected and negotiated modes |
| Mode delivers below its advertised rate | After 3 s below 80%, step down to the next preference (`CAP-003`); report when no lower mode exists |
| Frames stop without error (stalled device) | Watchdog at 2x the observed interval (clamped to 1-5 s) restarts once, then steps down and reports `CAP-004` |
| Timestamps go backwards or repeat | Re-anchor to arrival, keep the mapped timeline monotonic, emit `CAP-005` once per episode |
| OS suspend / resume | Suspend releases the device and resets the timestamp base; resume reopens the previous device and reports if it is gone |
| Session locked | Stop capture entirely; resume on unlock (privacy, `SEC-002`). The app's session handler calls `suspend()`/`resume()` |

The state machine does not clear the frame ring on `CAP-001` or `CAP-005`. `FrameRing::reset()` is
only safe when no consumer holds a lease, and a source cannot know that. Consumers must treat
`CameraSource::state()` as the authority and blank the output outside `streaming`; the last published
frames remain readable, and the four-slot ring ages them out. A lease-safe reset (an epoch in
`FrameRing`) remains a follow-up for T41; the T24 texture ring has its own generation counter and
does not reset the capture ring.

A camera that advertises a mode it cannot sustain is demoted, but demotion cannot fix an
exposure-limited low-light camera. That case is visible (`CAP-004`/`CAP-003`) rather than silent.
The webcam device survey that would measure how often it happens is not yet scheduled; T15 records
that as a gap rather than borrowing an unrelated task id.

## Performance targets

| Metric | Target |
|---|---|
| Capture callback → ring publish | < 1 ms |
| NV12 1080p → GPU texture | < 0.5 ms (upload) |
| Downscale to tracker input | < 0.3 ms (blit) |
| Dropped frames at 60 fps, 10 min | 0 |

## Open questions

| Question | Resolution path |
|---|---|
| Which cameras actually deliver 1080p60 over USB 3, and how often does MJPEG fallback trigger? | Device survey in M1 with 6 common webcams |
| Does the D3D12 Godot backend interoperate cleanly with Media Foundation's D3D manager? | M1 prototype, Windows only |
| Is the PipeWire camera portal stable enough to be the default on Wayland, or is V4L2 still required? | M1 on GNOME 46+ and KDE 6 |
| Can exposure/WB lock be exposed reliably across UVC devices? | Driver survey; feature-detect per device |
