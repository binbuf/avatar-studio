# Avatar Studio

A performant mixed reality avatar platform

## Getting Started

TODO

## Development

The native core is framework-free C++20 and does not require Godot. The Godot shell and GDExtension
adapter are built when the pinned engine and godot-cpp are bootstrapped into `.tools/`; everything
below works from a clean checkout.

Prerequisites:

- CMake 3.25 or newer.
- A C++20 compiler: Visual Studio 2022/2026 (MSVC) on Windows, GCC 12+ or Clang 15+ on
  macOS/Linux.
- Python 3.8+ (godot-cpp generates its bindings at CMake configure time).
- PowerShell 5.1+ on Windows, bash and `curl` on macOS/Linux.
- PowerShell 7 (`pwsh`) on macOS/Linux only if you run a task gate script (`tests/gates/TNN.ps1`).

Third-party dependencies are pinned by version and SHA256 and are installed into `.tools/`
(gitignored): nlohmann/json 3.11.3, Godot 4.7.2 stable, and godot-cpp 10.0.0-stable with
`GODOTCPP_API_VERSION=4.7`. Bootstrap is idempotent, verifies every download, and prints the engine
and bindings paths. CMake only consumes the local copy and never downloads anything.

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

### Godot shell

`bootstrap -Fast` installs the pinned engine and bindings and prints their paths:

```text
bootstrap: godot      <repo>\.tools\godot\Godot_v4.7.2-stable_win64_console.exe
bootstrap: godot-cpp  <repo>\.tools\godot-cpp
```

The Godot project is `app/project.godot`; it uses Forward+, a fixed 1920x1080 transparent
`SubViewport`, and the `AvatarRuntime` GDExtension class. Build and test it with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify.ps1 -Fast -Task T02
```

That command builds `avatar_gdextension`, copies it to `app/bin/`, and runs `app/tests/smoke.gd`
headlessly. `-Task T01` uses the native-only bootstrap and does not require Godot. To open the
project interactively after bootstrap:

```powershell
.\.tools\godot\Godot_v4.7.2-stable_win64.exe --path app --editor
```
