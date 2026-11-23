# Editor and UX

**Status:** proposal; minimal Live surface, panic blank, live edit guard and four degraded states implemented (T25); the diagnostic three-pane Setup window with manual mapping, freeze and still sources implemented (T25c); full editor shell, all error surfaces and i18n planned · **Last updated:** 2026-11-22

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
4. **Face profile.** A neutral capture runs: face the camera, eyes open, mouth closed. The app
   averages 15 frames and rejects tilt, blink, or low confidence
   ([`user-data.md`](user-data.md#capture)). The profile is named `Default` and can be renamed
   later.
5. **Avatar calibration** runs; the user is asked to look at the camera and blink once (used to
   verify the eye regions actually land on their eyes).
6. **"Looks right?"** with two buttons: *Yes, go live* and *Adjust*. The adjust path is the
   calibration gizmo, not a settings tree.
7. The profile is saved for reuse; the user never has to do this again unless their appearance or
   lighting changes drastically.

Total target: under five minutes, with the user's own model. Measured, not assumed
([`product-overview.md`](product-overview.md#success-criteria)).

---

## Face calibration

The single most important flow. Three entry points: first run, "Face" inspector panel, and a warning
banner when confidence is low or the face visibly floats.

**Profiles.** The active `FaceProfile` is per person, not per avatar. Settings → Profiles lists them,
with *New*, *Rename*, *Recalibrate*, and *Delete*. Switching is explicit; the app never guesses who is
in front of the camera. The first-run profile is created automatically.

### Layout

```
┌──────────────────────────────────────────────────────────────┐
│  Live preview (mirrored)             │  Face                  │
│                                      │  ─────                 │
│   ┌──────────┐                       │  Surface  ● Plane ○ Cylinder
│   │  ◯   ◯   │  ← draggable regions  │  Width    ─────●─────  0.09 m
│   │    ▭     │                       │  Height   ────●──────  0.11 m
│   └──────────┘                       │  Depth    ───●───────  0.02 m
│                                      │  Tilt     ──────●────  2°
│  [x] Show mesh  [x] Freeze head      │  ─────────────────────
│  [ ] Show camera feed (preview only) │  Head     ● Cut ○ Node
│                                      │  Neck     ──────●────  0.02 m
│                                      │  Falloff  ────●──────  0.04 m
│                                      │  Feather  ────●──────  0.18
│                                      │  Exposure ─────●─────  0.0
│                                      │  [ Match to model ]    │
└──────────────────────────────────────────────────────────────┘
```

The inspector is a view of the same state as `avatar.json`: **Width** is the model's inter-ocular
distance (`face.surface.scale`), and **Height**, **Depth** and **Tilt** position and orient the face
plane (`face.surface.origin`, `normal`, `up`). There is no second source of truth.

The preview is not a second render: T16's `AvatarRuntime` shows a `TextureRect` sampling the one
1920x1080 avatar `SubViewport` texture. Editor overlays (regions, mesh, camera feed) draw on top of
that copy in the window and never enter the avatar viewport. A hidden debug HUD, fed by C++
counters, can be toggled for support without changing the scene; the user-facing Live HUD is T25.

### Behaviours

- **Auto-placement** estimates the face plane from the model's bounding box and the region shapes
  from the head's aspect ratio. It is a starting point and is labelled as such.
- **Rotation centre**: derived from the face surface when the package does not set one; an advanced
  handle moves it. Needed for models whose origin sits at the feet rather than inside the head.
- **Head region**: *Cut* drags a plane and falloff at the neck and highlights the vertices that will
  rotate; *Node* lets the user click the head to select a subtree. The body is never deformed. A
  plane above the chin or an extreme falloff warns rather than blocks.
- **Model check on import** names the reported unit and axis, mesh and triangle counts, and whether a
  rig or animation was ignored, then says what must be calibrated. It never blocks a model that
  parses.
- **Direct manipulation**: drag a region to move, corner handles to resize, a ring handle to rotate,
  scroll to feather. Every gesture has a numeric equivalent in the inspector (both are always
  visible, so users can be precise).
- **Freeze head** holds the avatar in a neutral pose while the user drags, so the target does not
  move under the cursor. Essential; without it, calibration is maddening.
- **Show camera feed** overlays the raw webcam behind the model at low opacity during calibration
  only, so the user can check alignment. It is a preview overlay and never exists in the output
  path.
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

## Framing

Head rotation is rotation-only in 1.0 and applies to the selected head region, not the whole avatar.
Framing controls how the whole avatar is placed and never moves with the head. The editor exposes:

| Control | Effect |
|---|---|
| Fit | `Model` (whole model) or `Head` (face-centred, for full-body models) |
| Zoom | 0.5–2.0; 1.0 is the computed fit |
| Offset | Drag or two sliders; fractions of the visible half-extent |
| Margin | 0–0.5 padding around the fitted extent |
| Mirror preview | Display-only; on by default |
| Mirror output | Flips the final frame, including props; off by default, warns |

A bounds overlay shows the model's AABB and the camera frustum while framing. Leaning does not move
the avatar; a translation gain is `Deferred`. The `Show` may override the package's framing.

The T22 adapter already exposes the overlay data through `AvatarRuntime.get_bounds_overlay()`:
rest and swept AABBs, the fitted sphere, six world-space frustum planes, eight frustum corners, the
head sweep and containment flags. It is data only; T36 draws it outside the output `SubViewport`.
Zoom clamps to 0.5–2.0 and margin to 0–0.5. Containment is guaranteed at zoom ≤ 1; zoom > 1 is the
deliberate crop control. Preview mirroring flips the preview copy only; output mirroring flips the
final composited frame.

### Quality presets

| Preset | Output | Tracking | Rendering |
|---|---|---|---|
| Quality | 1080p60 | 60 Hz when the camera supports it, otherwise 30 Hz | Decal mode as authored; 2× MSAA; full-resolution camera |
| Balanced (default) | 1080p60 | 30 Hz | Decal capped at `surface`; no MSAA; full-resolution camera |
| Performance | 720p60 | 30 Hz | Decal capped at `surface`; no MSAA; prefer 720p60 capture |

Presets are a ceiling, not a per-avatar override: an avatar authored for `flat` stays `flat` under
Quality. If p99 frame time exceeds 16.6 ms for 3 s, the app suggests one step down and shows the
reason; it never changes quality silently. The preset is stored in the Show, so a laptop and a
desktop can differ.

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

- **Panic button** ("hide face"): instantly blanks the output and mutes the virtual camera, with the
  reason shown in the preview. For when something goes wrong on stream. Bound to a global hotkey.
  This is a streamer-specific requirement that is easy to overlook and impossible to add later
  convincingly. It blanks; it never substitutes a static face.
- **Status is honest**: transport state, actual fps, dropped frames, tracking confidence. If the
  output is degraded, it says so in Live, not only in a log.
- **No editing while live** except outfits and appearance, which are safe. Changing the avatar/model,
  face calibration, camera, output resolution, or transport requires Stop; the UI offers
  *Stop & switch* rather than reconfiguring the live pipeline. There is no preloaded avatar switch in
  1.0 ([`roadmap.md`](roadmap.md#post-10-candidates)).
- **OBS setup card.** If a transport is enabled and no consumer connects within a few seconds, Live
  shows the plugin name, minimum version, link, and *Test connection*, with window capture as the
  fallback ([`output-and-virtual-camera.md`](output-and-virtual-camera.md#transports-for-cooperating-apps)).

### Implemented Live surface (T25)

The vertical slice ships the honest surface, not the full editor shell (T33) or every error state
(T41). `app/scenes/live_hud.gd` overlays the existing mirrored preview and shows:

- **Status bar:** LIVE/STOPPED, elapsed time, actual fps, frame time, tracking confidence, face state,
  output state, and the global-hotkey sentence. The numbers come from `AvatarRuntime` and
  `avatar::live::LiveSession`; the HUD updates at 4 Hz, not per frame.
- **Status banner:** one sentence plus one action, with the support code. The four T25 states are
  camera lost (`CAP-001`, "Camera disconnected"), face not visible (`TRK-001`, "Can't see your face"),
  eyes or mouth hidden (`TRK-008`, "Can't see your eyes and mouth"), and tracking degraded (`TRK-004`,
  "Face tracking is struggling"). The banner never hides the face; it sits over the preview.
- **Panic:** the button and Ctrl/Cmd+Shift+H blank the output immediately through
  `OutputManager::set_panic`, clear the hold, and show "Output is hidden" plus the restore action in
  the preview. The same binding restores. A static or frozen face is never substituted.
- **Global binding:** a Windows `RegisterHotKey` listener is attempted on startup. When the OS
  refuses (or on a platform without a supported listener) the HUD says the in-app binding still
  works; it never silently fails.
- **Edit guard:** avatar, face calibration, camera, output resolution and transport changes are
  refused while live with a *Stop and switch* action. Outfit and appearance remain safe.
- **Output placeholder:** the window transport is real, but the monitor is labelled as a placeholder
  until T49 owns transport status. It samples the one avatar viewport at 10 Hz; it does not add a
  second render.
- **UI scale:** the surface cycles 100/150/200% and keeps the top and bottom bars inside the window.

The app has no live tracker wiring yet (T26/T32), so a normal launch shows
`TRK-007` ("Face tracking isn't running") and tracking `-`. The four states above are reachable in
this slice through the documented debug trigger (`-- --live-debug`, or the test harness) and through
the real pipeline once T26 lands. This is recorded as a T25 deviation rather than presented as a
working camera path. See [ADR-0035](adr/0035-live-hud-panic-and-edit-guard.md).

---

## Settings

| Section | Contents |
|---|---|
| Camera | Device, resolution/fps preference, format diagnostics, exposure/WB lock, mirror preview |
| Matte | Auto / chroma / software / off, key colour, *Test matte*, similarity and softness |
| Profiles | Active FaceProfile; New, Rename, Recalibrate, Delete |
| Appearance | Environment preset, key-light azimuth/elevation/intensity, per-region colour |
| Output | Transports, alpha mode, virtual camera install/uninstall, framing, mirror output, background |
| Performance | Quality preset (Quality/Balanced/Performance), tracking rate, output resolution, renderer info |
| Privacy | Diagnostics opt-in, log location, delete face profiles, delete all app data, data summary |
| About | Versions (app, engine, tracker model), licences, dependency manifest, support codes |

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
| Camera lost mid-stream | "Camera disconnected" | Auto-retry; output blanks |
| Face not visible | "Can't see your face" | Hint about lighting/position; output blanks |
| Eyes or mouth hidden | "Can't see your eyes and mouth" | Output is blank; remove sunglasses, face the camera |
| Tracking degraded | "Face tracking is struggling" | Suggest lower resolution or brighter light |
| GPU unsupported | "This graphics card isn't supported" | Explain Forward+/Vulkan/Metal requirement |
| Renderer is Compatibility | "OpenGL renderer is not supported" | Explain and offer to switch |
| Virtual camera not installed | "Virtual camera isn't set up" | Install (may require admin/approval) |
| Virtual camera blocked (macOS) | "macOS is waiting for your approval" | Open System Settings → Privacy & Security |
| Output consumer disconnected | "OBS isn't receiving frames" | Show transport name + troubleshooting |
| OBS plugin missing | "OBS needs the Spout2 plugin" | Setup card with link; use window capture |
| Package invalid | "This avatar can't be opened" | Show the first validation error with its JSON path |
| Library import failed | "Couldn't add this avatar" | Name the path and free space; Retry |
| Face profile unreadable | "Your face profile couldn't be read" | Create a new profile; keep a `.bad` copy |
| Model has no face-like region | "We couldn't find the face — place it yourself" | Opens calibration with the surface gizmo active |
| Software matte unavailable | "Using a basic face mask" | Region mask only; explain that a green screen gives a cleaner edge |
| Head region unchecked | "Check what turns with the head" | Opens the head-region gizmo |
| Session locked | "Paused while your screen is locked" | Resume on unlock; capture stays off |
| Second instance | "Avatar Studio is already running" | Activate the existing window |
| Virtual camera needs Windows 11 | "This camera needs Windows 11" | Use OBS Virtual Camera; explain the fallback |

Rule: no error is a bare code. Code + human sentence + one action.

### Second instance (T25a)

A second launch never opens a second camera or output path. It forwards `activate` to the running
instance and exits. The running window is raised and focused, and its banner says:

| Code | Sentence | Action |
|---|---|---|
| `CFG-007` | Avatar Studio is already running | Brought the existing window forward |

If the running instance exists but does not acknowledge within 1.5 s, the second launch shows the
same sentence with the action *Activate the existing window* and exits non-zero; it still does not
open a camera. The activation may carry an avatar or show path; T42 owns opening it.

`--new-instance` is a testing escape hatch, not a user-facing second-avatar mode. It gets distinct
lock, activation and shared-memory names, disables the virtual camera and output registration, and
shows "Test instance: virtual camera and output registration are off." If the primary already holds
the camera, the test instance shows the normal `CAP-002` busy state.

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
- Changing the avatar/model while live; that requires Stop/Go Live.
- Lighting beyond the environment presets and key-light override; no HDRI or multi-light rig.
- Recording or streaming.

Each of these exists in a better tool. The editor's value is the calibration and the preview.

## Setup window (T25c)

The diagnostic Setup window is one opaque surface with three live panes and a control strip. It
opens over the Live surface (`Setup` button or F2) and is deliberately not the OBS output: the
transparent output window remains a separate surface with its own gate.

- **Camera** renders the raw T24 ring slot with the same NV12/BT.601/709 decode the decal uses, then
  overlays the dense tracker landmarks, the face box, the canonical-to-image homography quad and the
  three eye/mouth region anchors. The camera pixels never enter the avatar SubViewport or the OBS
  output. The operator can activate **L eye / R eye / Mouth** and drag a box around the corresponding
  feature; the box is inverted through the live homography into canonical face coordinates and
  becomes that region's centre and size. With no mode active, drag a cutout outline to move it (the
  grab offset is preserved, so it does not snap under the pointer) or drag its corner handle to scale
  it. The cutouts are projected through the same per-frame homography and FaceRig anchor the decal
  uses, so they follow the face as the subject moves and turns. When the rig has no live solve the
  window holds the last good homography and anchors, draws the cutouts dashed in amber and labels the
  pane **Mapping held - no live face** instead of parking them at the identity projection the rig
  reports while it has no output. Before the first solve (a development still, or the first frames)
  the tracker face box stands in for the mapping, the cutouts are dashed and the pane says
  **No live mapping - face-box fallback**; with no face box at all it says **No live face mapping
  yet** and draws no cutouts. The fallback and held states are diagnostics only and are never
  presented as a calibrated mapping.
- **Mapping** draws the canonical-face values the decal actually consumes: the authored region rects,
  the live centroid plus rest-delta anchor, the head cut plane and falloff, the pivot at the face
  origin and the homography rows. Moving a slider changes these values; it is not a decorative
  diagram. The same draw/move/scale interaction works on this grid: the pointer converts directly to
  canonical face coordinates.
- **Model** samples the avatar SubViewport and draws the three diagnostic cutout outlines over it.
  Clicking an outline selects it; dragging unprojects the pointer onto the same face plane the decal
  shader uses and moves the region live, preserving the grab offset. With a region mode active,
  dragging a box on the model sets that region's centre and size, just like drawing on the camera;
  when no mode is active, dragging the square handle at the selected outline's bottom-right corner
  scales it about its centre. The white/black background toggle is in this pane. The OBS output is a
  separate transparent window and never includes these outlines.

All three panes share one interaction contract and one selection: clicking a cutout in any pane
selects it everywhere, and drawing a box in any pane updates the same authored region. Re-applying
the current draw mode is a no-op, so the selection signal cannot cancel an in-progress drag.

The three panes sit in nested split containers: the dividers are draggable, so the operator can
resize the camera, mapping and model panes without leaving the window. The default surface is
1920x1080 logical (about 1280x720 physical on a 150% display); the window is resizable and the
panes stay proportional at every size.

The bottom strip has a fixed action row (Apply to model, Reset, Snapshot, Load still, Use camera,
Back to Live) and a scrollable fine-tune area for the eye L/R and mouth centres/sizes, face
scale/origin, head cut/falloff and the capture/tracker selectors. `Apply to model` writes
`user://live_calibration.json`; `Reset` restores the provisional AABB mapping without saving.
Capture-mode and tracker-input selectors restart the native pipeline with the requested values.

Two development-only sources support unattended setup work. `Snapshot` captures the current frame,
freezes the native pipeline on it (holding the last ring slot, tracker result and FaceRig pose
without the 500 ms timeout or 150 ms loss hold), saves it to `user://T25c-snapshot.png`, and labels
the source `still (dev)`; `Use camera` unfreezes. `--live-source still --still <path>` (or `Load
still...`) publishes a PNG/JPEG as camera frames at the pipeline fps, runs the tracker/FaceRig/decal
and labels the source `still (file)` with its path. The OBS output is blanked while the diagnostic
freeze is active, so neither the frozen frame nor a held avatar pose reaches the output. The human
review always uses the real camera; the still source is not M1 gate evidence.

