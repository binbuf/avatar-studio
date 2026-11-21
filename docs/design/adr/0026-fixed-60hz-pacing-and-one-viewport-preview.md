# ADR-0026: Fixed 60 Hz pacing, one viewport, and a preview copy

- **Status:** Accepted
- **Date:** 2026-11-21
- **Supersedes:** —
- **Related:** [architecture.md](../architecture.md), [testing-and-quality.md](../testing-and-quality.md), [output-and-virtual-camera.md](../output-and-virtual-camera.md)
- **Tasks:** T16, T26, T46

## Context

The streamer runs Avatar Studio over OBS and a game. The avatar output must hold a fixed 60 Hz
cadence even when the desktop is 60, 120, 144 or 240 Hz, and the editor preview must not cost a
second scene render. Godot's defaults are not a product guarantee: `Engine.max_fps` alone drifts
with the display refresh, vsync at 144 Hz beats against a 60 Hz output, and low-processor mode or
macOS App Nap can suspend the loop when the window is hidden but a consumer is still connected.
The preview also has to be demonstrably a copy, not a second `SubViewport` that someone will later
mistake for free.

## Decision

`AvatarRuntime` owns one avatar `SubViewport` at 1920x1080, transparent, with
`render_target_update_mode = ALWAYS`, plus a fixed 180° presentation `Node3D`. The window preview is
a `TextureRect` whose texture is that viewport's `ViewportTexture`; it samples the same render
target and performs no scene render of its own.

`Engine.max_fps` is pinned to 60 in every mode. Vsync is enabled only when the display refresh is a
positive integer multiple of 60 within 1% (60, 120, 240, and 59.94); otherwise the window vsync is
disabled and `core::FramePacer` paces against an absolute monotonic deadline grid. The pacer counts
whole missed slots as dropped, advances the grid by whole intervals only, and never schedules a
catch-up frame. A late frame is dropped, never doubled.

Low-processor mode is disabled and `AvatarRuntime` uses `PROCESS_MODE_ALWAYS`, so a connected
consumer keeps the loop alive while the window is hidden or the tree is paused. macOS App Nap is
held off with an `NSProcessInfo` user-initiated activity while a consumer is connected. The debug
HUD is a C++-created `CanvasLayer`/`Label`, hidden by default and updated at ~4 Hz from the same
counters the test reads; no GDScript runs per frame.

## Alternatives considered

| Option | Why not |
|---|---|
| `Engine.max_fps = 60` alone, vsync always on | Vsync at 144 Hz beats against the 60 Hz output and the cadence follows the display, not the product. |
| Vsync always off, rely on max_fps | Frame phase drifts and late frames are replayed back-to-back by the loop's catch-up behaviour. |
| A second `SubViewport` for the preview | Doubles scene render cost and breaks the one-render invariant. |
| A GDScript `_process` HUD | Per-frame scripting on the render path; violates the GDExtension binding rules. |
| Keep low-processor mode while a consumer is connected | The output freezes when the window is hidden, which is exactly the OBS case. |

## Consequences

Positive: the output cadence is a property of the product, the preview is provably a copy, and the
HUD/test counters come from the same C++ state. Negative: the runtime now owns display-refresh
detection and a platform power hook; a 240 Hz display in vsync mode still relies on `max_fps` to
cap at 60, and macOS App Nap is only exercised on macOS hardware. The 10-second gate is a
regression signal; the 10-minute app soak remains T26 work.

## Follow-ups

- T26 applies the same frame-time and jitter rows to the 10-minute app soak with real avatar
  content and records Reference/Minimum tier evidence.
- T46 adds async readback and per-transport rings from the same viewport; it must not add a second
  render.
- T52/T55 verify the App Nap hook and display-refresh detection on macOS/Linux hardware.
