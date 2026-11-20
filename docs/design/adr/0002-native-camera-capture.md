# ADR-0002: Native camera capture on all three platforms

- **Status:** Proposed
- **Date:** 2026-11-20
- **Related:** [`../camera-capture.md`](../camera-capture.md)

## Context

Godot's `CameraServer` is implemented on Linux, Android, macOS and iOS — **not on Windows**. The
source brief proposed native capture on Windows and reuse of `CameraServer` on macOS and Linux.

The pipeline needs more than "a texture with frames in it":

- source timestamps, to pair pose with pixels ([`../architecture.md`](../architecture.md#frame-lifecycle)),
- control over pixel format, to avoid hidden conversions and to keep NV12 on the GPU,
- buffer-level access for a future zero-copy path,
- identical behaviour and identical bugs on every platform, so that quality work transfers,
- a capture path that works headlessly in tests, with no window and no GPU.

## Decision

Implement a single `ICameraSource` interface with three native backends: **Media Foundation**
(Windows), **AVFoundation** (macOS), and **V4L2 with a PipeWire fallback** (Linux). Do not use
`CameraServer` anywhere, including on the platforms where it exists.

## Alternatives considered

| Option | Why not |
|---|---|
| **Native on Windows, `CameraServer` on macOS/Linux** (the brief's proposal) | Two capture semantics, two timestamp stories, two format stories. Every capture bug becomes "does it reproduce on the other backend?" and the rig's timestamp pairing depends on semantics `CameraServer` does not guarantee. |
| **`CameraServer` everywhere, contributing a Windows backend upstream** | Would require landing a Media Foundation backend in the engine, then waiting for a release, then being unable to change it. Also still lacks source timestamps and buffer access. |
| **Third-party capture library** (OpenCV VideoCapture, libuvc, GStreamer) | OpenCV adds a large dependency and does its own conversions; libuvc bypasses the OS camera stack and breaks virtual cameras and permissions; GStreamer is a heavy runtime to ship on three platforms. |
| **A helper process that captures and forwards frames** | Adds IPC latency and a failure mode to the one path that must never stall. Rejected. |

## Consequences

**Positive**

- One frame contract, one timestamp contract, one set of failure modes.
- Format negotiation is ours, so NV12/YUY2/MJPEG behaviour is predictable.
- The camera layer is unit-testable with synthetic sources and replays.
- Virtual cameras (which appear as ordinary capture devices) work through the same path, so a user
  can chain two instances.

**Negative**

- Three platform implementations to write and maintain, including permissions, hot-plug and
  device-busy handling — roughly 3× the capture code the brief implied.
- We own bugs that Godot would otherwise own.
- Linux needs two paths (V4L2 and PipeWire) because sandboxed and Wayland environments differ.

**Neutral**

- macOS loses nothing: AVFoundation is what `CameraServer` uses underneath anyway.

## Follow-ups

1. M0: all three backends deliver timestamped NV12 at 1080p30 and 720p60 on the reference hardware.
2. M1: measure the CPU upload path and decide whether the D3D11/IOSurface/dmabuf zero-copy spike is
   worth scheduling before M3.
3. Document the permission model per platform in the user guide, since it is the first thing a new
   user hits.
