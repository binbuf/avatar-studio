# ADR-0005: Render once, distribute many; OS virtual cameras as separate processes

- **Status:** Proposed
- **Date:** 2026-11-20
- **Related:** [`../output-and-virtual-camera.md`](../output-and-virtual-camera.md),
  [`../architecture.md`](../architecture.md)

## Context

Output has two audiences that look similar and are not:

- **Cooperating apps** (OBS, Resolume, TouchDesigner) that can consume a GPU or PipeWire stream.
- **Ordinary apps** (Discord, Zoom, Teams, browsers) that only open a system camera.

The source brief concluded that no companion app is needed. That is true for capture and false for
output, for reasons that are properties of the operating systems, not of our design:

- On Windows 11, a virtual camera is a **Media Foundation media source COM object** that is
  registered in `HKLM` and loaded into *whichever application opens the camera*. Our frames must
  reach a different process, in a different address space, with a defined lifetime.
- On macOS, a camera is a **CoreMediaIO system extension** running in its own sandboxed process,
  talking to the app over an app-group Mach service.

Additionally, the reference implementation for Godot→OBS transport (`SpectatorStreamExporter`, MIT)
copies every frame through the CPU: ~500 MB/s at 1080p60. That is workable but it shapes the
architecture.

## Decision

1. **One render.** The avatar renders once into a transparent `SubViewport` at output resolution.
   Transports receive copies of that frame; none of them triggers a second scene render. Opaque
   consumers (virtual cameras, window capture) get the frame composited over a background in a
   full-screen pass, not by re-rendering.
2. **Asynchronous CPU readback in v1.** `RenderingDevice.texture_get_data_async`, completion on a
   worker, drop-oldest ring per transport, no readback at all when no consumer is connected.
3. **Virtual cameras are separate processes with an explicit frame transport**: a versioned
   shared-memory ring plus the platform's own synchronisation primitive (`IMFCameraSyncObject` on
   Windows, XPC + `IOSurface` on macOS). Linux is the exception: a PipeWire `Video/Source` node is
   in-process and needs no IPC.
4. **The device outlives the app and must say so.** When the app is not running, the virtual camera
   emits a defined "no signal" frame rather than hanging or black-screening.
5. **Zero-copy is research, not a requirement.** Per-platform feasibility is documented and spiked
   independently; shipping is not blocked on it.

## Alternatives considered

| Option | Why not |
|---|---|
| **Render per destination** | Doubles GPU cost, creates divergent frame timing, and makes "one stream" impossible to reason about. |
| **A companion process that owns capture and rendering, with a thin viewer** | Moves the problem, adds IPC to the latency-critical path, and doubles the process lifecycle work. Rejected; the virtual camera's cross-process hop is unavoidable but should be the *only* one. |
| **OBS-only (no virtual camera)** | Contradicts the product goal of working without OBS. The brief was explicit that both matter. |
| **DirectShow virtual camera on Windows (works on Windows 10 too)** | Legacy stack, invisible to Media Foundation-only consumers (including the Windows Camera app and UWP), and requires a kernel-mode or filter-graph shim. Kept only as a possible Windows 10 fallback, `Deferred`. |
| **`v4l2loopback` as the Linux default** | Requires a kernel module, root, and Secure Boot accommodation. PipeWire nodes are user-space and already consumed by OBS and browsers. |
| **Synchronous readback** | Stalls the GPU every frame; the exact failure the async path exists to avoid. |
| **Zero-copy first** | Cross-API texture sharing (Vulkan↔D3D11, Vulkan↔Metal, Vulkan↔dmabuf) is the riskiest and least portable part of the system. Building it first would delay validation of the actual product. |

## Consequences

**Positive**

- Frame timing is single-sourced; every consumer sees the same frame with the same timestamp.
- Idle cost is near zero when nobody is watching.
- Transports are independent: a slow VJ tool cannot degrade an OBS stream.
- The CPU path is portable and debuggable, which matters far more in year one than the last 3 ms.

**Negative**

- ~500 MB/s of GPU→CPU bandwidth at 1080p60, plus a per-transport conversion unless it is moved into
  the readback pass.
- Two extra build targets (Windows media source DLL, macOS extension) with their own signing,
  installation and crash-recovery stories.
- Windows requires an elevated installer; some users will refuse, and the fallback is OBS.
- A shared-memory contract is now a versioned interface that must be maintained.

**Neutral**

- Linux gets a materially better experience than the brief assumed, with less privilege.

## Follow-ups

1. M3: measure the real readback and conversion cost on Minimum hardware; move the RGBA→BGRA
   conversion into the readback pass if it exceeds 1.5 ms per transport.
2. M3: spike zero-copy for macOS (IOSurface→Metal) first, since it has the best odds, and only then
   Windows (D3D12 backend + shared handles) and Linux (dmabuf import).
3. M4: validate that a crash leaves a recoverable device on Windows and macOS, and that the repair
   flow works without a reboot.
4. Document the shared-memory header as a stable interface with its own version and tests.
