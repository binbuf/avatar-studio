#!/usr/bin/env bash
# Run the headless Godot shell smoke test (macOS / Linux).
#
# Finds the pinned Godot 4.7.2 binary in .tools/godot, seeds the extension list
# that the 4.7.2 headless editor fails to write reliably, runs
# app/tests/smoke.gd and fails on a non-zero exit or a fatal engine message.
#
# With --required a missing engine, project or GDExtension artifact is an error.
# Without it, a machine without Godot skips the smoke (the native-only T01 path).
# verify.sh and scripts/ci.sh use this script so the local and CI contracts share
# one implementation.
#
# Usage: ./scripts/smoke.sh [--required] [--root DIR] [--project DIR]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
project_dir=""
required=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --required) required=1 ;;
        --root)
            [[ $# -ge 2 ]] || { echo "smoke: --root needs a directory" >&2; exit 2; }
            root="$2"
            shift
            ;;
        --project)
            [[ $# -ge 2 ]] || { echo "smoke: --project needs a directory" >&2; exit 2; }
            project_dir="$2"
            shift
            ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *) echo "smoke: unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

if [[ -z "$project_dir" ]]; then
    project_dir="$root/app"
fi

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

extension_artifact() {
    local godot="$1"
    case "$godot" in
        *win64*) printf '%s' "$root/app/bin/avatar_studio.windows.x86_64.dll" ;;
        *linux*) printf '%s' "$root/app/bin/avatar_studio.linux.x86_64.so" ;;
        *) printf '%s' "$root/app/bin/avatar_studio.macos.x86_64.dylib" ;;
    esac
}

ensure_extension_list() {
    local list="$project_dir/.godot/extension_list.cfg"
    if [[ -f "$list" ]]; then
        return 0
    fi
    mkdir -p "$project_dir/.godot"
    printf '%s' 'res://bin/avatar_studio.gdextension' > "$list"
    echo "smoke: seeded $list (headless editor import is unreliable in Godot 4.7.2)"
}

godot="$(find_godot || true)"
if [[ -z "$godot" ]]; then
    if [[ "$required" -eq 1 ]]; then
        echo "smoke: FAIL - Godot 4.7.2 is missing from .tools/godot. Run scripts/bootstrap.sh --fast." >&2
        exit 1
    fi
    echo "smoke: Godot not present, skipping (native-only)"
    exit 0
fi

if [[ ! -f "$project_dir/project.godot" ]]; then
    if [[ "$required" -eq 1 ]]; then
        echo "smoke: FAIL - $project_dir/project.godot is missing; the Godot shell is required" >&2
        exit 1
    fi
    echo "smoke: $project_dir/project.godot not present, skipping"
    exit 0
fi

if [[ "$required" -eq 1 ]]; then
    extension="$(extension_artifact "$godot")"
    if [[ ! -f "$extension" ]]; then
        echo "smoke: FAIL - the GDExtension artifact is missing at $extension" >&2
        echo "smoke: check that .tools/godot-cpp exists and the build completed." >&2
        exit 1
    fi
fi

ensure_extension_list
printf '>> %s --headless --path %s --script res://tests/smoke.gd\n' "$godot" "$project_dir"
set +e
output="$("$godot" --headless --path "$project_dir" --script res://tests/smoke.gd 2>&1)"
code=$?
set -e
printf '%s\n' "$output"
if [[ "$code" -ne 0 ]]; then
    echo "smoke: FAIL - Godot shell smoke test exited $code" >&2
    exit 1
fi

for pattern in "Failed to load GDExtension" "GDExtension load failed" "Cannot open shared library" \
    "SCRIPT ERROR" "Parse Error"; do
    if printf '%s\n' "$output" | grep -Fq "$pattern"; then
        echo "smoke: FAIL - output contains '$pattern'" >&2
        exit 1
    fi
done

echo "smoke: PASS"
