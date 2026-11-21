# ADR-0029: Shared CPU vertex buffers for head deformation

- **Status:** Accepted
- **Date:** 2026-11-21
- **Related:** [ADR-0009](0009-head-region-rotation.md), [face compositing](../face-compositing.md)

## Context

T19 needs identical model/decal geometry while retaining T17's imported glTF materials.
The target is **< 0.15 ms for 20k triangles**. A custom vertex shader would require replacing
or reproducing those materials; Godot's public StandardMaterial3D API has no vertex injection
hook. We measured the material-preserving CPU path, including normal packing and buffer submission,
rather than reporting quaternion math alone as the deformation cost.

## Decision

Keep ADR-0009's scope and use CPU streaming for the vertical slice. Precompute rest-space cut
weights at configuration, retain uncompressed dynamic ArrayMesh vertex buffers, then update positions,
octahedral normals and tangents in place. Vertices with zero weight are untouched. No per-frame mesh
rebuild or application-owned collection resizing occurs; Godot may copy the retained PackedByteArray
when a pending render command still owns it. Measured submission includes that cost. Node mode changes
only the selected subtree transform about its authored origin.

Both passes reference the **same ArrayMesh**, not independent implementations. CUSTOM0 stores immutable
rest model position; `head_decal.gdshaderinc` declares its varying and the decal vertex function assigns
CUSTOM0.xyz to it. Fragment projection consumes the varying. Calibrated face projection remains T20/T21.

The indexed 20k-triangle / 10,201-vertex neck grid measured **0.463 ms median / 0.541 ms p95**
for cut deformation on a Ryzen 9 7900X3D, Windows 11, MSVC RelWithDebInfo. This is an explicit
**budget overrun**, not a claim to meet 0.15 ms. `tests/gates/T19.ps1` records fresh CPU+submission
measurements and actual OpenGL rendered goldens; `.tools/T19-head-region-render.json` is local evidence.
The 20k-triangle node fixture measured 0.009 ms median / 0.012 ms p95. The 0.15 ms target remains open: future material-preserving shader
work or CPU optimisation must measure the same operation before replacing this path. More split
vertices and larger meshes increase CPU and upload cost; this fixture is not a universal bound.

## Consequences

The slice preserves material fidelity and guarantees attached decals without duplicate deformation.
CPU copies and uploads exceed the target; use this measured path for M1, retain the performance debt
for corpus/performance work (T43/T64), and do not silently relax the target. Synthetic seam tests and
12 rendered references cover the design angles; real-model neck quality remains T43. The importer
uses a provisional AABB face until calibrated face data is supplied through `configure_head_region`.
