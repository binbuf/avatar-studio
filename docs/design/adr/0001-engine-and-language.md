# ADR-0001: Godot 4.7 + C++20 GDExtension as the application platform

- **Status:** Proposed
- **Date:** 2026-11-20
- **Related:** [`../architecture.md`](../architecture.md)

## Context

The application needs a real-time 3D renderer, runtime glTF loading, a UI toolkit, cross-platform
window management and GPU shader support, on Windows, macOS and Linux. The team is small. The
differentiator is the face compositor and the editor workflow, not the renderer.

Constraints:

- Permissive licensing throughout, including for commercial users of the app.
- 60 fps at 1080p with a transparent viewport and a second geometry pass.
- Native access to camera APIs and OS virtual-camera APIs.
- No web runtime in the hot path: frame pacing and latency are product requirements.
- Long-lived codebase; the platform choice must still be defensible in three years.

## Decision

Build a native desktop application on **Godot 4.7.2** (MIT), with the data plane in **C++20** exposed
through a **godot-cpp GDExtension**. Godot owns rendering, the scene graph, windowing and the UI
toolkit. Our C++ owns capture, tracking, rig math, package handling and output. GDScript is used only
for presentation-level UI code.

The native core is a framework-free static library; the GDExtension layer is an adapter and contains
no business logic. This keeps the core testable headlessly and portable if the engine is ever
replaced.

## Alternatives considered

| Option | Why not |
|---|---|
| **Tauri + Three.js + MediaPipe (web)** | Fastest prototype, but WebView rendering variance, `requestVideoFrameCallback` timing imprecision, and a webcam→canvas→texture path that fights the browser are all directly against the latency requirement. Transparent output to OBS becomes a browser-capture problem. |
| **Rust + wgpu + Qt Quick / egui** | Best control and best licensing story, but it means building a scene graph, an asset importer, an editor viewport and a shader hot-reload workflow before the first avatar appears. Months of undifferentiated work. |
| **Unity** | Licensing and runtime terms are unsuitable for a permissively licensed, commercially unrestricted application; also a large binary and a heavy install. |
| **Unreal** | Same licensing concerns, far heavier, and the renderer's complexity buys nothing here. |
| **Electron + three.js** | Worse than Tauri on all axes relevant here. |
| **Custom engine (Vulkan/Metal/D3D12 + ImGui)** | Maximum performance and control, but reimplements what Godot already provides, and the product risk is in the avatar editor, not the renderer. |
| **Godot 4.8 beta** | Pre-release; the app depends on stable GDExtension ABI and stable rendering behaviour. |

## Consequences

**Positive**

- Rendering, shaders, glTF import, UI and windowing are solved on day one.
- MIT license is compatible with the product's licensing goals.
- C++ core keeps the hot path fast and the OS integrations natural.
- Large ecosystem for debugging and for contributors to learn.

**Negative**

- The Compatibility (OpenGL) renderer cannot be supported, because GPU readback and RD resource
  access are required. This is a real user-facing limitation on old hardware.
- GDExtension is a per-call boundary; discipline is required to keep the data plane out of it.
- Engine upgrades are a coupled event: engine, godot-cpp, and GDExtension ABI move together.
- The Godot editor is not the product, but contributors will expect it to be; documentation must
  make the distinction clear.

**Neutral**

- Godot's 3D renderer is not the best in class at high-end visuals; for a stylised avatar at 1080p
  it is comfortably sufficient.

## Follow-ups

1. Pin 4.7.2 in CI images and in `godot-cpp`; record the exact commit.
2. Define the 4.8 upgrade gate: only after 4.8 stable plus one maintenance release, and only with a
   full golden-image and performance run.
3. Verify in M1 that `SubViewport` + `RenderingDevice` readback behaves as expected on all three
   backends (Vulkan, D3D12, Metal).
4. Confirm that a GDExtension can be shipped inside a signed, notarised macOS bundle alongside a
   system extension (M4).

## Amendment (2026-11-20, T02)

godot-cpp moved to independent versioning at v10 and no longer publishes a `godot-4.7.2-stable`
tag. The engine pin is unchanged: **Godot 4.7.2 stable**. The bindings pin is
**godot-cpp 10.0.0-stable** built with `GODOTCPP_API_VERSION=4.7`; the API version, not the
repository version number, is the compatibility contract. The adapter is built with godot-cpp's
CMake support. See [ADR-0012](0012-godot-shell-bootstrap.md) for the bootstrap, verification and
headless-import details. The 4.8 upgrade gate in Follow-up 2 still applies.
