# Output and Virtual Camera

**Status:** proposal · output gate, Null/File/Window transports, alpha/colour boundary and no-passthrough invariant implemented (T23); async readback, per-transport queues and third-party transports planned · **Last updated:** 2026-11-22
**Decision record:** [ADR-0005](adr/0005-output-transports.md), [ADR-0033](adr/0033-output-gate-and-window-transport.md)

Output looks like a fan-out problem. It is actually two different problems that share a frame
source: **transport to a cooperating app** (OBS, Resolume, TouchDesigner) and **presentation as a
system camera** (Discord, Zoom, Teams, browsers). The second is inherently cross-process, and that
shapes the whole design.

---

## Topology

```
                 one render, one readback
                           │
                    ┌──────▼──────┐
                    │ OutputFrame │  (RGBA8, timestamped, immutable)
                    └──────┬──────┘
                           │
                   drop-oldest ring (per transport)
        ┌──────────────┬───┴──────────┬──────────────────┐
        ▼              ▼              ▼                  ▼
     Spout2         Syphon         PipeWire         OS virtual camera
    (Win, GPU       (mac, GPU      (Linux, node     (Win MF / mac CMIO /
     or CPU)         or CPU)        or dmabuf)       Linux PipeWire node)
        │              │              │                  │
      OBS            OBS / VJ      OBS / GStreamer    any camera consumer
```

Non-negotiable rules:

1. **One render.** The avatar scene renders once into a `SubViewport`. No transport gets its own
   scene render. Background compositing for opaque consumers happens in a full-screen pass on the
   copied frame, not by re-rendering the scene.
2. **No transport may back-pressure rendering.** Each transport has its own bounded queue; a stalled
   consumer loses frames.
3. **Readback only when someone is listening.** If no transport has a consumer, no GPU→CPU copy
   happens. This is a measurable idle-power win and is how the reference implementation behaves.
4. **Alpha is a per-transport setting**, because the transports disagree and a wrong setting is
   visible as a black or bright fringe.

## OutputFrame

```cpp
struct OutputFrame {
    uint64_t  source_timestamp_ns;  // when the frame was captured (for latency measurement)
    uint64_t  render_ns;            // when the render completed
    uint32_t  sequence;
    uint32_t  width, height;
    PixelFormat format;             // RGBA8 in v1
    AlphaMode alpha;                // Premultiplied | Straight | Opaque
    ColorSpace color_space;         // sRGB encoded
    const uint8_t* pixels;          // tightly packed
    void*     native_handle;        // reserved for zero-copy paths
};
```

### Readback

- Submitted from the render thread via `RenderingDevice.texture_get_data_async`, completion handled
  on a worker. Never `Texture2D.get_image()` per frame: it stalls the GPU.
- Completion callbacks resolve the node by instance ID, so a destroyed viewport cannot crash the
  process.
- Cost at 1080p RGBA8: 8.3 MB per frame, ~500 MB/s at 60 fps. This is the number that justifies the
  drop-oldest policy and the idle suppression.

## Implemented output gate (T23)

`native/output/` is the framework-free output boundary. `OutputFrame` carries
`source_timestamp_ns`, `render_ns`, `sequence`, `width`/`height`, RGBA8 pixels, `AlphaMode`,
`ColorSpace` and a `FrameContent` marker (`avatar` or `blank`). `IVideoOutput` is the transport
seam; `NullOutput`, `FileOutput` (a PNG sequence) and the GDExtension `WindowOutput` implement it.
T46 adds async readback, fan-out and per-transport drop-oldest queues behind the same interface;
T23 deliberately owns one transport per manager so the invariant is easy to test.

`OutputGate` is the state machine. It submits the avatar only when `result_valid`, `features_valid`
and a frame are all present. Otherwise it holds the current rendered frame for **less than 150 ms**
from the last complete accepted sample, then submits a blank frame. At the deadline the output is
blank. An invalid result never refreshes the hold. The gate changes no pose and no pixels, so there
is no decay to neutral and no stored last-frame freeze. The transport is never shut down while
blank; it stays initialized and enumerated.

Transitions emit `TRK-002` (face lost), `TRK-008` (eyes/mouth missing) and `OUT-007` (gate blank)
**once**, with a `cause` field (`face_lost`, `missing_features`, `no_frame`). A cause change is a
new transition; repeated frames with the same cause do not repeat the code. Resuming the avatar
does not emit a blank diagnostic.

Blank means transparent for `straight`/`premultiplied` outputs and the configured background for
`opaque` outputs. The boundary normalises straight alpha, converts linear↔sRGB, composites opaque
outputs over the background in linear light, and premultiplies last. This is where the Compatibility
renderer's linear capture and Forward+'s sRGB capture become one transport-facing format. A linear
0.5 at alpha 0.5 over black therefore encodes to sRGB ≈0.537.

Evidence: `tests/unit/test_output.cpp` (gate, hold deadline, causes, PNG round-trip, alpha/colour,
transport-agnostic decisions), `app/tests/output_gate.gd` (headless Null/File and real-window
window/no-passthrough), and `tests/gates/T23.ps1`. The no-passthrough diff changes 2,565 pixels and
0 lie outside the union of region masks with a one-pixel footprint; dropped-mouth, tracker-timeout
and forced-invalid cases blank at 150 ms on window, Null and File. See
[ADR-0033](adr/0033-output-gate-and-window-transport.md).

## Transports for cooperating apps

| Platform | Transport | Mechanism | Alpha | Notes |
|---|---|---|---|---|
| Windows | **Spout2** | D3D11 shared texture | Yes (premultiplied) | OBS needs the Spout2 plugin; Resolume/TouchDesigner native |
| macOS | **Syphon** | `IOSurface` via Metal | Yes | OBS has a built-in Syphon client |
| Linux | **PipeWire** | `Video/Source` node | Limited | Consumed by OBS's PipeWire camera source (beta), GStreamer, `qpwgraph` |
| All | **Window capture** | The app's own window | No | Always available; the documented fallback |
| All | **OBS Virtual Camera** | OBS composits our window/source, then exposes a camera | No | Zero integration work; requires OBS |

**Spout2 caveat.** Spout's GPU path is D3D11. Godot's Forward+ renderer on Windows runs on Vulkan or
D3D12. There is no direct D3D11 texture to hand over, so v1 uses the CPU-readback path (convert RGBA
→ BGRA on the worker) and the D3D11 shared-texture path is a later optimisation. Publishing a Spout
sender that is fed from CPU memory is normal and is what the reference implementation does.

**PipeWire caveat.** The node must advertise a fixed format that consumers can negotiate without
guessing. BGRA at the viewport size, one framerate, no ranges — this is the shape that negotiates
with OBS's camera source in practice. Advertising a format menu is how you get black frames and
unhelpful bug reports.

**OBS onboarding.** Spout2 on Windows is a third-party OBS plugin; Syphon is built into OBS; the
PipeWire camera source is beta. The app cannot assume the plugin is installed. When a transport is
enabled and no receiver connects within a few seconds, Live mode shows a setup card naming the
plugin, the minimum version, a link, and a *Test connection* action. Window capture is always
offered as the fallback.

## Virtual cameras

### Windows — Media Foundation virtual camera

Requirements and constraints (verified):

- Windows 11 **build 22000+**. Windows 10 has no equivalent; the fallback is OBS Virtual Camera or a
  DirectShow-based shim, which is `Deferred`.
- The camera is backed by a **media source COM object** that must be registered in `HKLM`
  (`regsvr32`-style, administrator). It cannot be registered per-user, because it is loaded by
  *other* processes.
- Packaged apps must declare the webcam capability; the API must not be called from the UI thread
  (the capability check can deadlock behind a consent dialog).

Consequences for the product:

| Consequence | Handling |
|---|---|
| Installer needs elevation | Per-machine installer; document exactly what is written; offer a portable build without virtual camera |
| The media source lives in the consumer's process | Frames must cross a process boundary (below) |
| Registration outlives a crash | Startup health check, "repair camera" action, unregister on clean exit |
| The device is visible before the app runs | The media source must produce a defined "no signal" frame, not hang or return nothing |

### macOS — CoreMediaIO camera extension

- A **system extension** embedded in the app bundle, installed via `SystemExtensions` framework with
  user approval in System Settings. Delivered by Developer ID + notarisation, or via the App Store
  (Apple explicitly supports this delivery route).
- Runs in its own sandboxed process; the app and extension communicate over an app-group Mach
  service (`CMIOExtensionMachServiceName`).
- Entitlements: `com.apple.developer.system-extension.install` and Camera on the app, matching
  `App Groups` on both, `CMIOExtension` dictionary in the extension's `Info.plist`.
- Uninstalling the app removes the extension; there must also be an explicit "remove camera" action
  for users who keep the app but do not want the device.

### Linux — PipeWire node, `v4l2loopback` fallback

- **Primary:** publish a `Video/Source` node with `media.role=Camera`. User-space, no root, no kernel
  module, visible to OBS (PipeWire camera source), GStreamer, and browsers with PipeWire camera
  support. This is the modern answer and it is materially better than the kernel-module approach the
  source brief assumed.
- **Fallback:** `v4l2loopback` for consumers that only read V4L2. Requires the module, usually a
  `pkexec` prompt, and is complicated by Secure Boot (module signing). Offered, never required.
- Flatpak distribution needs the PipeWire socket permission; documented per package format.

## Cross-process frame transport

This is the subsystem the source brief missed. On Windows and macOS the consumer of the frames is
**not our process**:

- Windows: the media source DLL is loaded into the app that opened the camera (Teams, Chrome, OBS).
- macOS: the extension is a separate sandboxed process.

Design:

```
producer (Avatar Studio)                      consumer (media source / extension)
────────────────────────                      ──────────────────────────────────
write frame into shared ring slot
publish sequence (release)
signal sync object  ───────────────────────▶  wait on sync object (timeout 100 ms)
                                              read newest complete slot
                                              copy/wrap into platform buffer
                                              release slot
```

| Property | Decision |
|---|---|
| Memory | Named shared memory, one ring, 3 slots, fixed size negotiated at start |
| Format | RGBA8 or NV12, fixed per session; no mid-stream format changes |
| Synchronisation | Windows: `IMFCameraSyncObject` (the API's own primitive). macOS: XPC message + `IOSurface` in an app-group container |
| Policy | Drop-oldest. The consumer always gets the newest frame; it never blocks the producer |
| Teardown | Producer signals end-of-stream; consumer switches to the "no signal" frame |
| Versioning | Shared-memory header carries a layout version; mismatched versions refuse to connect with a clear error |

**No-signal behaviour is a feature.** When the app is closed, crashed, or paused, the device stays
enumerated. It must emit a neutral placeholder frame (configurable: black, a brand card, or a "no
signal" card) so that consumer apps do not freeze or spin. The placeholder is never a frozen frame of
the avatar or the camera. This single detail prevents most "your virtual camera is broken" reports. It applies to Windows and macOS, where the device outlives the process.
On Linux the PipeWire node is owned by the app and disappears when it exits, so there is nothing to
keep alive; consumers see the device go away rather than a placeholder.

While the app is running, a blank output-gate frame is submitted as a normal frame: transparent for
OBS/Spout and the configured background colour for the virtual camera. It is never a held frame and
never the camera; the device stays enumerated. T23 implements this for the window/Null/File
transports ([above](#implemented-output-gate-t23)); T53/T57 carry it across the process boundary.

### Transport security

The frame path is a local attack surface, and the OS camera permission prompt does not cover it.

| Platform | Control |
|---|---|
| Windows | The named shared-memory section is created with a DACL that grants the current user SID. Packaged consumers are the open question: their AppContainer SID is not known in advance, so M4 either grants `ALL APPLICATION PACKAGES` (same-user trust boundary, documented) or introduces a broker. A per-session nonce in the header rejects a stale producer; it is not a defense against a same-user process that can already open the section (T11). |
| macOS | The Mach service and `IOSurface` live in the app-group container. Only processes signed by the same team and carrying the app-group entitlement can connect. The extension validates the app's code signature (`SecCodeCheckValidity` with the expected requirement) before accepting frames. |
| Linux | The PipeWire node is owned by the user's session; PipeWire policy controls which clients may connect. No shared memory is used. |

Threats and residual risk:

- **Another local process reads the frames.** Same-user processes can already open a physical camera.
  The DACL/entitlement keeps other users and sandboxed apps out; it does not defend against a
  same-user process. This is documented in the privacy page rather than implied away.
- **A foreign producer injects frames.** The session nonce rejects a stale producer, and the macOS
  code-signature check rejects an unsigned one. On Windows, registering a competing media source
  requires administrator; a same-user process with write access is the T11 residual risk.
- **A stale consumer reads a recycled buffer.** The sequence number is published with release
  semantics; the consumer copies a complete slot and never reads a slot being written.

The shared-memory header is versioned and fuzzed
([`testing-and-quality.md`](testing-and-quality.md#sanitizers-static-analysis-and-fuzzing)).

## Alpha policy

| Consumer | Wants | Default we send |
|---|---|---|
| OBS (Spout2/Syphon/PipeWire) | Premultiplied, linear-light composited | Premultiplied sRGB |
| OBS window capture | Opaque | Opaque |
| Virtual camera (all) | Opaque | Opaque over configured background |
| VJ tools | Either | Premultiplied |

The app ships a **checkerboard test pattern** and a **fringe test card** (white-on-black and
black-on-white edges) so users can diagnose alpha problems in seconds instead of guessing.

## Lifecycle

| Event | Behaviour |
|---|---|
| App start | Enumerate transports; do not register virtual cameras until the user enables output |
| Second instance | Cannot register the same device; its virtual camera is disabled and the running instance is activated |
| Enable virtual camera | Register device (Windows), install/activate extension (macOS), publish node (Linux) |
| App exit | Unregister/withdraw device; flush queues |
| OS suspend | Transports pause; device emits no-signal; queues and timestamps reset on wake |
| Session lock | Capture and output stop; device emits no-signal; resume on unlock (`SEC-002`) |
| Output gate blank | No face or missing eyes/mouth beyond the 150 ms hold | Submit a blank frame; keep the device live; log once (`TRK-002`/`TRK-008`, `OUT-007`) |
| App upgrade | macOS may require extension re-approval; detect and prompt, do not silently fail |
| Crash | Windows/macOS: device may persist; next start detects and offers repair. Linux: node disappears with the process |
| Consumer connects | Transport reports `wants_frames = true`; readback begins |
| Consumer disconnects | Readback stops after a grace period (2 s) |
| Output resolution change | All transports restart with the new size; virtual camera re-registers |
| GPU device reset | Transports are paused, viewport rebuilt, transports resume; consumers see the no-signal frame meanwhile |

## Performance

| Item | Budget |
|---|---|
| Readback submit | 0.3 ms |
| RGBA→BGRA conversion (1080p, per transport) | 1.5 ms (vectorised, worker thread) |
| Shared-memory copy to virtual camera | 1.0 ms |
| Spout2/Syphon publish | 0.5 ms (CPU path) |
| Total added latency per frame | 2–4 ms |

Two consumers cost two conversions. If profiling shows this matters, the conversion moves into the
readback pass (produce BGRA directly) - one conversion for all consumers. That optimisation is
planned, not speculative.

T05's `avatar_bench --scenario render` measures a CPU BGRA→RGBA composite/readback proxy so the
harness has the right metric shape before the output manager exists. It is not a product number:
T46 replaces it with the async readback and per-transport queues described above, and T26 owns the
GPU-side frame-time gate.

## Open questions

| Question | Resolution path |
|---|---|
| Does Godot 4.7 expose enough of the RD texture to wrap an external D3D11/Metal/dmabuf texture? | M3 prototype per platform; `texture_create_from_extension` availability to be confirmed |
| Is the OBS PipeWire camera source reliable enough to document as the Linux path? | Test across OBS 31/32 and Flatpak |
| Does Windows allow re-registering a virtual camera with the same CLSID after a crash without a reboot? | M4 test; fallback is a new CLSID per session |
| Can the macOS extension survive an app upgrade without a re-approval prompt? | M4 test with a signed upgrade path |
| Does any mainstream consumer accept NV12 from a virtual camera, halving bandwidth? | M4 device matrix |
