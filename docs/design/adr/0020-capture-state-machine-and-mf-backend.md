# ADR-0020: Platform-neutral capture state machine with a thin Media Foundation backend

- **Status:** Accepted
- **Date:** 2026-11-20
- **Supersedes:** —
- **Related:** [`../camera-capture.md`](../camera-capture.md), [ADR-0002](0002-native-camera-capture.md),
  [ADR-0013](0013-core-data-plane-primitives.md)

## Context

ADR-0002 chooses native capture on all three platforms. T10 must make the Windows path real while
keeping macOS (T52) and Linux (T55) from re-implementing negotiation, timestamp mapping, the stall
watchdog, hot-plug recovery and the CAP failure mapping. The behaviour in
[`camera-capture.md`](../camera-capture.md) is almost entirely platform-independent; only device
enumeration, media-type selection and sample delivery differ.

A Media Foundation `IMFSourceReader` in async mode delivers samples on a platform work-queue thread.
That thread must not block on consumers, and it must not call back into the backend to recover from an
error: `stop()` and `start()` re-enter the same locks. The capture state machine also has to be
testable on a machine with no camera, because CI runs on all three platforms.

## Decision

Split capture into two layers:

1. **`CameraSource`** (`native/capture/src/camera_source.cpp`) implements `ICameraSource` and owns all
   policy: device resolution, preference-list negotiation, the ring slots from T04, timestamp
   mapping, the watchdog, the 2-second reconnect poll, suspend/resume, and the CAP codes.
2. **`ICameraDeviceBackend`** (`native/capture/include/avatar/capture/camera_device.h`) is the thin
   platform seam: `enumerate`, `capabilities`, `open(device_id, mode)`, `start(callbacks)`, `stop`,
   `close`. The Windows implementation (`native/capture/windows/mf_capture.cpp`) wraps an async
   `IMFSourceReader`; the tests use a fake backend; macOS and Linux implement the same interface.

Rules that follow from the split:

1. **Explicit media type, no advanced processing.** The backend sets `MF_MT_FRAME_SIZE`,
   `MF_MT_FRAME_RATE` and the subtype explicitly and never enables
   `MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING` or the D3D manager. A rejected type is
   reported as `invalid_format`, not converted.
2. **Device identity is the symbolic link.** `MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK`
   is the only persisted id; array positions are never stored.
3. **The backend never recovers.** A read error is classified (busy, permission, lost, read) and
   recorded atomically by the source; `produce()` performs the state transition, so `stop()` is never
   called from inside a capture callback.
4. **Timestamps are anchored and clamped.** The first source timestamp anchors to `FrameClock`; later
   source deltas are added, the result is clamped to the arrival time (a driver clock that runs fast
   must not place a frame in the future), and a backwards or duplicate source timestamp re-anchors to
   arrival and emits `CAP-005` once per episode. The ring is not cleared because
   `FrameRing::reset()` is unsafe while a consumer holds a lease; stale frames age out.
5. **A stall is measured against the observed interval.** The watchdog deadline is twice the observed
   frame interval, clamped to 1-5 s. A low-light camera that legitimately drops to a few fps is not a
   stall. A stall restarts once; a second stall steps down the negotiated mode and then reports.
6. **Sustained shortfall steps down.** After 3 s, a mode delivering less than 80% of its negotiated
   frame rate is demoted to the next preference and `CAP-003` reports the change. This is the design's
   advertised-versus-delivered rule; exposure-limited cameras may be demoted too, which is visible and
   reversible.
7. **MJPEG is reported but not selected.** `FrameRing` owns fixed-size CPU formats and no decoder is
   pinned. Capabilities list MJPEG modes; the preference list omits them until T24 owns decode.

## Alternatives considered

| Option | Why not |
|---|---|
| Put the state machine in the Windows backend | macOS/Linux would duplicate negotiation, timestamps, watchdog and CAP mapping. |
| One `ICameraSource` per platform | The policy is identical; only the seam differs. It also makes the tests hardware-dependent. |
| Call `stop()` from the Media Foundation callback | Re-enters the callback mutex and the reader; a deadlock or use-after-free is likely. |
| Reset the ring on `CAP-005` | `FrameRing::reset()` clears pins; an outstanding lease would underflow the pin count. |
| Watchdog at a fixed 2x the nominal interval | 66 ms at 30 fps restarts healthy low-light streams; the observed interval is the honest baseline. |
| Auto-select MJPEG when NV12 is unavailable | Needs a decoder and a CPU conversion per frame; T24 owns that. |
| Use `MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING` | Silently converts formats and adds latency, which the design forbids. |

## Consequences

**Positive**

- The policy is tested on every platform with a fake backend; no camera is required for CI.
- macOS and Linux only implement device enumeration and sample delivery.
- The CAP mapping, reconnect poll, watchdog and suspend/resume are one implementation, not three.
- The Windows backend is small enough to audit against the "no hidden conversion" rule.

**Negative**

- A fake backend can drift from real driver behaviour; the `--capture` smoke and the manual checklist
  are the guard.
- The two-layer split adds one indirection per sample (a callback through the seam).
- The ring is not reset on a timestamp regression; consumers must treat the source state as the
  authority instead of assuming old frames disappear.
- Demotion on a sustained shortfall can be triggered by low light, not just bandwidth. It is visible
  (`CAP-003`) and the next start renegotiates from the top.

**Neutral**

- `CameraSource` exposes events (`take_events`) instead of logging; the app decides how to surface
  them. CAP codes are attached where a diagnostic code exists; successful recoveries
  (`device_restored`, `resumed`) carry no code.
- The Windows backend initialises COM/MTA and Media Foundation on first use. The app should call it
  from the thread that will own the source.

## Follow-ups

1. T52/T55: implement `ICameraDeviceBackend` for AVFoundation and V4L2/PipeWire; run the same fake-
   backend suite plus a real-camera checklist.
2. T24: add an MJPEG decoder path (GPU where possible) and extend the preference list.
3. T41: surface `CameraEvent` in the UI (busy owner hint, privacy deep link, negotiated mode).
4. T13/T26: feed `publish_latency()` into the M0/performance result; measure a real 1080p60 camera.
5. Revisit the 1-5 s watchdog clamp and the 80% shortfall threshold after a device survey.
