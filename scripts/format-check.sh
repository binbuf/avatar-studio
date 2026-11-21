#!/usr/bin/env bash
# Check (or apply) the pinned clang-format style.
#
# Runs clang-format 18.1.8 with the repository .clang-format over every C++
# source in native/, tests/ and app/. CI installs clang-format and runs this
# with --require, so drift fails the build.
#
# A developer machine without clang-format prints a skip and exits 0 unless
# --require (or AVATAR_REQUIRE_FORMAT=1) is set.
#
# Usage: ./scripts/format-check.sh [--require] [--fix] [--root DIR]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
require=0
fix=0
requested="${CLANG_FORMAT:-}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --require) require=1 ;;
        --fix) fix=1 ;;
        --root)
            [[ $# -ge 2 ]] || { echo "format-check: --root needs a directory" >&2; exit 2; }
            root="$2"
            shift
            ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "format-check: unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

if [[ "${AVATAR_REQUIRE_FORMAT:-0}" == "1" ]]; then require=1; fi

if [[ ! -f "$root/.clang-format" ]]; then
    echo "format-check: FAIL - .clang-format is missing from $root" >&2
    exit 1
fi

executable=""
if [[ -n "$requested" ]]; then
    if command -v cygpath >/dev/null 2>&1; then
        requested="$(cygpath -u "$requested")"
    elif command -v wslpath >/dev/null 2>&1 && [[ "$requested" == *\\* || "$requested" == [A-Za-z]:* ]]; then
        requested="$(wslpath -u "$requested")"
    fi
    if command -v "$requested" >/dev/null 2>&1; then
        executable="$(command -v "$requested")"
    elif [[ -x "$requested" ]]; then
        executable="$requested"
    fi
else
    for candidate in clang-format clang-format-18; do
        if command -v "$candidate" >/dev/null 2>&1; then
            executable="$(command -v "$candidate")"
            break
        fi
    done
fi

if [[ -z "$executable" ]]; then
    if [[ "$require" -eq 1 ]]; then
        echo "format-check: FAIL - clang-format is required but was not found." >&2
        echo "format-check: install clang-format 18.1.8 or set CLANG_FORMAT." >&2
        exit 1
    fi
    echo "format-check: SKIP - clang-format not found."
    echo "format-check: install clang-format 18.1.8 (CI pins it) or set CLANG_FORMAT."
    echo "format-check: set AVATAR_REQUIRE_FORMAT=1 to make this fatal."
    exit 0
fi

files=()
while IFS= read -r file; do
    files+=("$file")
done < <(
    find "$root/native" "$root/tests" "$root/app" \
        -type f \( -name '*.h' -o -name '*.hh' -o -name '*.hpp' -o -name '*.hxx' \
        -o -name '*.ipp' -o -name '*.inl' -o -name '*.c' -o -name '*.cc' \
        -o -name '*.cpp' -o -name '*.cxx' \) \
        -not -path '*/build/*' -not -path '*/build-*/*' -not -path '*/.tools/*' \
        -not -path '*/node_modules/*' -not -path '*/results/*' -not -path '*/.godot/*' \
        2>/dev/null | sort
)

if [[ ${#files[@]} -eq 0 ]]; then
    echo "format-check: PASS (no C++ sources found)"
    exit 0
fi

echo "format-check: $("$executable" --version 2>&1) on ${#files[@]} files"

if [[ "$fix" -eq 1 ]]; then
    "$executable" -i --style=file "${files[@]}"
    echo "format-check: PASS (files rewritten)"
    exit 0
fi

if "$executable" --dry-run --Werror --style=file "${files[@]}"; then
    echo "format-check: PASS"
    exit 0
fi

echo "format-check: FAIL - formatting drift." >&2
echo "format-check: run scripts/format-check.sh --fix, review, and commit." >&2
exit 1
