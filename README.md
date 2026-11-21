# Avatar Studio

A performant mixed reality avatar platform

## Getting Started

TODO

## Development

The native core is framework-free C++20 and does not require Godot. Godot and the
GDExtension adapter arrive with later tasks; everything below works from a clean checkout.

Prerequisites:

- CMake 3.25 or newer.
- A C++20 compiler: Visual Studio 2022/2026 (MSVC) on Windows, GCC 12+ or Clang 15+ on
  macOS/Linux.
- PowerShell 5.1+ on Windows, bash and `curl` on macOS/Linux.
- PowerShell 7 (`pwsh`) on macOS/Linux only if you run a task gate script (`tests/gates/TNN.ps1`).

Third-party dependencies are pinned by version and SHA256 and are installed into `.tools/`
(gitignored). Bootstrap is idempotent and verifies every download. CMake only consumes the local
copy and never downloads anything.

```powershell
# Windows
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/bootstrap.ps1
cmake -S . -B build
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo -L fast --output-on-failure
```

```bash
# macOS / Linux
./scripts/bootstrap.sh
cmake -S . -B build
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo -L fast --output-on-failure
```

One command runs the same steps and is the contract for every task:

```powershell
# Fast: bootstrap, configure, build RelWithDebInfo, CTest label "fast"
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify.ps1 -Fast -Task T01

# Full: fast, the complete native CTest suite, and the task gate tests/gates/T01.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify.ps1 -Full -Task T01
```

```bash
./scripts/verify.sh --fast --task T01
./scripts/verify.sh --full --task T01
```

`-Fast` is the default when no mode is given. `-Task TNN` also matches CTest label `task_TNN` and
runs `tests/gates/TNN.ps1` when that gate exists. Every step prints the exact command it runs and
the script exits non-zero on the first failure. Warnings are errors by default
(`AVATAR_WARNINGS_AS_ERRORS=ON`); pass `-DAVATAR_WARNINGS_AS_ERRORS=OFF` to configure a
non-gating build, and `-DAVATAR_TESTS=OFF` to skip the test target.
