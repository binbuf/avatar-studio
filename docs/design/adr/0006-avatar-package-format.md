# ADR-0006: Directory-based `.avatar` package with versioned declarative JSON

- **Status:** Proposed
- **Date:** 2026-11-20
- **Related:** [`../avatar-package-format.md`](../avatar-package-format.md)

## Context

Users need to move an avatar between machines, share it, and keep it working across app versions.
The configuration is small (a few hundred numbers) but must describe geometry placement, region
shapes, prop attachments, colour tuning and output defaults.

The format will be edited by hand by some users, diffed in version control by others, and generated
by the app for everyone. It will also be read from untrusted sources.

## Decision

An `.avatar` is a **directory** (optionally zipped as `.avatarz`) containing:

- `model.glb` — the character,
- `avatar.json` — a versioned, schema-validated, declarative configuration,
- optional `face-calibration.json` for bulky tables,
- `props/`, `outfits/`, `thumbnail.png`, `LICENSE.txt`, and a required `assets.json` provenance
  manifest for anything distributed.

Rules that follow from this:

- JSON only; no custom binary, no scripts, no embedded shaders.
- Integer `version`; forward-incompatible packages are refused with a clear error; migrations run in
  memory and never rewrite the file without the user saving.
- Unknown fields in recognised objects are preserved on save; unknown behaviour-changing fields are
  rejected.
- Three configuration levels are kept separate: **Avatar** (geometry + calibration), **Outfit**
  (props + appearance), **Show** (machine-specific: camera, output, tracking). A **FaceProfile**
  (per person) lives outside the package entirely.

## Alternatives considered

| Option | Why not |
|---|---|
| **Single JSON with everything embedded (base64 model)** | Unreviewable diffs, huge files, and it defeats "open the folder and look". |
| **SQLite** | Great for indexes, terrible for hand-editing, diffing, and trust. |
| **Binary format (MessagePack/CBOR + schema)** | Smaller and faster, but opaque to users and to `git diff`. The data is tiny; size is not a problem to solve. |
| **YAML** | Friendlier to type, but indentation-sensitive and a security liability (arbitrary object construction in many parsers). |
| **glTF extensions only** (`KHR_*` in the model) | Elegant, but couples our calibration to the model file, breaks "swap the model, keep the calibration", and requires a glTF extension ecosystem that does not exist. |
| **Embed FaceProfile in the avatar** | Prevents sharing an avatar between users without leaking one user's face template, and prevents one calibration from serving all avatars. |
| **One flat "scene" file** | The single-level model is what makes avatar tools unusable: users cannot change outfit without duplicating the avatar. |

## Consequences

**Positive**

- Human-readable, diff-friendly, reviewable.
- Trivially validated and fuzzed.
- Cheap to version and migrate; a fixture per historical version makes migration testable.
- The Avatar/Outfit/Show split enables instant outfit switching while live.

**Negative**

- A directory is slightly awkward to share (hence the optional zip).
- JSON has no comments; documentation must live in the app and in this doc set.
- Preserving unknown fields requires a parser that round-trips rather than a strict struct mapping.

**Neutral**

- The format is ours; there is no interoperability story in 1.0 beyond importing `.glb`.

## Follow-ups

1. Publish the JSON Schema alongside the app and validate packages in CI.
2. Build the migration harness with fixtures for v1 before v2 exists.
3. Fuzz both the JSON loader and the glTF importer (nightly).
4. Revisit single-file vs directory after M2 usage data (Q in
   [`../avatar-package-format.md`](../avatar-package-format.md#open-questions)).
