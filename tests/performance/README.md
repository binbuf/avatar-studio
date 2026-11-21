# Performance harness

`avatar_bench` measures the foundation pipeline: deterministic capture source →
ring publish → scripted tracker → bench-local rig stage. The real render thread,
camera callbacks and transport queues land in T10/T11/T26/T46; until then the
harness is synchronous and reports `end_to_end_latency_ms` as `null` with a
reason.

## Run

```powershell
# Windows: build RelWithDebInfo, warmup + 300 measured frames, write results/
powershell -NoProfile -ExecutionPolicy Bypass -File tests/performance/run.ps1 `
  -Scenario rig -Frames 300 -Warmup 30
```

```bash
# macOS/Linux
tests/performance/run.sh --scenario rig --frames 300 --warmup 30
```

Results are written to `tests/performance/results/` (gitignored), plus a
`latest-<scenario>.json` copy. The process priority is pinned to Normal (`nice 0`
on Unix). The bench discards the warmup frames in the same process so ring slots
and allocator state are warm.

## Scenarios

| Scenario | Stages |
|---|---|
| `capture` | test pattern render + ring publish |
| `ring` | capture + pin/acquire + sampled pixel read + release |
| `rig` | ring + scripted tracker + rig smoothing/matrix compose (the full foundation pipeline) |
| `render` | rig + CPU BGRA→RGBA compositing/readback proxy (real GPU render is T26) |

`--tracker real` is accepted as a switch but exits with a clear "unavailable"
message until the MediaPipe backend lands in T11.

## JSON contract

Every scenario emits the same shape (`schema: avatar_bench.result.v1`):

```json
{
  "schema": "avatar_bench.result.v1",
  "scenario": "rig",
  "tracker": "scripted",
  "build": {"type": "RelWithDebInfo", "git_commit": "...", "git_dirty": false, "compiler": "..."},
  "config": {"frames": 300, "warmup_frames": 30, "width": 1280, "height": 720,
             "pixel_format": "bgra8", "fps": 60, "scenario": "rig"},
  "hardware": {"os": "...", "arch": "x86_64", "cpu": "...", "physical_cores": 12,
               "logical_cores": 24, "memory_bytes": 0, "gpu": "...",
               "fingerprint": "fnv1a64:..."},
  "metrics": {
    "frames": {"requested": 300, "warmup": 30, "produced": 300, "measured": 300, "missed": 0},
    "dropped_frames": 0,
    "dropped_tracking_frames": 0,
    "frame_time_ms": {"p50": 0, "p95": 0, "p99": 0, "mean": 0, "max": 0, "samples": 300},
    "frame_interval_ms": {"p50": 0, "p95": 0, "p99": 0, "mean": 0, "max": 0, "samples": 299},
    "app_latency_ms": {"p50": 0, "p95": 0, "p99": 0, "mean": 0, "max": 0, "samples": 300},
    "end_to_end_latency_ms": null,
    "stage_ms": {"capture": {...}, "ring": {...}, "tracker": {...}, "rig": {...}, "render": null},
    "rss_bytes": 0,
    "cpu_percent_one_core": 0,
    "gpu_percent": 0
  },
  "notes": ["..."],
  "end_to_end_note": "...",
  "gpu_note": "..."
}
```

- `app_latency_ms` is the app-side latency definition from
  `architecture.md`: capture callback (frame start) to the transport-submit
  proxy (end of the last measured stage).
- `frame_time_ms` is total per-frame work. The synchronous foundation harness
  makes the two intervals equal until the render/tracking threads land (T26).
- `frame_interval_ms` is start-to-start pacing (measured when `--fps > 0`).
- `rss_bytes` is the process working set at the end of the measurement window.
- `cpu_percent_one_core` is process CPU time / wall time; 100 = one full core.
- `gpu_percent` is null when the OS counter is unavailable. On Windows it is
  best-effort PDH `\GPU Engine(*)\Utilization Percentage` for this PID.

## Check against a baseline

```powershell
node tests/performance/check.mjs `
  --baseline tests/performance/baselines/foundation-local.json `
  --result tests/performance/results/latest-rig.json
```

The baseline is a result plus explicit checks:

```json
{"metric": "metrics.app_latency_ms.p95", "operator": "<=", "target": 100.0, "allowance": 0.10}
```

The effective threshold is `target * (1 + allowance)` for `<=` and
`target * (1 - allowance)` for `>=`; a per-check `allowance` overrides the
baseline default. Failures print the metric, result value, target and allowed
value.

`check.mjs --validate <result.json>` checks the JSON contract without a
baseline.

## Baseline policy

`tests/performance/baselines/foundation-local.json` is a **local** baseline
(`kind: "local"`, `release_gate: false`) generated on the machine whose
`hardware.fingerprint` it records. It is not a release gate: T06 owns the nightly
runner and T26/T46 own real render/transport gates. Regenerate it with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/performance/run.ps1 `
  -Scenario rig -Frames 300 -Warmup 30 -OutFile tests/performance/results/foundation-rig.json
node tests/performance/make_baseline.mjs `
  tests/performance/results/foundation-rig.json `
  tests/performance/baselines/foundation-local.json
```

## CTest labels and gates

- `avatar_bench_selftest` — labels `fast;task_T05`; runs every scenario tiny and
  validates the JSON. No hardware-dependent number is asserted.
- `avatar_bench_perf` — labels `perf;task_T05`; a real measurement. `verify -Fast`
  excludes `perf` with `-LE perf`; `verify -Full` runs it.
- `tests/gates/T05.ps1` — checks the files, baseline shape, labels, self-test,
  comparator pass/fail behaviour and (in `-Full`) a fresh measurement.
