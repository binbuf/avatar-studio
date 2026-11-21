#!/usr/bin/env bash
# Fetch the pinned native and Godot dependencies into .tools/ (macOS / Linux).
#
# Idempotent: an already verified dependency is left alone. Every download is
# SHA256 checked before it is installed. CMake consumes .tools/ directly and
# never downloads anything itself.
#
# Pinned versions and exact download URLs (also recorded in docs/progress/T02.md):
#
#   Godot 4.7.2 stable (released 2026-08-18)
#     linux   https://github.com/godotengine/godot/releases/download/4.7.2-stable/Godot_v4.7.2-stable_linux.x86_64.zip
#             SHA256 CADD3204E728A35D3F13ADB7FD0D7902636B79F6B95C40C265EB73B6C35329E4
#     macos   https://github.com/godotengine/godot/releases/download/4.7.2-stable/Godot_v4.7.2-stable_macos.universal.zip
#             SHA256 C58A24E31D720BE9D62F60CB5627C4E695FB72F21B0CFE1BC9CCAA9A3B3BA63E
#     windows https://github.com/godotengine/godot/releases/download/4.7.2-stable/Godot_v4.7.2-stable_win64.exe.zip
#             SHA256 731980F9608D61333E5BAF54A2EF17210ACC7A538446C0CB9969F002ACA1E953
#     Installed at .tools/godot/
#
#   godot-cpp 10.0.0-stable (released 2026-09-15), built with GODOTCPP_API_VERSION=4.7
#     https://github.com/godotengine/godot-cpp/archive/refs/tags/10.0.0-stable.zip
#     SHA256 C763A08B8EC18DAE300CD54BB8848FAF1A5FD11DA629449D16649A72A1884727
#     Installed at .tools/godot-cpp/
#
#   nlohmann/json 3.11.3
#     https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
#     SHA256 9BEA4C8066EF4A1C206B2BE5A36302F8926F7FDC6087AF5D20B417D0CF103EA6
#     Installed at .tools/nlohmann_json/include/nlohmann/json.hpp
#
# godot-cpp 10.x is versioned independently of Godot and targets Godot 4.3+
# through the GODOTCPP_API_VERSION option. 4.7 is the pinned target for this
# project; see ADR-0012. Do not silently fall back to Godot 4.8 beta or an
# older engine: a missing or mismatched download is an error.
#
# Usage: ./scripts/bootstrap.sh [--fast] [--native-only] [--force]
#   --fast        Fetch the pinned set required by the fast verification path
#                 (the default set today).
#   --native-only Skip Godot and godot-cpp; used by the T01 verification path.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tools_dir="$root/.tools"
downloads_dir="$tools_dir/downloads"
force=0
fast=0
native_only=0

for arg in "$@"; do
    case "$arg" in
        --force) force=1 ;;
        --fast|-Fast) fast=1 ;;
        --native-only|-NativeOnly) native_only=1 ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "bootstrap: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if [[ "$fast" -eq 1 && "$native_only" -eq 1 ]]; then
    echo "bootstrap: choose either --fast or --native-only, not both" >&2
    exit 2
fi

case "$(uname -s)" in
    Darwin*) platform="macos" ;;
    Linux*) platform="linux" ;;
    MINGW*|MSYS*|CYGWIN*) platform="windows" ;;
    *) echo "bootstrap: unsupported platform: $(uname -s)" >&2; exit 2 ;;
esac

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

download_file() {
    local url="$1" destination="$2"
    echo "           $url"
    if ! curl -fL --retry 3 --connect-timeout 20 -o "$destination" "$url"; then
        echo "bootstrap: download failed: $url" >&2
        echo "bootstrap: fallback: download the file manually, place it in $downloads_dir, and re-run this script" >&2
        exit 1
    fi
}

extract_zip() {
    local archive="$1" destination="$2"
    rm -rf "$destination"
    mkdir -p "$destination"
    if command -v unzip >/dev/null 2>&1; then
        unzip -q "$archive" -d "$destination"
    elif command -v python3 >/dev/null 2>&1; then
        python3 -m zipfile -e "$archive" "$destination"
    else
        echo "bootstrap: neither unzip nor python3 is available to extract $archive" >&2
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
        download_file "$url" "$partial"
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

install_archive() {
    local name="$1" version="$2" url="$3" expected="$4" file="$5" sentinel="$6" strip_root="$7"

    local dest="$tools_dir/$name"
    local marker="$dest/.pinned"
    local marker_text="$name $version sha256=$expected"
    expected="$(printf '%s' "$expected" | tr 'A-Z' 'a-z')"

    if [[ "$force" -eq 0 && -e "$dest/$sentinel" && -f "$marker" ]]; then
        if [[ "$(cat "$marker")" == "$marker_text" ]]; then
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
        download_file "$url" "$partial"
        local actual
        actual="$(sha256_of "$partial" | tr 'A-Z' 'a-z')"
        if [[ "$actual" != "$expected" ]]; then
            rm -f "$partial"
            echo "bootstrap: SHA256 mismatch for $name $version: expected $expected, got $actual" >&2
            exit 1
        fi
        mv -f "$partial" "$cache"
    fi

    local extract_root="$tools_dir/$name.extract"
    echo "  extract  $name $version"
    extract_zip "$cache" "$extract_root"

    local source="$extract_root"
    if [[ "$strip_root" -eq 1 ]]; then
        local entries=()
        shopt -s nullglob
        entries=("$extract_root"/*)
        shopt -u nullglob
        if [[ ${#entries[@]} -eq 1 && -d "${entries[0]}" ]]; then
            source="${entries[0]}"
        fi
    fi

    rm -rf "$dest"
    mv "$source" "$dest"
    rm -rf "$extract_root"

    if [[ ! -e "$dest/$sentinel" ]]; then
        echo "bootstrap: archive $name $version did not contain $sentinel" >&2
        exit 1
    fi

    printf '%s' "$marker_text" > "$marker"
    echo "  install  $name $version -> $dest"
}

echo "bootstrap: pinned dependencies in $tools_dir"
if [[ "$fast" -eq 1 ]]; then
    echo "bootstrap: fast profile (all pinned dependencies)"
fi

install_dependency \
    "nlohmann_json" \
    "3.11.3" \
    "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp" \
    "9BEA4C8066EF4A1C206B2BE5A36302F8926F7FDC6087AF5D20B417D0CF103EA6" \
    "json.hpp" \
    "nlohmann_json/include/nlohmann/json.hpp"

install_archive \
    "eigen" \
    "3.4.0" \
    "https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.zip" \
    "EBA3F3D414D2F8CBA2919C78EC6DAAB08FC71BA2BA4AE502B7E5D4D99FC02CDA" \
    "eigen-3.4.0.zip" \
    "Eigen/Core" \
    1

if [[ "$native_only" -eq 1 ]]; then
    echo "bootstrap: Godot and godot-cpp skipped (--native-only)"
    echo "bootstrap: done (native only). Configure with: cmake -S . -B build"
    exit 0
fi

case "$platform" in
    macos)
        install_archive \
            "godot" \
            "4.7.2" \
            "https://github.com/godotengine/godot/releases/download/4.7.2-stable/Godot_v4.7.2-stable_macos.universal.zip" \
            "C58A24E31D720BE9D62F60CB5627C4E695FB72F21B0CFE1BC9CCAA9A3B3BA63E" \
            "Godot_v4.7.2-stable_macos.universal.zip" \
            "Godot.app/Contents/MacOS/Godot" \
            0
        godot_bin="$tools_dir/godot/Godot.app/Contents/MacOS/Godot"
        chmod +x "$godot_bin"
        ;;
    linux)
        install_archive \
            "godot" \
            "4.7.2" \
            "https://github.com/godotengine/godot/releases/download/4.7.2-stable/Godot_v4.7.2-stable_linux.x86_64.zip" \
            "CADD3204E728A35D3F13ADB7FD0D7902636B79F6B95C40C265EB73B6C35329E4" \
            "Godot_v4.7.2-stable_linux.x86_64.zip" \
            "Godot_v4.7.2-stable_linux.x86_64" \
            0
        godot_bin="$tools_dir/godot/Godot_v4.7.2-stable_linux.x86_64"
        chmod +x "$godot_bin"
        ;;
    windows)
        install_archive \
            "godot" \
            "4.7.2" \
            "https://github.com/godotengine/godot/releases/download/4.7.2-stable/Godot_v4.7.2-stable_win64.exe.zip" \
            "731980F9608D61333E5BAF54A2EF17210ACC7A538446C0CB9969F002ACA1E953" \
            "Godot_v4.7.2-stable_win64.exe.zip" \
            "Godot_v4.7.2-stable_win64_console.exe" \
            0
        godot_bin="$tools_dir/godot/Godot_v4.7.2-stable_win64_console.exe"
        ;;
esac

install_archive \
    "godot-cpp" \
    "10.0.0-stable" \
    "https://github.com/godotengine/godot-cpp/archive/refs/tags/10.0.0-stable.zip" \
    "C763A08B8EC18DAE300CD54BB8848FAF1A5FD11DA629449D16649A72A1884727" \
    "godot-cpp-10.0.0-stable.zip" \
    "CMakeLists.txt" \
    1

echo "bootstrap: godot      $godot_bin"
echo "bootstrap: godot-cpp  $tools_dir/godot-cpp"
echo "bootstrap: done. Configure with: cmake -S . -B build"
