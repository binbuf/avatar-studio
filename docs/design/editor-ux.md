# Editor and UX

**Status:** proposal · **Last updated:** 2026-11-20

The editor edits *avatar configuration*, not geometry. It has one job: get a user from "I have a
`.glb`" to "I am live and it looks right" in minutes, without explaining how the renderer works.

---

## Principles

1. **The preview is the interface.** Every adjustment happens against a live, head-tracked preview.
   No modal dialogs that hide the face, no apply/OK cycles.
2. **One screen, one decision.** Library → Editor → Live are distinct modes. The editor is not
   reachable while live in a way that can break the stream.
3. **Auto-suggest, then let them fix it.** Placement is guessed from the mesh and refined by direct
   manipulation. A guess that is wrong must be obviously adjustable, not mysteriously wrong.
4. **Explain the failure, not the stack.** "Another app is using your camera" beats
   `MF_E_VIDEO_RECORDING_DEVICE_IN_USE`.
5. **Never lose work.** Autosave to a recovery file; saving is atomic; importing a new model never
   destroys the previous calibration without confirmation.

---

## Information architecture

```
┌────────────────────────────────────────────────────────────────────────────┐
│  Library            Editor              Live                    Settings   │
├────────────────────────────────────────────────────────────────────────────┤
│                                                                            │
│   ┌─────────────┐   ┌────────────────────────────────┐  ┌───────────────┐  │
│   │ Avatar list │   │                                │  │ Inspector     │  │
│   │             │   │        3D preview              │  │               │  │
│   │  Avocado    │   │      (live tracked)            │  │ ▸ Face        │  │
│   │  Potato     │   │                                │  │ ▸ Props       │  │
│   │  + Import   │   │   [drag handles on regions]    │  │ ▸ Appearance  │  │
│   │             │   │                                │  │ ▸ Tracking    │  │
│   └─────────────┘   └────────────────────────────────┘  └───────────────┘  │
│                                                                            │
│  Camera: FaceTime HD ▾   1080p30 ▾   ● Live preview   [ Go Live ]           │
└────────────────────────────────────────────────────────────────────────────┘
```

Four top-level modes, no nested navigation:

| Mode | Purpose | Leaves it via |
|---|---|---|
| **Library** | Pick, import, duplicate, delete avatars and outfits | Selecting an avatar |
| **Editor** | Calibrate face, place props, tune appearance and tracking | Go Live / back |
| **Live** | Preview + output status + quick outfit switch | Stop |
| **Settings** | Camera, output, performance, privacy, diagnostics | Back |

---

## First run

1. Welcome: one sentence about what the app does, and a **camera permission** request with a reason.
   No permission prompt at launch before the user knows why.
2. Camera check: live thumbnail, device picker, "no camera found" path with a fix link.
3. Pick a bundled avatar (three CC0 examples) or **Import .glb**.
4. Auto-calibration runs; the user is asked to look at the camera and blink once (used to verify
   the eye regions actually land on their eyes).
5. **"Looks right?"** with two buttons: *Yes, go live* and *Adjust*. The adjust path is the
   calibration gizmo, not a settings tree.
6. FaceProfile is saved for reuse; the user never has to do this again unless their camera or
   lighting changes drastically.

Total target: under five minutes, with the user's own model. Measured, not assumed
([`product-overview.md`](product-overview.md#success-criteria)).

---

## Face calibration

The single most important flow. Three entry points: first run, "Face" inspector panel, and a warning
banner when confidence is low or the face visibly floats.

### Layout

```
┌──────────────────────────────────────────────────────────────┐
│  Live preview (mirrored)             │  Face                  │
│                                      │  ─────                 │
│   ┌──────────┐                       │  Surface  ○ Plane ● Cylinder
│   │  ◯   ◯   │  ← draggable regions  │  Width    ─────●─────  0.09 m
│   │    ▭     │                       │  Height   ────●──────  0.11 m
│   └──────────┘                       │  Depth    ───●───────  0.02 m
│                                      │  Tilt     ──────●────  2°
│  [x] Show mesh  [x] Freeze head      │  ─────────────────────
│  [ ] Show camera feed                │  Feather  ────●──────  0.18
│                                      │  Exposure ─────●─────  0.0
│                                      │  [ Match to model ]    │
└──────────────────────────────────────────────────────────────┘
```

### Behaviours

- **Auto-placement** estimates the face plane from the model's bounding box and the region shapes
  from the head's aspect ratio. It is a starting point and is labelled as such.
- **Direct manipulation**: drag a region to move, corner handles to resize, a ring handle to rotate,
  scroll to feather. Every gesture has a numeric equivalent in the inspector (both are always
  visible, so users can be precise).
- **Freeze head** holds the avatar in a neutral pose while the user drags, so the target does not
  move under the cursor. Essential; without it, calibration is maddening.
- **Show camera feed** overlays the raw webcam behind the model at low opacity, so the user can see
  whether their real eyes line up with the avatar's.
- **Blink test**: a button that highlights both eye regions and asks the user to blink; if the
  regions do not cover the eyes, it says so. This catches the most common mis-calibration without
  requiring the user to judge it themselves.
- **Yaw sweep**: a guided prompt to turn left and right, showing a live "detachment" indicator
  (the angular error between the decal normal and the view direction at the region centre). If it
  goes red, the surface type is probably wrong.
- **Match to model** samples the model's albedo around each region and sets colour gain so the face
  is not a different photograph ([`face-compositing.md`](face-compositing.md#colour-and-light-matching)).

### Undo

Every calibration change is undoable (`Ctrl/Cmd+Z`), including auto-placement. The stack is
per-session and does not survive a save.

---

## Props

- Drag from a library panel onto an anchor; drop targets highlight when a drag starts.
- After placement, the inspector shows anchor, offset (position/rotation/scale) and a "snap to
  surface" toggle.
- Auto-fit for hats and glasses: scale and position are derived from the head's bounding box, then
  the user nudges. It is right about half the time, which is fine because adjusting is one drag.
- Props that are attached to `hand_l`/`hand_r` follow the model's static hands. Without hand
  tracking, held props are decorative; the UI says so rather than implying interaction.
- Outfits are named sets of enabled props + offsets. Switching an outfit while live is instant and
  does not restart the stream (that is the point of the format's separation of avatar and outfit).

---

## Live mode

```
┌───────────────────────────────────────────────────────────────┐
│  ● LIVE   00:14:32                          1080p60 · 58 fps  │
│                                                               │
│   ┌─────────────────────────┐   Output                        │
│   │                         │   ● Spout2 "AvatarStudio"       │
│   │   Preview (mirrored)    │   ○ Virtual camera  (installed) │
│   │                         │   ○ Window capture              │
│   └─────────────────────────┘                                 │
│                                                               │
│   Outfit: [ Default ▾ ]      Camera: FaceTime HD ▾            │
│   [ Stop ]   [ Panic: hide face ]                             │
└───────────────────────────────────────────────────────────────┘
```

- **Panic button** ("hide face"): instantly replaces the decals with a neutral, non-live face and
  mutes the virtual camera to a placeholder. For when something goes wrong on stream. Bound to a
  global hotkey. This is a streamer-specific requirement that is easy to overlook and impossible to
  add later convincingly.
- **Status is honest**: transport state, actual fps, dropped frames, tracking confidence. If the
  output is degraded, it says so in Live, not only in a log.
- **No editing while live** except outfits and appearance, which are safe. Anything that
  reconfigures capture or output asks for confirmation because it will drop frames.

---

## Settings

| Section | Contents |
|---|---|
| Camera | Device, resolution/fps preference, format diagnostics, exposure/WB lock, mirror preview |
| Output | Transports, alpha mode, virtual camera install/uninstall, background for opaque output |
| Performance | Quality preset, tracking rate, output resolution, renderer info |
| Privacy | Diagnostics opt-in, log location, "delete recordings", data summary |
| About | Versions (app, engine, tracker model), licences, dependency manifest |

Advanced and diagnostic values live behind a single "Advanced" disclosure. Nothing that can break
the stream is one accidental click away.

---

## States and error surfaces

Every one of these has designed copy, an icon, and a recovery action:

| State | Message | Action |
|---|---|---|
| No camera | "No camera found" | Rescan, open settings |
| Camera denied | "Camera access is blocked by Windows/macOS" | Deep-link to the OS privacy page |
| Camera busy | "Another app is using this camera" | List likely apps, Retry |
| Camera lost mid-stream | "Camera disconnected" | Auto-retry; output shows placeholder |
| Face not visible | "Can't see your face" | Hint about lighting/position; output holds last pose |
| Tracking degraded | "Face tracking is struggling" | Suggest lower resolution or brighter light |
| GPU unsupported | "This graphics card isn't supported" | Explain Forward+/Vulkan/Metal requirement |
| Renderer is Compatibility | "OpenGL renderer is not supported" | Explain and offer to switch |
| Virtual camera not installed | "Virtual camera isn't set up" | Install (may require admin/approval) |
| Virtual camera blocked (macOS) | "macOS is waiting for your approval" | Open System Settings → Privacy & Security |
| Output consumer disconnected | "OBS isn't receiving frames" | Show transport name + troubleshooting |
| Package invalid | "This avatar can't be opened" | Show the first validation error with its JSON path |

Rule: no error is a bare code. Code + human sentence + one action.

---

## Keyboard and accessibility

| Action | Binding |
|---|---|
| Go live / stop | `Ctrl/Cmd+Enter` |
| Panic (hide face) | `Ctrl/Cmd+Shift+H` (global where the OS allows it; in-app otherwise) |
| Undo / redo | `Ctrl/Cmd+Z` / `Ctrl/Cmd+Shift+Z` |
| Toggle mesh overlay | `M` |
| Freeze head | `Space` |
| Reset region | `R` |
| Cycle outfit | `[` / `]` |

Global hotkeys require OS-level permissions on macOS and may be unavailable; the in-app binding
always works, and the UI says so rather than silently failing.

Accessibility commitments for 1.0:

- Full keyboard operation of the editor, with visible focus.
- UI scale 100–200% independent of OS scaling.
- No information conveyed by colour alone (region states also change shape/label).
- Respect reduced-motion: no camera shake, no animated transitions beyond a cross-fade.
- Text contrast ≥ 4.5:1 in both themes.

---

## Internationalisation

- All user-facing strings in resource files from the start; no string literals in UI code.
- Number, date and unit formatting through the platform locale.
- Layouts tolerate +40% string growth (German baseline).
- 1.0 ships English only; the pipeline exists so that adding a language is a translation task, not a
  refactor.

---

## What the editor deliberately does not do

- Model geometry, UV editing, rigging, weight painting.
- Texture painting or material graph authoring (material *overrides* only).
- Scene composition with multiple characters.
- Recording or streaming.

Each of these exists in a better tool. The editor's value is the calibration and the preview.
