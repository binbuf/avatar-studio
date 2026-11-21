# ADR-0015: Performance harness result schema and local baselines

- **Status:** Accepted
- **Date:** 2026-11-20
- **Supersedes:** —
- **Related:** [`../testing-and-quality.md`](../testing-and-quality.md), [`../architecture.md`](../architecture.md), [`../output-and-virtual-camera.md`](../output-and-virtual-camera.md)

## Context

Performance has to be a gate from the first milestone, but T05 runs before the
real capture backend (T10), tracker (T11), rig (T18), render pipeline (T26) and
transport manager (T46). A harness that waits for all of them leaves latency and
memory unmeasured for months; a harness that invents a synthetic loop measures
nothing useful. Hardware also varies, so a tight absolute number in the fast
suite would be noise.

The result JSON becomes a contract that later tasks and gates parse. Changing
its shape later means touching every consumer.

## Decision

1. **One result schema for every scenario.** `avatar_bench` emits
   `schema: "avatar_bench.result.v1"` with `build`, `hardware`, `config` and a
   `metrics` object. Percentiles are always `p50/p95/p99` plus `mean`, `max` and
   `samples`. Stages that did not run are `null`, never absent.
2. **Measure the real foundation pipeline.** Scenarios are `capture`, `ring`,
   `rig` and `render`; `rig` is the full capture → ring → scripted tracker →
   rig-stage path. The rig and render stages are explicitly bench-local
   stand-ins until T18/T26. `end_to_end_latency_ms` is `null` with a reason
   because photon-to-display needs a real camera and transport.
3. **A hardware fingerprint and provenance are mandatory.** The result records
   the OS, CPU brand, physical/logical cores, RAM, GPU name and an `fnv1a64:`
   fingerprint over that tuple, plus build type, git commit and dirty flag.
4. **The checked-in foundation baseline is local, not a release gate.** It is
   marked `kind: "local"` and `release_gate: false`, carries the fingerprint of
   the machine that produced it, and stores explicit
   `{metric, operator, target, allowance}` checks. `check.mjs` applies a 10%
   allowance by default and prints the failing metric with both values. A
   fingerprint mismatch warns by default and fails with `--require-hardware`.
5. **Perf tests are opt-in.** CTest carries a `perf` label that `verify -Fast`
   excludes with `-LE perf`; `verify -Full` runs it. The fast suite only runs
   `avatar_bench --self-test`, which validates the JSON contract on tiny
   scenarios and asserts no hardware-dependent number.
6. **Gates are task-local files.** `verify -Task TNN` runs
   `tests/gates/TNN.ps1` when present and sets `AVATAR_VERIFY_MODE` to `fast` or
   `full`, so T05 can skip a fresh measurement in the fast path while T14/T27/
   T45/T50/T64 add their own gates without changing the verifier.

## Alternatives considered

| Option | Why not |
|---|---|
| Wait for the real render/transport pipeline before measuring | Leaves the first milestone without a regression signal; the design asks for performance from the start. |
| One bespoke JSON shape per scenario | Every later task and gate needs a new parser; the design explicitly asks for one shape. |
| Assert the measured value in the fast suite | Hardware varies; a tight threshold on a shared runner is noise. The fast path asserts the contract, the full path compares to a local baseline. |
| Treat the local baseline as a release gate | It was generated on one machine; T06/T26 own the nightly and release hardware. Marking it local keeps the honesty. |
| Fail every hardware mismatch | A developer on another machine cannot use the baseline; a warning plus an opt-in strict flag is more useful. |

## Consequences

**Positive**

- Latency, frame time, dropped frames, RSS, CPU and GPU fields exist before the
  subsystems that will fill them, so later tasks extend values rather than the
  schema.
- The fast suite stays hardware-independent and under a couple of seconds.
- A regression on the machine that owns the baseline is caught by `-Full` and
  the T05 gate, with the failing metric named.

**Negative**

- The rig and render scenarios measure stand-ins until T18/T26; their numbers
  are not product numbers.
- The baseline is only meaningful on matching hardware and must be regenerated
  deliberately when the machine changes.
- `check.mjs` requires Node. The C++ self-test keeps the fast CTest path free of
  that dependency, but the gate and the runner do use Node.

**Neutral**

- `gpu_percent` is best-effort (Windows PDH) and `null` elsewhere; it is not a
  gate until T26 has a real GPU workload.
- `frame_time_ms` and `app_latency_ms` are equal while the harness is
  synchronous; T26 splits them when the render thread lands.

## Follow-ups

1. T06: run the harness nightly on the self-hosted runner and publish results.
2. T11/T18: replace the scripted tracker and bench-local rig stage; keep the
   metric names and add tracker/rig stage values.
3. T26/T46: split frame time from app-side latency, add the 10-minute soak,
   real GPU utilization and the end-to-end measurement.
4. Regenerate the local baseline when the reference machine or the pipeline
   changes materially, with a reviewed commit explaining the change.
