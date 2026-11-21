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

if [[ "$requires_godot" -eq 1 ]]; then
    run bash "$root/scripts/smoke.sh" --required
else
    run bash "$root/scripts/smoke.sh"
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
