# ADR-0023: Headless M0 harness, measurement definitions, and soak thresholds

- **Status:** Accepted
- **Date:** 2026-11-21
- **Supersedes:** -
- **Related:** [testing-and-quality.md](../testing-and-quality.md), [tracking.md](../tracking.md), [roadmap.md](../roadmap.md), [ADR-0019](0019-tracker-c-abi-v1.md), [ADR-0021](0021-real-mediapipe-windows-backend.md)

## Context

M0 asks whether the tracker and capture stack are viable before rendering exists. The answer
must be a measurement, not a demo. The five-function tracker ABI v1 has no statistics query, so
the caller cannot read the worker's private inference timer, busy-drop counter, or confidence
bins. The T11 benchmark compiled the worker into a separate measurement executable; that is
useful for graph timing but is not the app-facing path and does not exercise the DLL boundary,
the latest-only result handoff, or a 30-minute process.

The harness also has to survive the difference between "fast model" and "fast pipeline": a
submit-to-poll latency that excludes scheduling, or a frame-age metric that ignores a stalled
consumer, would hide the failure mode M0 exists to catch. Memory evidence needs a fixed sampling
method and a baseline after warm-up, and the human jitter call needs a recorded pose sequence and
a viewable clip rather than a single number.

## Decision

`tests/m0/m0.cpp` builds `avatar_m0`, a headless executable that links the app's core, capture and
tracking libraries and loads the real tracker DLL dynamically through the T09 C ABI. It supports
a deterministic test pattern and an `AVCLIP1` replay (the T11 diagnostic clip format, looped),
and it runs either the real backend or `ScriptedTracker`.

The report is one JSON object, `schema: avatar_m0.result.v1`. Its definitions are fixed:

| Field | Definition |
|---|---|
| `metrics.inference_ms` | Caller-visible submit start to the poll that returns that frame's result. Includes submit copy, worker scheduling and at most 1 ms of poll granularity. It is **not** a vendor-internal graph timer. |
| `metrics.frame_age_ms` | Poll time minus the frame's source timestamp. This is the end-to-end age a consumer would see, including the polling delay. |
| `metrics.dropped` | Frames that produced no result, classified after the run: `superseded` (a newer result arrived first), `stalled` (no later result), `submit_error` (push failed). `source_missed` is reported separately because it never reached the tracker. |
| `metrics.confidence` | Ten bins over [0,1] from every resolved result, including no-face zeros. |
| `metrics.rss` / `metrics.leak` | Working set sampled after warm-up and every configured interval. The leak check fails when RSS grows more than 5% from the first post-warm-up sample to the last, or when the handle count is strictly increasing across at least five samples. |
| `metrics.cpu` | Process CPU as a fraction of one core and a per-thread CPU snapshot, so the inference worker is visible. |
| `metrics.pose_stability` | Frame-to-frame quaternion delta and yaw/pitch/roll ranges; a numeric proxy only. |

`tests/m0/run.ps1` owns the two budgets:

| Mode | Duration | Warm-up | RSS interval | Leak failure |
|---|---|---|---|---|
| `short` (CI) | 2 minutes total, 60 s per size | 5 s | 5 s | reported, not fatal |
| `full` (soak) | 30 minutes total, 15 min per size | 60 s | 60 s | fatal (`--strict`) |

Both sizes, 192x192 and 256x256 prepared input, run in every mode. Results go to the gitignored
`tests/performance/results/`; the human summary goes to `docs/progress/T13-m0-results.md` for the
soak and `docs/progress/T13-m0-short.md` for the short run. Each run writes a pose CSV, an SVG
pose chart, and a short BMP clip with eye, iris, nose, mouth and chin landmarks overlaid.

The M0 thresholds applied by `tests/gates/T14.ps1` are:

| Check | Reference tier | Minimum tier |
|---|---:|---:|
| Inference p95 | ≤ 15 ms | ≤ 30 ms |
| Inference p99 | ≤ 25 ms | ≤ 45 ms |
| Frame age p95 | ≤ 50 ms | ≤ 80 ms |
| Frame age p99 | ≤ 100 ms | ≤ 150 ms |
| Dropped frames | ≤ 2% | ≤ 2% |
| RSS growth after warm-up | ≤ 5% | ≤ 5% |
| Handle count | not monotonically growing | not monotonically growing |
| Soak duration | ≥ 1800 s total, ≥ 600 s per size | same |

The tier is recorded by the harness from logical cores (≥ 16) and RAM (≥ 32 GiB); T14 may
override it with the hardware table, but a measured value is never changed to make a gate pass.
The M0 gate requires a real tracker and a real replay source. A scripted run is explicitly
labelled `synthetic: true` and can prove the harness in T13, but it cannot satisfy T14.

## Alternatives considered

| Option | Why not |
|---|---|
| Extend ABI v1 with a statistics query | A layout/semantic change to a pinned ABI for a feasibility harness; T26 owns the app-facing HUD decision. The caller-visible latency is the number the app actually experiences. |
| Reuse `avatar_tracker_bench` as the M0 harness | It compiles the worker directly and never crosses the DLL boundary; it cannot measure the latest-only handoff, the real process RSS, or a 30-minute DLL session. |
| Use a live camera for the soak | Unattended 30-minute runs cannot depend on a person staying in frame; the T11 clip is deterministic and local-only. A camera mode remains a future option. |
| Gate on inference only | A fast model behind a stalled poll loop would pass. Frame age is reported and gated alongside inference. |
| Fail the short CI run on a 2-minute RSS trend | Too noisy and too short to distinguish a leak from lazy allocator growth; the full soak owns the leak verdict. |

## Consequences

Positive: M0 is measured through the same C ABI the app will use, at both input sizes, with
latency, age, drop reasons, confidence, memory, handles, CPU and human-review artifacts in one
report. The 30-minute run is reproducible and its JSON is machine-checked.

Negative: submit-to-poll latency includes scheduling and poll granularity, so it is slightly
larger than the T11 graph timer; the harness does not claim to be a vendor benchmark. The real
backend is Windows-only today, so other platforms exercise the scripted path. The full soak adds
about 30 minutes to `verify -Full -Task T13` and the nightly, which is why it is under the `perf`
label rather than the fast suite.

## Follow-ups

- T14 consumes the soak JSON and makes the keep/pivot decision; it must review the pose clip.
- T26 decides whether the app-facing HUD gets real tracker statistics and how; this harness does
  not change ABI v1.
- T18 owns pose filtering and the reprojection/plausibility confidence; the pose-stability number
  here is a review aid, not a filter.
- A future platform task must add macOS/Linux real backends and rerun this harness there.
