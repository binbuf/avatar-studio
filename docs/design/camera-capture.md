# Camera Capture

**Status:** proposal · **Last updated:** 2026-11-20
**Decision record:** [ADR-0002](adr/0002-native-camera-capture.md)

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
- YUV→RGB is a shader (`app/shaders/yuv_convert.gdshader`) with BT.601/709 and full/limited range as
  uniforms, driven by `ColorSpace` from the frame. Getting this wrong produces the classic
  green/magenta cast.

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

Capture must be testable without hardware, so two synthetic sources are first-class:

| Source | Purpose | Determinism |
|---|---|---|
| `TestPatternSource` | Colour bars, moving square, timestamp burn-in, alpha checkerboard | Fully deterministic |
| `SyntheticFaceSource` | A rendered face that translates/rotates on a scripted path, with a known ground-truth pose | Fully deterministic |
| `SyntheticFaceSource` (distorted) | The same face with a synthetic barrel distortion, to quantify the accepted lens error | Fully deterministic |
| `ReplaySource` | Recorded `.y4m`/`.mkv` clip with a sidecar pose track | Frame-exact |

`SyntheticFaceSource` is what makes CI possible: it feeds the real tracker (or the scripted tracker)
and lets golden-frame tests assert both pose accuracy and decal placement without a person in front
of a camera.

## Failure handling

| Failure | Behaviour |
|---|---|
| Device disappears | Stop, emit `device_lost`, drop the frame ring, poll for return every 2 s |
| Device reappears | Reopen with previous config, renegotiate, emit `device_restored` |
| Open fails (busy) | `device_busy`, list likely owners, offer retry |
| Format rejected | Step down the preference list, log the actual negotiated mode, inform the UI |
| Frames stop without error (stalled device) | Watchdog at 2× expected interval → restart the stream once, then report |
| Timestamps go backwards | Reset the ring, log once, re-map to arrival time |
| OS suspend / resume | Stop the stream; reset ring and timestamp base; reopen the previous device; report if it is gone |
| Session locked | Stop capture entirely; resume on unlock (privacy, `SEC-002`) |

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
