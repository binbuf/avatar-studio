# ADR-0035 — Live HUD, deliberate panic blank, and the live edit guard

- **Status:** Accepted
- **Date:** 2026-11-22
- **Supersedes:** —
- **Related:** [editor-ux.md](../editor-ux.md), [face-compositing.md](../face-compositing.md), [diagnostics.md](../diagnostics.md)
- **Tasks:** T25, T41, T49

## Context

The vertical slice renders one avatar but has no honest Live surface. A streamer needs to see the
actual fps, frame time, tracking confidence, face and output state, and to blank the output with one
action when something goes wrong. The design already says the output is the avatar or nothing, so a
panic action must not freeze the last frame or reveal the camera. It also says editing that can break
the stream requires Stop, while outfits and appearance are safe.

T25 must deliver a minimal surface without pre-empting the full editor shell (T33), the complete
error table (T41) or transport status (T49). The app has no live tracker/capture wiring yet (T26/T32),
so the surface must be honest about that rather than simulate a face.

## Decision

1. **Panic is an output-gate state, not a UI overlay.** `OutputGate::set_panic(true, render_time_ns)`
   clears the hold and returns `BlankCause::panic`; `OutputManager::set_panic` submits a blank frame
   **at the call**, not on the next render. `OUT-007` is emitted once with `cause = "panic"`. A valid
   frame cannot resume the avatar until panic is cleared; clearing it leaves the gate blank until the
   next complete valid sample. No static, frozen or camera frame is ever substituted.
2. **The Live state machine and copy are framework-free.** `native/live/` owns elapsed time, actual
   fps, frame time, tracking confidence, face/output state, status precedence, the four degraded
   state sentences/actions and the edit guard. `AvatarLiveSession` is a thin GDExtension adapter.
   `app/scenes/live_hud.gd` is the surface over the existing mirrored preview; it updates at 4 Hz.
3. **Four states are surfaced now:** camera lost (`CAP-001`), face not visible (`TRK-001`), eyes or
   mouth hidden (`TRK-008`) and tracking degraded (`TRK-004`). Each is one sentence, one action and
   the support code. The banner never hides the face.
4. **Hotkeys:** the in-app Ctrl/Cmd+Shift+H binding always works. A Windows `RegisterHotKey` listener
   is attempted on a dedicated message thread; when the OS refuses, or on a platform without a
   supported listener, the HUD states that the in-app binding still works.
5. **Edit guard:** while live, avatar, face calibration, camera, output resolution and transport
   changes are refused with *Stop and switch*. Outfit and appearance remain allowed.
6. **Output is a labelled placeholder.** The window transport is real, but the HUD calls it a
   placeholder until T49. The monitor samples the one avatar viewport at 10 Hz and adds no second
   render or camera passthrough.

## Consequences

- The app launches Live with the real output gate and panic, but shows `TRK-007` ("Face tracking
  isn't running") until T26/T32 wire capture and tracking. The four states are reachable through the
  documented debug trigger in this slice; T41 owns the full state table and manual triggers.
- T41 reuses the panic gate and the code/sentence/action shape; it must not add a second panic path.
- T49 replaces the placeholder output label and adds consumer/transport status without changing the
  panic semantics.
- T33 owns the four-mode shell and the full accessibility/i18n pass; T25's HUD copy is deliberately
  centralised so it can move to the string table.
- The global hotkey is best-effort by design. A failed registration is a visible state, never a
  silent failure.

Evidence: `avatar_live_unit`, the panic cases in `avatar_output_unit`, `app/tests/live_hud.gd`
(headless and Forward+ rendered) and `tests/gates/T25.ps1`.
