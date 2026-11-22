# ADR-0033: Output gate and window transport

- **Status:** Accepted
- **Date:** 2026-11-22
- **Related:** [`../output-and-virtual-camera.md`](../output-and-virtual-camera.md), [`../face-compositing.md`](../face-compositing.md), [`../testing-and-quality.md`](../testing-and-quality.md), [ADR-0010](0010-output-is-avatar-or-nothing.md), [ADR-0005](0005-output-transports.md)

## Context

ADR-0010 fixes the invariant: the output is the rendered avatar or a blank
frame, never the camera and never a frozen face. The gate has to hold for every
transport, including transports added later. T23 needs the first real
transports (window, Null, File) and a way to test the invariant without
pretending a shader is the gate.

The captured avatar viewport is linear under the Compatibility renderer and sRGB
under Forward+. Transports disagree about alpha (OBS-style outputs want
premultiplied; an opaque window capture wants no alpha). Both differences must
be resolved once, at the output boundary, or every transport becomes a place
where the invariant can be lost.

## Decision

**The gate is a native, transport-agnostic state machine in the output manager,
and every frame crosses one alpha/colour boundary.**

- `native/output/` owns `OutputFrame`, `IVideoOutput`, `OutputGate` and
  `OutputManager`. `OutputFrame` carries source timestamp, render time,
  sequence, size, RGBA8 pixels, alpha mode, colour space and whether the frame
  is the avatar or blank.
- The gate submits the avatar only when `result_valid` and `features_valid` are
  true and a frame exists. Otherwise it holds the current rendered frame for
  less than 150 ms from the last complete accepted sample, then submits a blank
  frame. At the deadline the output is blank. Invalid results never extend the
  hold. The gate never changes pose or pixels, so it cannot decay to neutral or
  freeze a stored frame.
- Blank means transparent for non-opaque outputs and the configured background
  for opaque outputs. The output stays initialized and enumerated while blank.
- Transitions emit `TRK-002` (face lost), `TRK-008` (eyes/mouth missing) and
  `OUT-007` (gate blank) once, with a `cause` field. A cause change is a new
  transition; repeated frames with the same cause do not repeat the code.
- The output boundary normalises straight alpha, converts linear/sRGB, then
  composites an opaque output over its background in linear light. A
  premultiplied output is premultiplied last.
- T23 implements `NullOutput`, `FileOutput` (a PNG sequence with a
  dependency-free stored-deflate writer) and `WindowOutput` (a GDExtension
  `TextureRect` presentation). `OutputManager` owns one transport; T46 adds
  async readback, fan-out and per-transport drop-oldest queues behind the same
  seam.
- The invariant is tested on all three transports: the native unit suite, the
  headless adapter and a real Compatibility window. The no-passthrough diff
  feeds a high-contrast camera pattern and a black camera texture and requires
  every changed pixel to be inside the union of region masks, with a one-pixel
  footprint tolerance.

## Alternatives considered

| Option | Why not |
|---|---|
| Gate in the decal shader | A transport or a later compositing pass can bypass a shader; the invariant must be above every transport |
| Hold the last frame indefinitely | Shows a frozen face; the exact failure ADR-0010 rejects |
| Decay to a neutral pose | Still a face, still not live; hides the failure from the performer |
| Implement the gate in GDScript | Per-frame script work, and the native transports would not share one tested state machine |
| Add fan-out and queues now | T46 owns back-pressure, idle suppression and readback; T23 needs one correct transport boundary first |
| Compress the PNG sequence | Adds a dependency or a large encoder for a diagnostic transport; stored deflate is a valid PNG and is easy to inspect |

## Consequences

**Positive**

- One gate and one alpha/colour boundary for every current and future transport.
- The blank deadline, causes and diagnostics are deterministic and unit-tested.
- The no-passthrough invariant is proven on real rendered pixels, not inferred
  from shader source.

**Negative**

- `OutputManager` is single-transport until T46; a caller that needs fan-out
  creates one manager per transport for now.
- The PNG writer trades file size for zero dependencies.
- The one-pixel footprint tolerance is a test concession to rasterisation and
  bilinear sampling, not a licence to widen the masks in production.

**Neutral**

- The window transport presents through a `TextureRect`; window capture by OBS
  sees exactly that frame, including the transparent/opaque blank.
