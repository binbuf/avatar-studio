# ADR-0034: Timestamp-paired camera texture ring

- **Status:** Accepted
- **Date:** 2026-11-22
- **Related:** [`../camera-capture.md`](../camera-capture.md), [`../face-compositing.md`](../face-compositing.md), [`../architecture.md`](../architecture.md), [`../testing-and-quality.md`](../testing-and-quality.md), [ADR-0020](0020-capture-state-machine-and-mf-backend.md), [ADR-0028](0028-facerig-estimation-and-temporal-contract.md)
- **Supersedes:** none

## Context

The decal's sample position comes from a tracking result, while its pixels come
from the camera. If the renderer applies the newest result to the newest frame,
the two are one to three frames apart during motion and the face visibly slides
against the model. The camera frame that produced a pose must remain available
until the decal has sampled it, including across ring wrap and a camera format
change.

The capture layer already publishes immutable CPU frames with a mapped source
timestamp into a four-slot `FrameRing` ([ADR-0020](0020-capture-state-machine-and-mf-backend.md)).
T24 adds the GPU-facing half of that contract and the selection rule that ties a
`FaceRigOutput` back to its source frame.

## Decision

1. **Four-slot GPU ring.** `avatar::capture::CameraTextureRing` owns four slots
   of timestamp, arrival time, sequence, format, colour space and generation.
   `publish` uploads through an `ICameraTextureUploader` seam. The producer
   overwrites the oldest unpinned slot; when every slot is pinned by a slow
   consumer it drops and counts the frame rather than blocking or growing a
   queue. A wrap is counted so the debug HUD can prove it happened.
2. **Exact timestamp pairing.** The decal calls `select(result.source_timestamp_ns,
   render_time_ns)`. The ring returns only a slot whose timestamp is exactly
   that value, is not in the future and is no older than 150 ms. A miss makes the
   decal blank; it never substitutes different pixels. `select_for_tracking`
   separately returns the newest slot at or before the render clock for
   inference, and pins it for the copy.
3. **Drop before inference.** `TrackingFrameGate` admits at most one inference
   at a time. While it is in flight every offered frame is dropped and counted;
   stale or duplicate frames are rejected. There is no inference queue.
4. **Format changes are deferred, never torn.** A new format or shape bumps the
   generation and invalidates the old slots only when no slot is pinned. While a
   pinned slot exists the new-format frame is dropped; the next publish after
   release applies the change. Consumers compare `generation` before sampling.
5. **Plane upload, shader YUV.** NV12 uploads an R8 luma texture and an RG8
   chroma texture; BGRA8/RGBA8 uploads unchanged bytes as RGBA8 and the shader
   swaps R/B from the slot's format. YUV -> RGB happens in
   `app/shaders/yuv_convert.gdshaderinc` with BT.601/709 and limited/full range
   uniforms. The CPU never performs a full-frame colour conversion.
6. **FaceRig newest-not-after.** `FaceRig::submit` buffers at most four results;
   `sample(render_time)` applies the newest result whose source timestamp is at
   or before the render time and discards the older ones. A future result waits
   instead of being applied early. The applied result's source timestamp is what
   the decal uses to select the ring slot.

## Consequences

- Pose and pixels come from the same instant; the synthetic fast-motion test
  measures paired sliding at 0.66 px while the naive newest-pose/newest-pixels
  path measures 6.45 px and fails the same assertion.
- The ring costs four textures (or eight plane textures for NV12) per camera,
  which is the intended memory cost. It is a correctness mechanism, not an
  optimisation; removing it reintroduces the slide.
- A consumer that fails to release a slot loses frames rather than delaying the
  producer. The counters distinguish wrap, drop, deferred format change and
  selection miss.
- The shader path supports NV12 and BGRA8/RGBA8. YUY2 and MJPEG remain rejected
  by the texture uploader until a decoder or a packed-YUV shader lands.
- The GPU upload is a CPU-to-GPU copy in v1. Zero-copy capture remains research
  ([ADR-0005](0005-output-transports.md)); the measured cost is recorded in the
  local progress workspace.
