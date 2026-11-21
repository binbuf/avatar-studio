# ADR-0012: Godot shell bootstrap, godot-cpp 10 pinning, and CMake build

- **Status:** Accepted
- **Date:** 2026-11-20
- **Supersedes:** —
- **Related:** [`0001-engine-and-language.md`](0001-engine-and-language.md), [`0011-pinned-local-dependencies.md`](0011-pinned-local-dependencies.md), [`../architecture.md`](../architecture.md)

## Context

ADR-0001 pins Godot 4.7.2 and assumes godot-cpp carries a matching `4.7.2` tag. That is no longer how
upstream ships: godot-cpp moved to independent versioning at v10 and targets Godot 4.3+ through the
`GODOTCPP_API_VERSION` option. The current stable is godot-cpp `10.0.0-stable` (2026-09-15), which
ships `extension_api-4-7.json` and a CMake build. There is no `godot-4.7.2-stable` godot-cpp tag.

T02 also needs a shell that verifies without a GPU: Godot's headless mode must load the GDExtension,
open the transparent viewport, and run the smoke script on a machine with no display.

## Decision

Pin Godot **4.7.2 stable** and godot-cpp **10.0.0-stable** in both bootstrap scripts, with exact URLs
and SHA256. Bootstrap unpacks them to `.tools/godot/` and `.tools/godot-cpp/` and prints both paths.
godot-cpp is built with `GODOTCPP_API_VERSION=4.7` and `GODOTCPP_TARGET=template_release`.

The GDExtension is a CMake target (`native/gdextension/CMakeLists.txt`) added by the root project
only when `.tools/godot-cpp` exists. It links `godot::cpp` as a static library and copies the shared
library to `app/bin/avatar_studio.<os>.<arch>.<ext>`. godot-cpp include directories are marked as
system headers so the project warning policy does not inherit third-party warnings.

One RelWithDebInfo artifact is referenced by both the debug and release entries in
`app/bin/avatar_studio.gdextension`. A debug-specific build can add a suffixed artifact later; the
GDExtension ABI is the same.

The verification contract requires Godot and the built adapter for `-Task T02` and later. `-Task T01`
bootstraps with `--native-only` and skips the engine, so the framework-free core remains testable
without Godot.

Godot 4.7.2's headless editor import writes `.godot/extension_list.cfg` and then exits with an
access violation on shutdown. `verify` seeds that one-line file directly before running the smoke
test; the smoke test, not the editor import, is the authoritative load check. This workaround is
removed once a Godot patch release imports cleanly headlessly.

## Alternatives considered

| Option | Why not |
|---|---|
| Wait for a godot-cpp `4.7.2` tag | Does not exist under the new versioning scheme; blocking the shell on a tag upstream will not create is not a plan. |
| Pin godot-cpp 4.5 (the last Godot-versioned tag) | Generated bindings would target 4.5, not 4.7; this silently weakens the ABI contract ADR-0001 exists to protect. |
| Build godot-cpp with SCons | A second build system and a Python/SCons dependency for no benefit; upstream ships CMake and the rest of the project is CMake. |
| Require the Godot editor for the smoke test | The editor is not the product, needs a display for some paths, and its headless shutdown crashes in 4.7.2. |
| Substitute Godot 4.8 beta or an older engine | Explicitly forbidden by T02 and ADR-0001; it would invalidate the pinned ABI and rendering behaviour. |

## Consequences

**Positive**

- Clean checkout plus `bootstrap -Fast` produces a reproducible engine, bindings and shell.
- The headless smoke test runs without a GPU or display.
- Native-only work does not regress when the engine is unavailable.

**Negative**

- godot-cpp version pins are no longer visually aligned with the engine version; the API version is
  the real compatibility contract and must be checked on every upgrade.
- The extension-list seeding is a documented workaround for a 4.7.2 crash.
- A single artifact serves debug and release until a debug build is needed.

**Neutral**

- `.tools/` grows by roughly 260 MB for the engine and bindings; it stays gitignored and disposable.

## Follow-ups

1. Re-test `--headless --import` on each Godot patch release and delete the extension-list seeding
   when it no longer crashes.
2. Add a debug-suffixed artifact and a real debug configure once the adapter has debug-only checks.
3. Move the engine and godot-cpp pins into the shared manifest planned by ADR-0011.
4. Verify the same CMake target on macOS (Metal) and Linux (Vulkan) in T06/T52/T55.
