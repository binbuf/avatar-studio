#!/usr/bin/env bash
# Run the same local quality gates as the CI workflow (macOS / Linux).
#
#   1. clang-format drift (skipped when clang-format is not installed; CI
#      installs it and passes AVATAR_REQUIRE_FORMAT=1),
#   2. the third-party licence manifest (scripts/check-licenses.mjs),
#   3. the diagnostics source scan (advisory until T41),
#   4. the design-link checker (scripts/check-design-links.mjs),
#   5. bootstrap of the pinned dependencies,
#   6. CMake configure and a RelWithDebInfo build,
#   7. CTest label "fast" (the "perf" label is opt-in with --full),
#   8. the headless Godot shell smoke test (unless --native-only).
#
# The GitHub Actions matrix runs the same sequence on Windows, macOS and Linux.
# This script is not a replacement for scripts/verify.sh: verify also runs the
# task gate and is the objective contract.
#
# Usage: ./scripts/ci.sh [--build-dir DIR] [--config CFG] [--skip-bootstrap]
#                        [--native-only] [--full]
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="build"
config="RelWithDebInfo"
skip_bootstrap=0
native_only=0
full=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            [[ $# -ge 2 ]] || { echo "ci: --build-dir needs a directory" >&2; exit 2; }
            build_dir="$2"
            shift
            ;;
        --config)
            [[ $# -ge 2 ]] || { echo "ci: --config needs a value" >&2; exit 2; }
            config="$2"
            shift
            ;;
        --skip-bootstrap) skip_bootstrap=1 ;;
        --native-only) native_only=1 ;;
        --full) full=1 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "ci: unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

run() {
    printf '>> %s\n' "$*"
    "$@"
}

step=0
write_step() {
    step=$((step + 1))
    printf '\nci: [%d] %s\n' "$step" "$1"
}

cd "$root"

if ! command -v node >/dev/null 2>&1; then
    echo "ci: node is required for the licence, diagnostics and design-link checks" >&2
    exit 1
fi

write_step "clang-format drift"
run bash "$root/scripts/format-check.sh"

write_step "third-party licence manifest"
run node scripts/check-licenses.mjs

write_step "diagnostics source scan (advisory until T41)"
run node scripts/check-diagnostics.mjs

write_step "design links"
run node scripts/check-design-links.mjs

if [[ "$skip_bootstrap" -eq 1 ]]; then
    echo "ci: bootstrap skipped (--skip-bootstrap)"
elif [[ "$native_only" -eq 1 ]]; then
    write_step "bootstrap pinned dependencies (native only)"
    run bash "$root/scripts/bootstrap.sh" --native-only
else
    write_step "bootstrap pinned dependencies"
    run bash "$root/scripts/bootstrap.sh" --fast
fi

write_step "CMake configure"
run cmake -S . -B "$build_dir" -DAVATAR_TESTS=ON -DAVATAR_BENCHMARKS=ON

write_step "build $config"
run cmake --build "$build_dir" --config "$config" --parallel

write_step "CTest fast suite"
if [[ "$full" -eq 1 ]]; then
    run ctest --test-dir "$build_dir" -C "$config" --output-on-failure
else
    run ctest --test-dir "$build_dir" -C "$config" -L fast -LE perf --output-on-failure
fi

if [[ "$native_only" -eq 0 ]]; then
    write_step "headless Godot shell smoke"
    run bash "$root/scripts/smoke.sh" --required
fi

printf '\nci: PASS\n'
