#!/usr/bin/env bash
# The verification contract (macOS / Linux).
#
#   --fast : bootstrap, configure, build RelWithDebInfo, run CTest label "fast",
#            then run the headless Godot shell smoke test when the pinned engine
#            and app/project.godot are present.
#   --full : --fast plus the complete native CTest suite plus tests/gates/<task>.ps1.
#
# With --task T01 the bootstrap runs with --native-only so the framework-free
# core still verifies without Godot. With --task T02 or later the script requires
# the pinned Godot 4.7.2 binary, the GDExtension artifact, and a passing
# app/tests/smoke.gd; it fails rather than silently skipping them. Every command
# is printed before it runs. The script exits non-zero if any step fails.
#
# Usage: ./scripts/verify.sh [--fast|--full] [--task TNN] [--build-dir DIR] [--config CFG]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mode=""
task=""
build_dir="build"
config="RelWithDebInfo"

while [[ $# -gt 0 ]]; do
    case "$1" in
        -Fast|--fast) mode="fast" ;;
        -Full|--full) mode="full" ;;
        -Task|--task)
            [[ $# -ge 2 ]] || { echo "verify: $1 needs a task id" >&2; exit 2; }
            task="$(printf '%s' "$2" | tr '[:lower:]' '[:upper:]')"
            shift
            ;;
        -BuildDir|--build-dir)
            [[ $# -ge 2 ]] || { echo "verify: $1 needs a directory" >&2; exit 2; }
            build_dir="$2"
            shift
            ;;
        -Config|--config)
            [[ $# -ge 2 ]] || { echo "verify: $1 needs a configuration" >&2; exit 2; }
            config="$2"
            shift
            ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "verify: unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

if [[ -z "$mode" ]]; then mode="fast"; fi
if [[ -n "$task" && ! "$task" =~ ^T[0-9]{2,3}$ ]]; then
    echo "verify: invalid task id '$task': expected T01" >&2
    exit 2
fi

task_number=0
if [[ -n "$task" ]]; then
    task_number=$((10#${task#T}))
fi
requires_godot=0
if [[ -n "$task" && "$task_number" -ge 2 ]]; then
    requires_godot=1
fi

run() {
    printf '>> %s\n' "$*"
    "$@"
}

find_godot() {
    local candidates=(
        "$root/.tools/godot/Godot_v4.7.2-stable_linux.x86_64"
        "$root/.tools/godot/Godot.app/Contents/MacOS/Godot"
        "$root/.tools/godot/Godot_v4.7.2-stable_win64_console.exe"
    )
    local candidate
    for candidate in "${candidates[@]}"; do
        if [[ -e "$candidate" ]]; then
            printf '%s' "$candidate"
            return 0
        fi
    done
    return 1
}

ensure_godot_extension_list() {
    # Godot only scans res://*.gdextension into .godot/extension_list.cfg during
    # an editor import. In 4.7.2 the headless editor exits with an access
    # violation on shutdown after writing that file, so seed the one-line list
    # directly; the headless smoke test is the authoritative load check.
    local project_dir="$1"
    local list="$project_dir/.godot/extension_list.cfg"
    if [[ -f "$list" ]]; then
        return 0
    fi
    mkdir -p "$project_dir/.godot"
    printf '%s' 'res://bin/avatar_studio.gdextension' > "$list"
    echo "verify: seeded $list (headless editor import is unreliable in Godot 4.7.2)"
}

run_godot_smoke() {
    local godot="$1" project_dir="$2"
    local output code
    ensure_godot_extension_list "$project_dir"
    printf '>> %s --headless --path %s --script res://tests/smoke.gd\n' "$godot" "$project_dir"
    set +e
    output="$("$godot" --headless --path "$project_dir" --script res://tests/smoke.gd 2>&1)"
    code=$?
    set -e
    printf '%s\n' "$output"
    if [[ "$code" -ne 0 ]]; then
        echo "verify: Godot shell smoke test failed with exit code $code" >&2
        exit 1
    fi
    local pattern
    for pattern in "Failed to load GDExtension" "GDExtension load failed" "Cannot open shared library" "SCRIPT ERROR" "Parse Error"; do
        if printf '%s\n' "$output" | grep -Fq "$pattern"; then
            echo "verify: Godot shell smoke output contains '$pattern'" >&2
            exit 1
        fi
    done
}

cd "$root"

echo "verify: mode=$mode task=${task:-(none)} build=$build_dir config=$config"

if [[ "$task" == "T01" ]]; then
    run bash "$root/scripts/bootstrap.sh" --native-only
else
    run bash "$root/scripts/bootstrap.sh" --fast
fi
run cmake -S . -B "$build_dir" -DAVATAR_TESTS=ON
run cmake --build "$build_dir" --config "$config" --parallel

if [[ "$mode" == "full" ]]; then
    run ctest --test-dir "$build_dir" -C "$config" --output-on-failure
elif [[ -n "$task" ]]; then
    run ctest --test-dir "$build_dir" -C "$config" -L "fast|task_$task" --output-on-failure
else
    run ctest --test-dir "$build_dir" -C "$config" -L fast --output-on-failure
fi

godot="$(find_godot || true)"
project="$root/app/project.godot"
if [[ -z "$godot" ]]; then
    if [[ "$requires_godot" -eq 1 ]]; then
        echo "verify: Godot 4.7.2 is missing from .tools/godot. Run scripts/bootstrap.sh --fast." >&2
        echo "verify: a native-only run is only allowed for T01." >&2
        exit 1
    fi
    echo "verify: Godot not present, skipping the shell smoke (native-only)"
elif [[ ! -f "$project" ]]; then
    if [[ "$requires_godot" -eq 1 ]]; then
        echo "verify: app/project.godot is missing; the Godot shell is required for $task" >&2
        exit 1
    fi
    echo "verify: app/project.godot not present, skipping the shell smoke"
else
    if [[ "$requires_godot" -eq 1 ]]; then
        case "$(uname -s)" in
            Darwin*) extension="$root/app/bin/avatar_studio.macos.x86_64.dylib" ;;
            MINGW*|MSYS*|CYGWIN*) extension="$root/app/bin/avatar_studio.windows.x86_64.dll" ;;
            *) extension="$root/app/bin/avatar_studio.linux.x86_64.so" ;;
        esac
        if [[ ! -f "$extension" ]]; then
            echo "verify: the GDExtension artifact is missing at $extension" >&2
            echo "verify: check that .tools/godot-cpp exists and the build completed." >&2
            exit 1
        fi
    fi
    run_godot_smoke "$godot" "$root/app"
fi

if [[ -n "$task" ]]; then
    gate="$root/tests/gates/$task.ps1"
    if [[ -f "$gate" ]]; then
        if ! command -v pwsh >/dev/null 2>&1; then
            echo "verify: $gate exists but pwsh is not installed" >&2
            exit 1
        fi
        run pwsh -NoProfile -File "$gate"
    else
        echo "verify: no gate script for $task (tests/gates/$task.ps1 not present), skipped"
    fi
elif [[ "$mode" == "full" ]]; then
    echo "verify: --full without --task runs no task gate (pass --task TNN to run one)"
fi

echo "verify: PASS ($mode)"
