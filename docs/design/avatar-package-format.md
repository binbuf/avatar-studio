# Avatar Package Format

**Status:** proposal · **Last updated:** 2026-11-20
**Decision record:** [ADR-0006](adr/0006-avatar-package-format.md)

An `.avatar` package is a directory (optionally zipped as `.avatarz`) containing a glTF model, a
declarative JSON description of how the live face and props attach to it, and provenance metadata.
It is a data format, not a 3D modelling standard, and it is deliberately boring: no scripts, no
custom binary blobs, no executable content.

---

## Three levels of configuration

Mixing these together is the usual reason avatar tools become unusable. They are kept separate:

| Level | Contains | Lives | Shared between users? |
|---|---|---|---|
| **Avatar** | Model, face surface calibration, region shapes, anchors, available props | Inside the `.avatar` package | Yes — this is what people download |
| **Outfit** | Which props are enabled, material overrides, colour tuning | Inside the package (`outfits/`) or in user config | Yes, optionally |
| **Show** | Avatar + outfit + camera + output + tracking tuning | User config directory | No — machine-specific |

A `Show` is what "go live" actually starts. It references an avatar by id and path, never embeds it.

A fourth, separate concept:

| **FaceProfile** | Landmark → canonical `(u,v)` mapping for a *person* | User config directory | Per user, reusable across avatars |
| --- | --- | --- | --- |

Keeping `FaceProfile` out of the package is what lets one user calibrate once and try every avatar,
and lets an avatar be authored without knowing who will wear it.

---

## Layout

```
Avocado.avatar/
├── avatar.json            # required; the whole configuration
├── model.glb              # required; the character
├── face-calibration.json  # optional; large landmark tables, kept out of avatar.json
├── assets.json            # required for distribution; provenance + hashes
├── LICENSE.txt            # required for distribution; the model's licence text
├── thumbnail.png          # required for distribution; 512×512
├── props/
│   ├── cowboy_hat.glb
│   └── sunglasses.glb
├── outfits/
│   └── default.json
└── preview/
    └── turntable.webp     # optional; store presentation
```

Rules:

- Paths are relative, POSIX-style, and must not escape the package root (`..`, absolute paths and
  symlinks are rejected on load).
- Only `.glb` is accepted for models and props in v1. `.gltf` + external buffers is rejected with a
  clear message; converting is a one-click "embed resources" action in the importer.
- Every file referenced by JSON must exist. Missing files are a validation error, never a silent
  fallback.

---

## `avatar.json`

```json
{
  "format": "avatar-studio/avatar",
  "version": 1,
  "id": "0f3a7c1e-5b2d-4f8a-9c11-7d6e2b9a4c30",
  "name": "Avocado",
  "author": "Example Author",
  "license": "CC0-1.0",
  "model": {
    "path": "model.glb",
    "units": "m",
    "forward": "-Z",
    "up": "Y",
    "pivot": [0.0, 0.0, 0.0],
    "scale": 1.0,
    "face_submesh": null
  },
  "face": {
    "mode": "surface",
    "surface": {
      "type": "plane",
      "origin": [0.0, 0.12, 0.155],
      "normal": [0.0, 0.0, 1.0],
      "up": [0.0, 1.0, 0.0],
      "scale": 0.09
    },
    "regions": [
      {
        "id": "eye_l",
        "type": "eye",
        "center": [-0.5, 0.0],
        "size": [0.46, 0.34],
        "rotation_deg": 0.0,
        "shape": "ellipse",
        "feather": 0.18,
        "opacity": 1.0,
        "landmark_anchor": "eye_l_centroid",
        "color": { "exposure": 0.0, "temperature": 0.0, "saturation": 1.0, "contrast": 1.0 }
      },
      {
        "id": "eye_r",
        "type": "eye",
        "center": [0.5, 0.0],
        "size": [0.46, 0.34],
        "rotation_deg": 0.0,
        "shape": "ellipse",
        "feather": 0.18,
        "opacity": 1.0,
        "landmark_anchor": "eye_r_centroid",
        "color": { "exposure": 0.0, "temperature": 0.0, "saturation": 1.0, "contrast": 1.0 }
      },
      {
        "id": "mouth",
        "type": "mouth",
        "center": [0.0, -0.82],
        "size": [0.62, 0.46],
        "rotation_deg": 0.0,
        "shape": "rounded_rect",
        "feather": 0.22,
        "opacity": 1.0,
        "landmark_anchor": "mouth_centroid",
        "color": { "exposure": 0.0, "temperature": 0.0, "saturation": 1.0, "contrast": 1.0 }
      }
    ],
    "lighting_influence": 0.35
  },
  "anchors": [
    { "id": "head_top", "position": [0.0, 0.34, 0.0], "rotation": [0, 0, 0, 1], "scale": 1.0 },
    { "id": "eyes",     "position": [0.0, 0.12, 0.16], "rotation": [0, 0, 0, 1], "scale": 1.0 },
    { "id": "neck",     "position": [0.0, -0.18, 0.0], "rotation": [0, 0, 0, 1], "scale": 1.0 }
  ],
  "props": [
    {
      "id": "cowboy_hat",
      "asset": "props/cowboy_hat.glb",
      "anchor": "head_top",
      "offset": { "position": [0.0, 0.02, 0.0], "rotation": [0, 0, 0, 1], "scale": 1.0 },
      "default_enabled": false
    }
  ],
  "appearance": {
    "material_overrides": [],
    "environment": "studio_soft"
  },
  "output": {
    "resolution": [1920, 1080],
    "background": { "type": "transparent" }
  },
  "meta": {
    "created": "2026-11-20T00:00:00Z",
    "generator": "avatar-studio/0.1"
  }
}
```

### Field reference

**`model`**

| Field | Type | Meaning |
|---|---|---|
| `path` | string | Package-relative `.glb` |
| `units` | `"m"` \| `"cm"` \| `"mm"` | Units the model was authored in; converted to metres on load |
| `forward` / `up` | string | Authored axes; the importer rotates to −Z/Y |
| `pivot` | `[x,y,z]` | Rotation origin in model space. Used directly as the head rotation centre |
| `scale` | number | Uniform scale applied after unit conversion |
| `face_submesh` | string \| null | Optional mesh name; restricts the decal pass to it |

**`face`**

| Field | Type | Meaning |
|---|---|---|
| `mode` | `"flat"` \| `"surface"` \| `"shaded"` | Which geometry carries the decal; see [`face-compositing.md`](face-compositing.md#projection-modes). Default `surface` |
| `surface` | object | Head space → canonical face plane mapping (below) |
| `regions` | array | Eye and mouth regions (below) |
| `lighting_influence` | 0–1 | Fraction of the scene key light applied to the decal in `shaded` mode; default 0.35 |

**`face.surface`**

| Field | Meaning |
|---|---|
| `type` | `"plane"` or `"cylinder"` — how head-space points map to canonical `(u,v)` |
| `origin` | Centre of the face plane in model space (metres) |
| `normal` | Face outward direction; the decal samples along it |
| `up` | In-plane up direction; combined with `normal` to build the basis |
| `scale` | Metres per canonical unit (i.e. the model's inter-ocular distance) |

For `cylinder`, `origin` is a point on the axis and `normal` is the axis direction; `scale` is the
circumference-to-canonical ratio. The calibration gizmo sets all of these; users do not type them.

**`face.regions[]`**

| Field | Meaning |
|---|---|
| `id` | `eye_l`, `eye_r`, `mouth` (stable; extensions may add more) |
| `type` | `eye` \| `mouth` \| `generic` |
| `center`, `size` | Canonical-space rectangle, inter-ocular units. `size` is full width/height; the shader halves it for the mask |
| `rotation_deg` | In-plane rotation |
| `shape` | `ellipse` \| `rounded_rect` |
| `feather` | Fraction of the half-size that fades (0 = hard edge) |
| `opacity` | 0–1 multiplier |
| `landmark_anchor` | Which landmark centroid drives the slow local offset: `eye_l_centroid`, `eye_r_centroid` or `mouth_centroid` |
| `color` | Per-region colour transform (see [`face-compositing.md`](face-compositing.md#colour-and-light-matching)) |

**`anchors[]`** — named attach points. `position`/`rotation`/`scale` are in model space. Prop
attachment is `anchor * offset`. Standard names, in priority order for auto-placement:

`head_top`, `head_back`, `eyes`, `ears_l`, `ears_r`, `neck`, `chest`, `hand_l`, `hand_r`.

Unknown anchor names are allowed (for custom rigs) but props attached to them are not auto-fitted.

**`props[]`** — `asset` must be inside `props/`. `default_enabled` is the state used when the
avatar is first loaded; outfits override it.

**`appearance`** — `environment` names a lighting environment shipped with the app (for example
`studio_soft`). `material_overrides` is an array of per-material overrides applied at load; its
entry schema is **not yet specified** and is finalised with the appearance system in M2.

**`output`** — `resolution` is the default `[width, height]`. `background` is specified only as
`{"type": "transparent"}` in the example above; the opaque background variants required by
virtual-camera and window output are **not yet enumerated** and must be added with the output work.

**`meta`** — authoring metadata (`created`, `generator`). Informational only; never interpreted as
behaviour.

---

## `face-calibration.json`

Large tables are split out so that `avatar.json` stays human-readable:

```json
{
  "version": 1,
  "surface_samples": [
    { "u": -0.5, "v": 0.0, "head": [-0.045, 0.121, 0.163] }
  ],
  "notes": "Optional: per-landmark adjustments captured during calibration."
}
```

`surface_samples` lets the importer fit a cylinder (or a low-order surface) from a handful of
user-placed points instead of asking for an axis and a radius. It is optional; a plane needs none.

---

## `assets.json` (provenance)

```json
{
  "version": 1,
  "assets": [
    {
      "path": "model.glb",
      "author": "Example Author",
      "source": "https://example.org/avocado",
      "license": "CC0-1.0",
      "sha256": "9f2c…",
      "modified": false
    }
  ]
}
```

Required for anything distributed with or through the app. The app refuses to add a package to the
bundled library without it, and warns (but allows) when a user imports one without it. Hash
verification runs on load when `sha256` is present.

---

## Outfits and shows

`outfits/default.json`:

```json
{
  "version": 1,
  "name": "Default",
  "props": [
    { "id": "cowboy_hat", "enabled": true,
      "offset": { "position": [0.0, 0.02, 0.0], "rotation": [0, 0, 0, 1], "scale": 1.0 } }
  ],
  "appearance": { "environment": "studio_soft" },
  "face": { "lighting_influence": 0.35 }
}
```

A `Show` (user config, not in the package):

```json
{
  "version": 1,
  "avatar": { "id": "0f3a7c1e-…", "path": "…/Avocado.avatar" },
  "outfit": "default",
  "camera": { "device_id": "…", "width": 1280, "height": 720, "fps": 60 },
  "tracking": { "smoothing": 0.5, "mirror_preview": true },
  "output": { "transports": ["spout"], "virtual_camera": true }
}
```

---

## Validation

Two layers, both run on load and before save:

**Schema layer** (JSON Schema 2020-12, shipped with the app): types, required fields, enums, ranges,
unknown-field policy.

**Semantic layer:**

| Check | Failure |
|---|---|
| Referenced files exist and stay inside the package | `AVT-101` |
| `model.glb` parses, has ≥ 1 mesh, ≤ 200k triangles | `AVT-102` |
| `face.surface.scale > 0`; `normal` and `up` are non-parallel unit vectors | `AVT-103` |
| Regions do not extend beyond `u ∈ [-3, 3]`, `v ∈ [-4, 3]` | `AVT-104` |
| Region ids unique; `eye_l`/`eye_r`/`mouth` present | `AVT-105` |
| Every `props[].anchor` exists | `AVT-106` |
| Every prop asset parses and is inside `props/` | `AVT-107` |
| `assets.json` covers every distributed binary file | `AVT-108` (warning) |
| Package total ≤ 200 MB, single glb ≤ 100 MB | `AVT-109` |

Errors are reported with the JSON path (`face.regions[1].size`) and a suggestion, because the
alternative is a support ticket.

---

## Versioning and migration

- `version` is an integer, incremented only for breaking changes.
- On load: if `version < current`, run migrations in memory, log the chain, and keep the original
  file untouched until the user saves.
- On load: if `version > current`, refuse with `AVT-120` and name the app version that wrote it.
- Migrations are pure functions with unit tests over fixture packages (one per historical version).
- Unknown fields inside a recognised object are preserved verbatim on save (forward compatibility
  for additive fields); unknown fields that would change behaviour are rejected.
- Extensions use a namespaced key (`x-vendor-feature`) and are ignored if unrecognised.

Migration policy matters more than it looks: a package format that silently loses a user's
calibration is worse than one that refuses to open.

---

## Import safety

A package is untrusted input. The importer enforces:

- No external URIs in glTF (no `http(s)://`, no `data:` buffers above 1 MB).
- No symlinks, no `..`, no absolute paths, no NTFS alternate data streams.
- No embedded scripts or HTML in metadata fields; strings are length-capped and rendered as text.
- Texture dimensions capped (8k) and decoded with limits to avoid decompression bombs.
- glTF extensions: allow-listed. Unknown extensions are ignored with a warning, not passed to the
  loader.

Details and the threat model: [`security-privacy.md`](security-privacy.md#untrusted-avatar-packages).

---

## Interoperability

| Direction | Support |
|---|---|
| Import `.glb` from Blender/any DCC | Yes (unit and axis conversion on import) |
| Import another app's avatar format | Not planned; `.vrm` is the only serious candidate and is `Deferred` |
| Export to another app's format | Not planned |
| Round-trip through version control | Yes — JSON is stable, ordered, and diff-friendly (2-space indent, LF, sorted keys on save) |

---

## Open questions

| Question | Resolution path |
|---|---|
| Is a directory package acceptable to users, or is a single-file archive required for sharing? | Both supported; measure which users pick in M2 |
| Should outfits be embedded in the package or always user-level? | Ship embedded defaults + user overrides; revisit after M2 |
| Do authors need a "face mask texture" escape hatch instead of parametric regions? | If M2 calibration proves too coarse for organic shapes |
| VRM import: worth it? | Deferred; only if avatar-author demand is demonstrated |
