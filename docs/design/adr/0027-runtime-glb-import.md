# ADR-0027: Runtime GLB import builds a rigid bind-pose scene from GLTFState

- **Status:** Accepted
- **Date:** 2026-11-21
- **Supersedes:** —
- **Related:** [avatar-package-format.md](../avatar-package-format.md#runtime-import-t17),
  [architecture.md](../architecture.md#runtime-glb-import-t17),
  [face-compositing.md](../face-compositing.md)

## Context

T17 must load a downloaded `.glb` at runtime, without the editor, and show it in bind pose with
its materials. Real models routinely carry rigs, skins, morph targets and animations that the
avatar pipeline deliberately ignores. The face decal also needs rest model-space positions,
normals and triangle indices, and the model-check panel needs an AABB, node tree, mesh/material
lists, triangle count and candidate head nodes. The package is untrusted input, so a GLB must be
self-contained.

The engine already ships a battle-tested glTF parser. Re-implementing it would duplicate a large
correctness and security surface. Letting the parser generate the runtime scene directly, however,
instantiates `Skeleton3D`, `AnimationPlayer`, skins and blend shapes that the design says are not
part of the avatar.

## Decision

`AvatarModel` uses `GLTFDocument`/`GLTFState` to parse the file, then builds its own rigid scene
from the parsed state:

- Every surface is copied into an `ArrayMesh` with its glTF material; `ARRAY_BONES` and
  `ARRAY_WEIGHTS` are dropped and no blend shapes are attached.
- Node transforms and hierarchy come from `GLTFState.get_nodes()`; no `Skeleton3D`,
  `AnimationPlayer`, skin or morph target is created, so the model always renders in its bind pose.
- Units (`m`/`cm`/`mm`), authored up/forward and a uniform scale are applied as one rigid
  conversion on the model root. The source asset is never edited or re-exported.
- The importer exposes the realised size/facing, AABB, node tree, mesh/material counts, triangle
  count, candidate head nodes, warnings and rest-position/normal/index decal arrays.
- `face_submesh` restricts only the decal data; it never hides the rest of the model.
- A GLB preflight rejects malformed containers, external buffer/image URIs, shader extensions and
  models over 200k triangles with `AVT-102`; a missing file is `AVT-101`.
- Warnings (unknown axes/units, missing `face_submesh`, missing normals, degenerate AABB) never
  block a model that parses.

## Alternatives considered

| Option | Why not |
|---|---|
| `GLTFDocument.generate_scene()` and strip unwanted nodes afterwards | Instantiates skeletons, animation players and skins first; stripping leaves dangling skin paths and is fragile across parser versions. |
| Editor `.import` pipeline | Requires the editor, writes sidecar files, and cannot run from a downloaded package path. |
| Custom GLB parser in the framework-free core | Duplicates the engine parser and its texture/material handling; the data plane does not need a second glTF implementation. |
| Import in GDScript | The import is one-time, but the mesh extraction, transform composition and model-check surface are exactly the C++ adapter's job, and the decal pass must not marshal per frame. |

## Consequences

Positive: bind pose is guaranteed by construction; rigs and animations cannot leak into the
render; the decal pass gets stable rest-space arrays; the model-check surface is one object; the
engine handles the hard glTF parsing and material/texture work.

Negative: skinned models are intentionally rigid, so a T-pose character cannot be posed; morph
targets and animations are unavailable by design. Candidate head nodes are a heuristic and may
need user correction in T32. The importer currently accepts only self-contained GLB, not
`.gltf` plus external buffers; package wrapping and conversion are T30.

Neutral: import is a one-time cost, measured in `docs/progress/T17.md`; the resulting
`ArrayMesh` and arrays are persistent, so no per-frame CPU copy is introduced.

## Follow-ups

- T19: use the node tree/candidate heads for head-region rotation; verify the rest-space arrays.
- T21: consume the decal arrays and confirm the no-per-frame-copy rule under the 10-minute soak.
- T29/T30: wrap bare GLBs into managed packages and enforce the same preflight at package load.
- T32: surface the model-check fields and let the user correct units/axes/scale.
- T43: extend the corpus and revisit the candidate-head heuristic against real downloaded models.
