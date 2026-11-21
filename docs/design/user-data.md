# User Data, Profiles, and Library

**Status:** proposal; the FaceProfile named-landmark subset, canonical correspondence
validation and JSON round-trip are implemented (T12). Capture, storage and lifecycle remain
planned (T31). **Last updated:** 2026-11-20

This document defines everything the application persists outside an `.avatar` package: the managed
avatar library, per-person face profiles, shows, settings, and the migration, permission, and
deletion rules that apply to them. Package contents are specified in
[`avatar-package-format.md`](avatar-package-format.md); this document is about the user's machine.

---

## Principles

| Principle | Consequence |
|---|---|
| **The library owns its copies** | Importing a package copies it into app data. The original is never modified or required again. |
| **Profiles follow the person, not the model** | One `FaceProfile` works across every avatar; avatar calibration stays in the package. |
| **Every write is atomic** | Write to a temporary file in the same directory, then rename. A crash never leaves a half-written profile, show, or settings file. |
| **User-only by default** | Directories and files are readable only by the OS user. Packages and profiles are never world-readable. |
| **Indexes are rebuildable caches** | Deleting an index loses no data. The directory tree is the source of truth. |
| **No cloud, no sync** | Nothing here is uploaded. Export and import are explicit file operations. |

---

## Directory layout

Per platform, the application uses the OS convention for local application data:

| Platform | Root |
|---|---|
| Windows | `%LOCALAPPDATA%\AvatarStudio\` |
| macOS | `~/Library/Application Support/AvatarStudio/` (the app-group container when sandboxed) |
| Linux | `$XDG_DATA_HOME/avatar-studio/` (default `~/.local/share/avatar-studio/`) |

Under the root:

```
<data>/
├── library/
│   └── <package-id>/            # a complete, copied .avatar package
├── profiles/
│   └── <profile-id>.json        # FaceProfile, one per person
├── shows/
│   └── <show-id>.json           # machine-specific live configuration
├── settings.json                # global settings, active profile, UI state
├── library.json                 # rebuildable index cache
├── backups/
│   └── <app-version>/           # pre-migration copies, last two kept
├── logs/                        # rotated, 7 days
├── recordings/                  # only if the user records; never automatic
└── cache/
    └── thumbnails/
```

Linux additionally uses `$XDG_CONFIG_HOME/avatar-studio/` for `settings.json` and
`$XDG_STATE_HOME/avatar-studio/` for logs where the desktop expects those split locations; the
application resolves the three XDG variables once at startup and records the resolved paths in
diagnostics.

Overrides for testing and portable use:

- `--data-dir <path>` on the command line.
- `AVATAR_STUDIO_DATA` environment variable.

The override applies to the whole tree. It is documented, not hidden, because support needs it and
portable installs are a legitimate use.

---

## Managed library

Import is a copy, not a reference. There are two inputs: an existing `.avatar` package, and a bare
`.glb`/`.gltf`, which the importer wraps into a package first
([`avatar-package-format.md`](avatar-package-format.md#from-an-existing-glb)). The pipeline is:

1. Validate the package, or the draft the importer just generated (schema and semantic layers,
   [`avatar-package-format.md`](avatar-package-format.md#validation)).
2. Copy into `library/.incoming-<uuid>/` on the same volume.
3. Re-validate the copy and verify `assets.json` hashes.
4. Rename into `library/<package-id>/`.
5. Update `library.json` and generate a thumbnail if the package has none.

Rules:

- **Duplicate ID.** Prompt *Replace*, *Keep both*, or *Cancel*. *Keep both* assigns a new UUIDv4 to
  the imported copy and rewrites `id` in `avatar.json`; the original package is untouched. Provenance
  in `assets.json` is preserved.
- **Source path.** Stored in the index as `source_path` for "Reveal original". It is never used to
  load the avatar after import. On sandboxed macOS, store a security-scoped bookmark instead of a
  raw path; a raw path may be unreadable after the picker closes.
- **Delete.** Removes the managed directory and its thumbnail. The original file or folder is
  untouched. Confirmation names the avatar.
- **Move.** A user may move the whole data directory while the app is closed; the index stores
  relative paths so the library survives.
- **Disk.** Before import, warn if free space is below 1 GB. A failed copy leaves `.incoming-*`
  removed on next start. A bare-GLB import that is abandoned before calibration is saved is removed
  the same way; the user's original file is never touched.
- **Thumbnails.** Use `thumbnail.png` from the package. If missing, render a 512×512 frame from the
  neutral pose and cache it at `cache/thumbnails/<id>.png`. Regenerate when the package mtime
  changes.
- **No system-wide library.** The library is per OS user. Shared installs are not a 1.0 feature.

`library.json` is a cache, not a contract:

```json
{
  "version": 1,
  "avatars": [
    {
      "id": "0f3a7c1e-5b2d-4f8a-9c11-7d6e2b9a4c30",
      "name": "Avocado",
      "path": "library/0f3a7c1e-5b2d-4f8a-9c11-7d6e2b9a4c30",
      "source_path": "D:/models/Avocado.avatar",
      "model_sha256": "9f2c…",
      "imported": "2026-11-20T00:00:00Z"
    }
  ]
}
```

`model_sha256` is a cache key over `model.glb`, not a provenance check; `assets.json` remains the
provenance record. If the index is missing or corrupt, rebuild it by scanning
`library/*/avatar.json`. A rebuild is logged as `CFG-001` only if the cache existed and could not be
read.

---

## FaceProfile

A `FaceProfile` is the per-person mapping from anatomical landmarks to the canonical face plane. It
is the reason a user calibrates once and then tries every avatar. It lives in `profiles/` and is
never embedded in an avatar package.

### Canonical convention

Defined in [`face-compositing.md`](face-compositing.md#coordinate-spaces), repeated here because the
profile is where it is measured:

- Origin: midpoint between the eye centres in the neutral frame.
- Scale: inter-ocular distance = 1.0.
- `+u`: toward the **subject's right eye**. The u axis runs from the subject's left eye centre to
  the right eye centre; in an unmirrored image it points toward the viewer's left.
- `+v`: toward the forehead.
- Coordinates are computed in **aspect-corrected image units** (x scaled by `width / height`), so a
  change of camera aspect ratio does not distort the template.

The profile stores canonical `(u,v)` per named landmark, not raw pixels and not backend indices.
`LandmarkMap` in [`tracking.md`](tracking.md#landmark-map) resolves names to the active backend.

The named landmark subset and its JSON round-trip are implemented in
`native/tracking/include/avatar/tracking/profile_correspondence.h` (T12). A profile that
names a landmark the active backend cannot provide falls back to the available subset and
raises `RIG-003`; unknown names survive the round-trip and are ignored by the solver.
T31 owns the full profile document, capture and storage, and embeds this `landmarks` array
unchanged.

### Schema

```json
{
  "format": "avatar-studio/face-profile",
  "version": 1,
  "id": "6a1e2f4c-8b3d-4a71-9e55-2c7f0d1a6b84",
  "name": "Default",
  "created": "2026-11-20T00:00:00Z",
  "updated": "2026-11-20T00:00:00Z",
  "camera": {
    "device_id": "\\\\?\\usb#vid_046d&pid_085e…",
    "width": 1280,
    "height": 720
  },
  "neutral": {
    "yaw_deg": 0.4,
    "pitch_deg": -0.8,
    "roll_deg": 0.2,
    "frames_averaged": 15
  },
  "landmarks": [
    { "name": "eye_outer_r", "u": 0.52, "v": 0.02 },
    { "name": "eye_inner_r", "u": 0.21, "v": 0.01 },
    { "name": "eye_inner_l", "u": -0.21, "v": 0.01 },
    { "name": "eye_outer_l", "u": -0.52, "v": 0.02 },
    { "name": "nose_tip", "u": 0.0, "v": -0.38 },
    { "name": "mouth_corner_r", "u": 0.31, "v": -0.82 },
    { "name": "mouth_corner_l", "u": -0.31, "v": -0.82 },
    { "name": "chin", "u": 0.0, "v": -1.34 }
  ],
  "region_centroids": {
    "eye_r": [0.5, 0.0],
    "eye_l": [-0.5, 0.0],
    "mouth": [0.0, -0.82]
  },
  "quality": {
    "inlier_ratio": 0.98,
    "reprojection_rms": 0.0035,
    "landmark_jitter": 0.0012
  }
}
```

The example is abbreviated; a real profile stores every name in the `LandmarkMap` that the backend
provides.

### Capture

Neutral capture is a guided step, not a snapshot:

1. Ask the user to face the camera, keep their eyes open, and close their mouth.
2. Collect 15 frames over ~0.5 s.
3. Reject the capture unless: face confidence ≥ 0.9, `|yaw|`, `|pitch|`, `|roll|` ≤ 5°, eye blink
   coefficient < 0.2, jaw-open coefficient < 0.1, and per-landmark jitter below threshold.
4. Discard per-landmark outliers beyond 2σ, then average.
5. Require at least 8 stable correspondences; the homography needs 4, but fewer than 8 is a warning
   that the capture is poor.
6. Store `quality`; a low-quality profile still loads, but the editor offers recalibration.

If the active backend does not provide blink or jaw coefficients, the gate derives the same signal
from eyelid and jaw landmark geometry. The gate is required; the signal is backend-specific.

If the user already has a profile and recalibrates, the previous file is kept as
`profiles/<id>.json.bak` (one generation) before the atomic replace.

### Lifecycle

| Event | Behaviour |
|---|---|
| First run | Create a profile as part of the first-run flow ([`editor-ux.md`](editor-ux.md#first-run)) |
| Active profile | `settings.json` stores `active_profile_id`; exactly one is active |
| Add a person | Settings → Profiles → *New profile*; each has a name, id, and capture |
| Rename / delete | Rename is metadata only. Delete removes the file and clears the active id if it was active; confirm first |
| Recalibrate | Replaces the active profile atomically, keeping one backup |
| Camera change | Profile remains valid. Camera provenance is informational; aspect changes are handled by aspect-corrected coordinates |
| Appearance change | Beard, glasses, or major lighting changes may raise reprojection error; the editor suggests recalibration, never blocks |
| Corrupt profile | `CFG-005`; fall back to creating a new profile from the current frame and tell the user |
| Missing profile | Same as first run: create one from the current frame, marked provisional |

There is no automatic face recognition and no silent profile switching. Selecting a profile is an
explicit user action; the app never guesses who is in front of the camera.

### Privacy

A `FaceProfile` is a biometric template even though it cannot reconstruct a face image. It is:

- stored only in the user data directory, with user-only permissions;
- never included in logs, diagnostics, crash reports, or avatar packages;
- never exported or imported in 1.0 (a future export must be explicit and warned);
- deleted by Settings → Privacy → *Delete face profiles*, with the same confirmation as other
  destructive actions;
- covered by the data inventory and threat model in
  [`security-privacy.md`](security-privacy.md#data-inventory).

---

## Shows and settings

A **Show** is the machine-specific live configuration. It references an avatar and an outfit; it
never embeds either.

```json
{
  "version": 1,
  "id": "b1d0…",
  "name": "Default",
  "avatar": { "id": "0f3a7c1e-…", "path": "library/0f3a7c1e-…" },
  "outfit": "default",
  "profile_id": "6a1e2f4c-…",
  "camera": { "device_id": "…", "width": 1280, "height": 720, "fps": 60 },
  "tracking": { "smoothing": 0.5, "mirror_preview": true },
  "matte": { "mode": "auto", "key": [0.0, 1.0, 0.0], "similarity": 0.4, "softness": 0.1 },
  "framing": { "fit": "model", "zoom": 1.0, "offset": [0.0, 0.0], "margin": 0.1 },
  "output": {
    "resolution": [1920, 1080],
    "mirror_output": false,
    "transports": ["spout"],
    "virtual_camera": true,
    "background": { "type": "transparent" }
  },
  "quality": "balanced"
}
```

- `settings.json` holds global defaults (active profile, theme, UI scale, diagnostics opt-in, update
  check) and the last active show.
- A Show overrides package defaults for camera, framing, output, quality, and matte. Avatar geometry
  and face calibration never move into a Show.
- `matte` is per machine because it depends on the room: `auto` detects a green or blue screen and
  otherwise uses the software mask; `chroma`, `software` and `off` are explicit overrides. It affects
  the decal alpha only, never the output background
  ([`face-compositing.md`](face-compositing.md#face-matte)).

| Matte field | Meaning |
|---|---|
| `mode` | `auto`, `chroma`, `software`, `off` |
| `key` | sRGB key colour for `chroma`; ignored by `auto` |
| `similarity` | 0–1 chroma tolerance |
| `softness` | 0–1 edge falloff |
- A Show's `background.path` may reference a user file; a package's background must stay inside
  `backgrounds/`. Both use the same decode limits.
- Switching a Show is only allowed while stopped. Outfit changes inside a Show are live.
- Every file carries `version`; migrations run in memory and write only on save.

---

## Migration and backups

- Every persisted JSON object has an integer `version`.
- On load, a lower version is migrated in memory; the original is not rewritten until the user saves.
- Before the first write after an app upgrade, copy the affected tree to
  `backups/<app-version>/`. Keep the last two backups.
- A higher version refuses to load with `CFG-004` and names the app version that wrote it.
- Migrations are pure functions with fixtures, same policy as avatar packages
  ([`avatar-package-format.md`](avatar-package-format.md#versioning-and-migration)).
- Atomic write rule: temporary file in the destination directory, `fsync` where the platform
  supports it, rename over the target.

---

## Permissions and deletion

| Platform | Enforcement |
|---|---|
| Windows | Default per-user ACL on `%LOCALAPPDATA%`; no `Everyone` or `Users` grant added |
| macOS | `0700` directories and `0600` files; app-group container for the extension only |
| Linux | `0700` directories and `0600` files; umask is not trusted |

Settings → Privacy → *Delete all app data* removes the library, profiles, shows, settings, logs, and
cache after an explicit confirmation that names each category. Recordings are separate and are never
deleted implicitly.

Uninstall leaves user data in place by default. The Windows installer offers a *Remove user data*
checkbox; macOS and Linux document the paths instead of deleting them silently.

---

## Open questions

| Question | Resolution path |
|---|---|
| Should the library keep multiple versions of the same avatar ID? | M2; current rule is replace or re-ID, no version history |
| Is a 512×512 thumbnail enough for a library grid on 200% UI scale? | M2; regenerate at 2× if needed |
| Should FaceProfiles be exportable between machines? | Deferred; biometric data, needs an explicit warning and a decision on encryption |
| Does the managed copy handle 100+ avatars without a visible scan delay? | M2; index cache is the mitigation |
