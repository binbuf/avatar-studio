#!/usr/bin/env bash
# Build and run the Avatar Studio performance harness (macOS/Linux).
#
# Builds RelWithDebInfo, runs avatar_bench with warmup plus measurement, pins
# process priority to normal with nice(1), and writes the result JSON to
# tests/performance/results/ (gitignored).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
scenario="rig"
frames=300
warmup=30
width=1280
height=720
tracker="scripted"
fps=60
build_dir="build"
config="RelWithDebInfo"
out_dir="tests/performance/results"
out_file=""
no_build=0
no_validate=0

usage() {
    cat <<'EOF'
usage: tests/performance/run.sh [options]

  --scenario capture|ring|rig|render|all   pipeline depth (default rig)
  --frames N                               measured frames (default 300)
  --warmup N                               discarded warmup frames (default 30)
  --width N --height N                     source size (default 1280x720)
  --tracker scripted|real                  tracker backend (real is T11)
  --fps N                                  paced frames per second, 0 = unpaced (default 60)
  --build-dir DIR                          build directory (default build)
  --config NAME                            CMake configuration (default RelWithDebInfo)
  --out-dir DIR                            result directory (default tests/performance/results)
  --out-file PATH                          exact output path (single scenario only)
  --no-build                               use an existing avatar_bench
  --no-validate                            skip check.mjs --validate
  --help                                   this text
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --scenario) scenario="${2:?missing value}"; shift 2 ;;
        --frames) frames="${2:?missing value}"; shift 2 ;;
        --warmup) warmup="${2:?missing value}"; shift 2 ;;
        --width) width="${2:?missing value}"; shift 2 ;;
        --height) height="${2:?missing value}"; shift 2 ;;
        --tracker) tracker="${2:?missing value}"; shift 2 ;;
        --fps) fps="${2:?missing value}"; shift 2 ;;
        --build-dir) build_dir="${2:?missing value}"; shift 2 ;;
        --config) config="${2:?missing value}"; shift 2 ;;
        --out-dir) out_dir="${2:?missing value}"; shift 2 ;;
        --out-file) out_file="${2:?missing value}"; shift 2 ;;
        --no-build) no_build=1; shift ;;
        --no-validate) no_validate=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "run.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

cd "$root"

if [[ "$no_build" -eq 0 ]]; then
    echo ">> cmake -S . -B $build_dir -DAVATAR_TESTS=ON -DAVATAR_BENCHMARKS=ON"
    cmake -S . -B "$build_dir" -DAVATAR_TESTS=ON -DAVATAR_BENCHMARKS=ON
    echo ">> cmake --build $build_dir --config $config --target avatar_bench --parallel"
    cmake --build "$build_dir" --config "$config" --target avatar_bench --parallel
fi

exe=""
for candidate in \
    "$root/$build_dir/$config/avatar_bench" \
    "$root/$build_dir/avatar_bench" \
    "$root/$build_dir/$config/avatar_bench.exe" \
    "$root/$build_dir/avatar_bench.exe"; do
    if [[ -x "$candidate" ]]; then
        exe="$candidate"
        break
    fi
done
if [[ -z "$exe" ]]; then
    echo "run.sh: avatar_bench not found under $build_dir (build first or drop --no-build)" >&2
    exit 1
fi

if [[ "$scenario" == "all" ]]; then
    scenarios=(capture ring rig render)
else
    scenarios=("$scenario")
fi
if [[ -n "$out_file" && "${#scenarios[@]}" -gt 1 ]]; then
    echo "run.sh: --out-file is only valid with a single --scenario" >&2
    exit 2
fi

if [[ "$out_dir" = /* ]]; then
    results="$out_dir"
else
    results="$root/$out_dir"
fi
mkdir -p "$results"

stamp="$(date +%Y%m%d-%H%M%S)"
check_script="$root/tests/performance/check.mjs"
nice_prefix=()
if command -v nice >/dev/null 2>&1; then
    nice_prefix=(nice -n 0)
fi

for current in "${scenarios[@]}"; do
    if [[ -n "$out_file" ]]; then
        if [[ "$out_file" = /* ]]; then
            output_path="$out_file"
        else
            output_path="$root/$out_file"
        fi
    else
        output_path="$results/$current-$stamp.json"
    fi

    # A Windows avatar_bench.exe run from WSL needs a Windows path argument.
    output_argument="$output_path"
    if [[ "$(uname -r)" == *microsoft* ]] && [[ "$exe" == *.exe ]] && command -v wslpath >/dev/null 2>&1; then
        output_argument="$(wslpath -w "$output_path")"
    fi

    echo ">> ${nice_prefix[*]} $exe --scenario $current --tracker $tracker --frames $frames --warmup $warmup --width $width --height $height --fps $fps --output $output_path"
    "${nice_prefix[@]}" "$exe" \
        --scenario "$current" \
        --frames "$frames" \
        --warmup "$warmup" \
        --width "$width" \
        --height "$height" \
        --tracker "$tracker" \
        --fps "$fps" \
        --output "$output_argument"

    if [[ "$no_validate" -eq 0 ]]; then
        if command -v node >/dev/null 2>&1; then
            node "$check_script" --validate "$output_path"
        else
            echo "run.sh: node not found; skipping JSON validation" >&2
        fi
    fi

    cp "$output_path" "$results/latest-$current.json"
    echo "result: $output_path"
    echo "latest: $results/latest-$current.json"
done
