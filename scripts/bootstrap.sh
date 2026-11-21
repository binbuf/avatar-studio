#!/usr/bin/env bash
# Fetch the pinned native dependencies into .tools/ (macOS / Linux).
#
# Idempotent: an already verified dependency is left alone. Every download is
# SHA256 checked before it is installed. CMake consumes .tools/ directly and
# never downloads anything itself.
#
# Usage: ./scripts/bootstrap.sh [--force]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tools_dir="$root/.tools"
downloads_dir="$tools_dir/downloads"
force=0

for arg in "$@"; do
    case "$arg" in
        --force) force=1 ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) echo "bootstrap: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        echo "bootstrap: no sha256sum or shasum found" >&2
        exit 1
    fi
}

install_dependency() {
    local name="$1" version="$2" url="$3" expected="$4" file="$5" dest_rel="$6"

    local dest="$tools_dir/$dest_rel"
    local marker="$tools_dir/$name/.pinned"
    local marker_text="$name $version sha256=$expected"
    expected="$(printf '%s' "$expected" | tr 'A-Z' 'a-z')"

    if [[ "$force" -eq 0 && -f "$dest" && -f "$marker" ]]; then
        local marker_ok=0 hash_ok=0
        if [[ "$(cat "$marker")" == "$marker_text" ]]; then marker_ok=1; fi
        if [[ "$(sha256_of "$dest" | tr 'A-Z' 'a-z')" == "$expected" ]]; then hash_ok=1; fi
        if [[ "$marker_ok" -eq 1 && "$hash_ok" -eq 1 ]]; then
            echo "  ok       $name $version (verified, skipped)"
            return 0
        fi
    fi

    local cache="$downloads_dir/$file"
    local cache_ok=0
    if [[ -f "$cache" && "$(sha256_of "$cache" | tr 'A-Z' 'a-z')" == "$expected" ]]; then
        cache_ok=1
    fi

    if [[ "$force" -eq 0 && "$cache_ok" -eq 1 ]]; then
        echo "  cache    $name $version (verified download cache)"
    else
        mkdir -p "$downloads_dir"
        local partial="$cache.part"
        rm -f "$partial"
        echo "  download $name $version"
        echo "           $url"
        curl -fL --retry 3 --connect-timeout 20 -o "$partial" "$url"
        local actual
        actual="$(sha256_of "$partial" | tr 'A-Z' 'a-z')"
        if [[ "$actual" != "$expected" ]]; then
            rm -f "$partial"
            echo "bootstrap: SHA256 mismatch for $name $version: expected $expected, got $actual" >&2
            exit 1
        fi
        mv -f "$partial" "$cache"
    fi

    mkdir -p "$(dirname "$dest")"
    cp -f "$cache" "$dest"

    local installed
    installed="$(sha256_of "$dest" | tr 'A-Z' 'a-z')"
    if [[ "$installed" != "$expected" ]]; then
        echo "bootstrap: installed copy of $name failed SHA256 verification: $installed" >&2
        exit 1
    fi

    printf '%s' "$marker_text" > "$marker"
    echo "  install  $name $version -> $dest"
}

echo "bootstrap: pinned dependencies in $tools_dir"

install_dependency \
    "nlohmann_json" \
    "3.11.3" \
    "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp" \
    "9BEA4C8066EF4A1C206B2BE5A36302F8926F7FDC6087AF5D20B417D0CF103EA6" \
    "json.hpp" \
    "nlohmann_json/include/nlohmann/json.hpp"

echo "bootstrap: done. Configure with: cmake -S . -B build"
