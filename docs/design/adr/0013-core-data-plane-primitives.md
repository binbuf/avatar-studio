# ADR-0013: Core data-plane primitives and the stable diagnostics registry

- **Status:** Accepted
- **Date:** 2026-11-20
- **Supersedes:** —
- **Related:** [`../architecture.md`](../architecture.md), [`../diagnostics.md`](../diagnostics.md), [`../testing-and-quality.md`](../testing-and-quality.md)

## Context

T03 is the first code that every later subsystem publishes through. The architecture requires a
bounded, allocation-free data plane with no mutex on the render path, and diagnostics that a user can
report without recording a video. The choices are expensive to reverse because frame, pose and error
types cross subsystem boundaries.

## Decision

Use three small, framework-free primitives in `native/core/include/avatar/core/`:

1. **`SpscRing<T, Capacity>`** — a fixed-capacity single-producer/single-consumer ring with explicit
   acquire/release orders, producer and consumer indices on separate cache lines, and a counter for
   rejected pushes. The primitive never evicts. **`DropOldestRing<T, Capacity>`** is a policy wrapper
   that discards the oldest item and retries once; the eviction policy is visible at the call site.
2. **`LatestSlot<T, Slots>`** — a single-writer latest-value slot. The writer publishes into a slot
   that is neither current nor held by a reader, then flips an atomic index. Readers increment a
   per-slot reader count before copying and re-check the index; a reader sees a complete value or the
   previous one. If every candidate slot is reader-held the writer drops the value rather than
   blocking or overwriting a live read.
3. **`FrameClock` over an injected `TimeSource`**, with `SteadyTimeSource` as the only OS-clock reader
   and `ManualTimeSource` in the core for deterministic tests. A T03 gate rejects wall-clock reads and
   `<chrono>` outside `clock.cpp`.

The diagnostics registry is a stable `enum class DiagnosticCode : uint32_t` whose numeric value is
`<area digit><NNN>`, plus a `DiagnosticInfo` table (area, severity, message key, action key) in the
same header. Codes are additive forever. `Logger` emits a code, registry severity, `FrameClock`
timestamp and up to eight fixed structured fields; it redacts frame/landmark/image payload keys in
the API. `Error` and `Status` carry a code, a short message and an optional detail string, and no
exception is thrown across the public core API.

## Alternatives considered

| Option | Why not |
|---|---|
| Seqlock for the latest-value slot | Concurrent non-atomic read/write of the payload is a data race in the C++ memory model and TSan reports it; a reader-count design avoids the race without a lock. |
| Plain double buffer | A reader that loaded the old index can still be copying when the writer wraps and overwrites that slot; the torn-read test exists to catch exactly this. |
| Mutex-protected ring or latest slot | Forbidden on the render/publish path; a lock also makes the 1M-item stress test meaningless. |
| Unbounded `std::deque` queue | Allocation and permanent latency growth under a slow consumer; the architecture rejects unbounded queues. |
| Drop-oldest hidden inside `try_push` | Hides the policy from output/capture call sites and makes the primitive unsuitable for callers that want back-pressure. |
| Diagnostic codes as enum names only | Renaming or reordering a code breaks logs and support conversations; numeric area encoding plus a table is stable and parseable. |
| Logger accepting arbitrary key/value fields | The privacy contract requires redaction by API, not reviewer discipline. |

## Consequences

**Positive**

- The data plane is allocation-free and lock-free; the stress and torn-read tests are the regression
  signal.
- The diagnostics enum/table is the single source for the UI, logs and support; a test enforces unique
  codes and non-empty message/action keys.
- `ManualTimeSource` and the clock gate make determinism enforceable rather than aspirational.

**Negative**

- `LatestSlot` uses three slots and per-slot reader counters, so a very slow reader can make the
  writer drop a value. That is acceptable for latest-value semantics but must be counted and reported.
- The drop-oldest wrapper is producer-side policy; it is not safe with a consumer inside `pop` during
  eviction. T46 owns the concurrent output queue with per-slot state.
- The registry table is hand-written until T06 generates it from the header.

**Neutral**

- The enum currently carries 69 codes (the reviewed rows in `diagnostics.md`); the number grows
  additively.
- Benchmark numbers are reported, not gated, until T05 defines thresholds.

## Follow-ups

1. T46: implement the concurrent drop-oldest output queue and replace the wrapper restriction.
2. T06: generate the `diagnostics.md` tables and the string table from the header; add the source scan
   for unregistered `AREA-NNN` literals.
3. T05: turn the microbenchmark into a regression gate on reference hardware.
4. Revisit the latest-slot slot count if a real workload reports dropped publishes.
