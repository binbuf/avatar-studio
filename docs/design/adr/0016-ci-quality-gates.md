# ADR-0016: CI quality gates, pinned formatting, and the licence manifest

- **Status:** Accepted
- **Date:** 2026-11-20
- **Supersedes:** —
- **Related:** [testing-and-quality.md](../testing-and-quality.md),
  [security-privacy.md](../security-privacy.md), [diagnostics.md](../diagnostics.md),
  [ADR-0011](0011-pinned-local-dependencies.md)

## Context

T06 is the first task whose deliverable is the guardrail rather than a subsystem. The existing
`scripts/verify.ps1|sh` contract builds and tests the native tree, but it does not enforce formatting,
the dependency manifest, or the diagnostics source rules. CI did not exist, so "every push builds on
three platforms" was an intention in `testing-and-quality.md`, not a fact.

Three constraints shape the decision:

- Fast jobs must not need a camera or a GPU; golden images and performance belong to a nightly job.
- `native/licenses.json` must be a build gate, not a report: a new dependency without an entry has to
  fail the build, and a GPL/AGPL/non-commercial licence has to fail unless a reviewed exception is
  recorded.
- The diagnostics string table and its generated registry arrive in T41, so the source scan cannot be
  a blocking gate yet.

## Decision

`scripts/ci.ps1` and `scripts/ci.sh` are the local mirror of the fast CI job: formatting drift, the
licence manifest, the advisory diagnostics scan, bootstrap, configure, a RelWithDebInfo build,
`ctest -L fast -LE perf`, and the headless Godot smoke. `.github/workflows/ci.yml` runs the same
sequence on Windows (MSVC, default generator), macOS and Ubuntu 24.04. `.github/workflows/nightly.yml`
owns the complete suite, ASan/UBSan and TSan, clang-tidy, lavapipe golden images, and the performance
harness.

Formatting is pinned to clang-format 18.1.8 and `.clang-format`; the format job installs that exact
version and fails on drift. Static analysis is pinned to clang-tidy 18 and `.clang-tidy`, and is
nightly because it is the slowest CPU-only job.

`native/licenses.json` is the single source of truth for third-party components. `scripts/check-licenses.mjs`
scans angle-bracket includes, rejects any third-party include without a manifest entry, rejects a
component that lacks name/version/licence/url/modified, and rejects a licence on the deny list unless
the component has a reviewed exception (reason plus ADR). The CTest checks `license_manifest_check` and
`diagnostics_source_scan` run in the fast label.

The diagnostics scan (`scripts/check-diagnostics.mjs`) rejects a literal `AREA-NNN` absent from the
`DiagnosticCode` enum and a raw log call without a code. It is advisory (exit 0 with a warning) until
T41; `--strict` is the mode the gate and self-test exercise today.

Release-blocking jobs are marked `(release-blocking)` in the job name, with a `# release-blocking: true`
comment. The list and the branch-protection expectations live in `docs/progress/T06.md`.

## Alternatives considered

| Option | Why not |
|---|---|
| A separate CI script per OS | Drifts immediately; the matrix must run the same contract as the developer. |
| A licence report instead of a gate | A report nobody reads lets a GPL dependency ship. |
| clang-tidy on every push | Doubles the critical path for a check that is not per-commit actionable; nightly keeps it blocking. |
| SHA-pin actions now | Correct end state, but each SHA must be verified against the upstream repository; major tags are first-party and `contents: read` only. Recorded as a follow-up. |
| Make the diagnostics scan blocking now | The generated string table and the T41 wiring do not exist yet; a blocking scan would encode a temporary state. |

## Consequences

Positive: one command (`scripts/ci.ps1`) predicts CI; formatting and licence drift cannot land; the
licence decision is reviewable in one file; the nightly job owns everything expensive.

Negative: the existing native tree was reformatted once (mechanical, no behaviour change); Node is a
build-time requirement for two gates; the format job needs a pinned LLVM install; the golden and
performance jobs are not yet real release gates because T26/T46 have not landed.

Neutral: `.tools/` remains the only download location, and CI caches nothing yet. Action SHA pinning
and `.tools/` caching are follow-ups, not omissions hidden by this record.

## Follow-ups

1. Verify and pin first-party action commit SHAs; add Dependabot or a scheduled review.
2. Cache `.tools/` in CI keyed on the pin table, and move the engine/godot-cpp pins into the manifest
   (ADR-0011 follow-up).
3. Remove `--no-tests=ignore` from the golden job when T26 registers the first golden test.
4. Move the diagnostics scan from advisory to blocking in T41.
5. Add the self-hosted reference runner and make the performance job release-blocking.
