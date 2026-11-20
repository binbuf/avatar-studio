# Product Overview

**Status:** proposal · **Last updated:** 2026-11-20

---

## The problem

Streamers and creators want a live character that reads as a *real* face, not an animated mask.
The recognizable version of this effect (TheBurntPeanut-style) composites the creator's actual
eyes and mouth onto a rigid 3D object. The result is unusually expressive because blinks, pupil
movement, lip shape and micro-expressions are real pixels, not animation.

Today this requires one of:

1. Snap Lens Studio (free but proprietary, Windows/macOS only, effect locked to Snap's runtime),
2. a bespoke Blender + OBS + scripting pipeline that only its author can maintain, or
3. source-available hobby projects that are either non-commercial (`pster-live-avatar`, PolyForm
   Noncommercial) or not applications at all (`Mixed-Reality`, `vtubing` — foundations and demos).

None of them let a non-technical user load an arbitrary model, place a live face on it, attach
props, save the result, and go live in a video call or on stream — under a license that permits
commercial use.

## The insight

The effect does not need facial rigging, blendshapes, or a special mesh. It needs three things:

1. a rigid model with a plausible face surface,
2. a head pose and a *head-stable* face coordinate frame,
3. a projection that samples webcam pixels in that frame, so the face stays glued to the model as
   it rotates.

Everything else — editor, props, presets, output plumbing — is product work around that core.
The core is a well-understood rendering problem; the product is the hard part.

## Product principles

| Principle | Consequence |
|---|---|
| **The face is the product** | Rendering quality, latency and stability outrank feature count. Every milestone is judged on how convincing the face looks in motion. |
| **One render, many destinations** | The avatar renders once into a dedicated viewport; transports fan it out. No destination may block rendering. |
| **Never require another app** | OBS is optional. A virtual camera works with Discord/Zoom/Teams directly. |
| **Works offline, always** | No network calls in the default path. No accounts. No telemetry without explicit opt-in. |
| **Rigid models only** | No rig, no bones, no blendshape requirement. If a user has a `.glb`, they have an avatar. |
| **Data, not code** | Avatar packages are declarative JSON plus glTF. Nothing in a package executes. |
| **Degrade, never fail** | Camera unplugged, face lost, consumer disconnected, GPU device reset: the app keeps running and says what happened. |

## Users

| Persona | Need | What they get |
|---|---|---|
| **Streamer** (primary) | A distinctive live face with minimal setup; transparent overlay in OBS | Import model, calibrate once, save preset, add Spout/Syphon/PipeWire source in OBS |
| **Call participant** | Appear as a character in Zoom/Teams/Discord without OBS | Pick "Avatar Studio Camera" in the app's camera list |
| **Model author / 3D hobbyist** | Turn an existing model into a usable avatar without learning a game engine | Editor with anchors, prop slots and a portable `.avatar` package |
| **Tinkerer / contributor** | Understand and extend the pipeline | Documented interfaces, replaceable tracker and transports, MIT license |

Non-users, explicitly: VTuber performers who need full-body tracking and rigged expression
animation, and mobile-only creators.

## Platforms

1.0 targets Windows 11 (Windows 10 best-effort for capture and OBS output, without the native virtual
camera), macOS 13+ on Intel and Apple Silicon, and Linux x86-64. The exact matrix, including what is
blocking QA, is in [`architecture.md`](architecture.md#supported-platforms).

## Scope

### In scope for 1.0

| Capability | Status |
|---|---|
| Import `.glb`, or pick a bundled character | `Planned` |
| Live eyes and mouth from webcam, three quality modes | `Planned` |
| Head-driven rotation with smoothing and loss handling | `Planned` |
| Rotation-only framing with model/head fit, zoom, offset, mirroring | `Planned` |
| Face calibration UI (auto-suggest + manual adjustment) | `Planned` |
| Named FaceProfiles, reusable across avatars | `Planned` |
| Managed avatar library (copy on import) | `Planned` |
| Named prop anchors and prop library (hats, glasses, held items) | `Planned` |
| Save/load `.avatar` packages and outfits/presets; live outfit switching | `Planned` |
| Four environment presets plus key-light control | `Planned` |
| Quality presets (Quality/Balanced/Performance) | `Planned` |
| Transparent output to OBS via Spout2 / Syphon / PipeWire | `Planned` |
| System virtual camera on Windows 11, macOS, Linux | `Planned` |
| Window output and OBS window-capture fallback | `Planned` |
| Camera selection, format negotiation, hot-plug handling | `Planned` |
| Stable support codes and diagnostics export | `Planned` |

### Deferred

| Capability | Why deferred | Re-entry condition |
|---|---|---|
| Fitted clothing (shirts, jackets) | Requires per-character deformation or a cloth fitting system; props on anchors cover the common case | Demand from model authors plus a rigged-character format decision |
| Rigged characters / blendshape-driven expressions | The core effect does not need it; adds a second animation path | If expression data is already tracked, driving rigs is additive |
| Multi-face / guest avatars | Doubles tracking cost and editor complexity | After 1.0, if streamer collab use is validated |
| Full 6-DOF head translation | Rotation-only keeps stream framing stable; translation needs a follow camera or a gain/clamp model | Demand for leaning/position expression plus a framing design |
| Live avatar/model switching | Loading a GLB and rebuilding the scene on the live path risks a visible gap; outfits cover the common case | Preloaded-switch design validated after 1.0 |
| Lens distortion correction | Accepted error at typical webcam distance; no intrinsics dependency in 1.0 | M2 quality tests show a visible error at close range |
| FaceProfile export/import | Biometric template; no safe, warned transfer flow yet | An encrypted, explicit export design |
| ARM64 Windows/Linux | x64 first; a second architecture doubles the build matrix | Stable 1.0 plus demonstrated demand |
| Hand/body tracking | Different product | Not planned |
| Built-in recording / streaming | OBS exists | Not planned |
| Mobile, web, XR | Different rendering and permission models | Not planned |

## Non-goals

- **Cloning a specific creator's character.** The technique is reimplemented; the artwork, name and
  branding are not. See the legal posture below.
- **A general-purpose 3D editor.** The editor edits *avatar configuration*, not geometry.
- **A plugin for the Godot editor.** This is a standalone application; the engine is an
  implementation detail.
- **A webcam relay or a virtual-camera-only utility.** Those exist and are simpler; the value here
  is the avatar.

## Success criteria

1.0 is successful when all of the following hold, measured on the reference hardware class
(see [`testing-and-quality.md`](testing-and-quality.md#reference-hardware)):

| Criterion | Target | How measured |
|---|---|---|
| Face reads as attached to the model at ±30° yaw | No visible detachment, no "floating sticker" | Blind A/B review of recorded clips |
| Output frame rate | 60 fps sustained at 1080p | Frame-time percentiles over 10 minutes |
| App-side latency (capture callback → output frame) | ≤ 70 ms median, ≤ 100 ms p95 | Timestamp instrumentation, see [`architecture.md`](architecture.md#latency-budget) |
| Setup time for a new user with a prepared model | ≤ 5 minutes to first live avatar | Moderated usability run, 5 participants |
| Crash-free sessions | ≥ 99.5% | Local crash log counters (opt-in reporting) |
| Cold start to live preview | ≤ 4 s | Instrumented startup |

These are engineering targets, not measurements. Nothing here has been benchmarked yet.

## Legal posture

This is a **product policy**, not legal advice, and it is not a clearance opinion. Counsel review
is a release gate.

| We do | We do not |
|---|---|
| Reimplement the technique (projection of live facial regions onto a rigid mesh) | Ship or derive from TheBurntPeanut's model, textures, or artwork |
| Ship original or CC0 default characters | Use his name, logo, or confusingly similar branding |
| Let users import their own licensed models | Advertise the app as an official or endorsed product of any creator |
| Record per-asset provenance in packages | Accept packages with unknown-licence assets into the bundled library |

Techniques and methods are generally not protected by copyright; specific expression is. Trademark
risk attaches to branding and trade dress, not to the existence of a peanut-shaped avatar with real
eyes. The source brief's claim about specific trademark applications was not verified and is not
relied upon. Details and the asset review process: [ADR-0007](adr/0007-licensing-and-assets.md).

## Naming and identity

"Avatar Studio" is a working name. It is deliberately generic and descriptive; a distinctive name
is a branding decision, not an architectural one, and is not blocking. The virtual camera presents
itself to the OS as **Avatar Studio Camera**, which is user-visible and therefore part of the
naming decision.

## Prior art reviewed

| Project | License | What we take | What we do not |
|---|---|---|---|
| `J-Pster/pster-live-avatar` | PolyForm Noncommercial | Nothing. Read for technique confirmation only. | No code. The license is incompatible with unrestricted commercial use. |
| `MParamos/Mixed-Reality` | MIT | Face-mesh and inset ideas worth reading | No direct code dependency; unmaintained |
| `DarkStar1997/vtubing` | MIT | Reference for MediaPipe→VRM pipelines | Animates expressions instead of compositing pixels; different core |
| `Quiet-Terminal-Interactive/SpectatorStreamExporter` | MIT | Frame queue, async readback, transport backend shape | Its CPU-copy transport is a starting point, not the destination |
| Snap Lens Studio "Face Inset" | Proprietary | Conceptual validation that insets on 3D objects work without special geometry | No dependency, no format |

Any code reuse requires a license audit recorded in the asset/dependency manifest
(see [`security-privacy.md`](security-privacy.md#dependency-and-asset-manifest)).
