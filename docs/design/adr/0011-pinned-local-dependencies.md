# ADR-0011: Pinned local dependencies and one verification entry point

- **Status:** Accepted
- **Date:** 2026-11-20
- **Supersedes:** —
- **Related:** [`../architecture.md`](../architecture.md), [`../testing-and-quality.md`](../testing-and-quality.md)

## Context

The native core must configure, build and test from a clean checkout on Windows, macOS and Linux
without Godot. Later tasks add dependencies (nlohmann/json now; Eigen, godot-cpp and MediaPipe
later). Two failure modes are unacceptable: a build that fetches arbitrary code at configure time,
and a build that only works on a machine where someone installed the right version by hand.

Contributors also need one command that proves a change did not break the build. Without it, each
task invents its own partial check and green means something different every time.

## Decision

All third-party native dependencies are pinned by version and SHA256 in `scripts/bootstrap.ps1` and
`scripts/bootstrap.sh`. Bootstrap installs them into `.tools/` (gitignored), verifies every
download, and is idempotent. CMake consumes the local copy, fails configuration when a pinned
dependency is missing, and never downloads anything at configure or build time.

`scripts/verify.ps1` and `scripts/verify.sh` are the single verification contract. `-Fast`
bootstraps, configures, builds RelWithDebInfo and runs CTest label `fast`. `-Full` adds the complete
native suite and `tests/gates/TNN.ps1`. `-Task TNN` additionally selects CTest label `task_TNN` and
runs that task's gate when present. Every command is printed and the script exits non-zero on any
failure.

Warnings are errors by default: MSVC `/W4 /WX`, GCC/Clang `-Wall -Wextra -Wpedantic -Werror`,
controllable with `AVATAR_WARNINGS_AS_ERRORS`.

## Alternatives considered

| Option | Why not |
|---|---|
| CMake `FetchContent` / `ExternalProject` | Network access during configure/build, unpinned by default, and a missing dependency looks like a transient error instead of a setup error. |
| vcpkg or Conan | A second package manager and manifest to maintain; the dependency set is small and pinned. Can be adopted later without changing the `.tools/` contract. |
| Vendor headers into the repository | Bloat, unclear provenance, and licence auditing becomes implicit. |
| System-installed dependencies | Not reproducible across developer machines and CI. |
| A separate CI-only build script | The local and CI paths drift; the contract must be runnable by every contributor. |

## Consequences

**Positive**

- Clean checkout → one command → verified build, on every supported platform.
- Offline builds after bootstrap; dependency provenance is explicit and auditable.
- Later tasks inherit the same gate instead of adding ad-hoc checks.

**Negative**

- Bootstrap needs network access once per clean tree; CI must cache or re-run it.
- Pin data lives in both bootstrap scripts; they must be updated together until a shared manifest
  exists.
- Gates are PowerShell scripts, so macOS/Linux gate runs need `pwsh`.

**Neutral**

- `.tools/` is disposable local state; deleting it is always safe.

## Follow-ups

1. Move the pin table to one shared manifest consumed by both bootstrap scripts and CMake.
2. Cache `.tools/` in CI keyed on the pin table.
3. Extend the licence audit to read the same manifest.
4. From T02, make a missing Godot a verification failure for tasks that touch the adapter.
