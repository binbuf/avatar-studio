# ADR-0018: Clean-checkout verification and the implementation-map audit

- **Status:** Accepted
- **Date:** 2026-11-20
- **Supersedes:** —
- **Related:** [testing-and-quality.md](../testing-and-quality.md),
  [implementation-map.md](../implementation-map.md),
  [ADR-0011](0011-pinned-local-dependencies.md), [ADR-0016](0016-ci-quality-gates.md)

## Context

Every foundation task was verified on the developer's tree, where `build/`, `.tools/`, `docs/progress/`
and `docs/tasks/` already exist. That proves the incremental path, not reproducibility. The T08 audit
created a detached worktree of the T07 commit and found three classes of defect that an incremental run
cannot see:

- `tests/gates/T06.ps1` required `docs/progress/T06.md`, which is gitignored, so `verify -Task T06`
  failed in a clean checkout even though the code and fast tests passed.
- The implementation map claimed a `presets` build feature that does not exist, and said no bundled
  asset exists while `assets/models/avocado_half.glb` is tracked without a provenance manifest.
- The map listed ADR-0011 as `Accepted` while the ADR and its index said `Proposed`.

The foundation also contains infrastructure that only exists as workflow YAML (the CI matrix,
sanitizers, clang-tidy). A local machine cannot execute it, so "Implemented" needs a precise meaning
for those rows.

## Decision

**The clean checkout is the acceptance environment for the foundation, and an implementation-map row
is a claim that must be checkable.**

1. A task gate must not require a gitignored workspace file. `docs/progress/`, `docs/tasks/`,
   `docs/ROADMAP.md`, `docs/PROGRESS.md` and `docs/INDEX.md` are local planning artifacts. A gate may
   read them when present and must skip them when absent.
2. A row is `Implemented` only when its code path exists and a test, CTest entry or gate exercises it.
   When a claim is premature, correct the map rather than inventing code to justify it.
3. Rows backed only by `.github/workflows/` are verified as workflow definitions by
   `tests/gates/T06.ps1`. Their first hosted run is recorded as residual risk until it is observed.
4. ADR status follows the record. A status transition from `Proposed` to `Accepted` updates the ADR,
   the index and the implementation map in the same change; a disagreement is a documentation defect.
5. `tests/gates/T08.ps1` enforces the audit mechanically: design links, map currency, absent planned
   paths, `.gitignore` coverage, no tracked generated output, and the presence of the foundation
   evidence section in `testing-and-quality.md`.

## Alternatives considered

| Option | Why not |
|---|---|
| Keep incremental verification only | A green build on a populated tree hides missing bootstrap inputs and gate dependencies; T08 found both. |
| Track `docs/progress/` so gates can read it | It is a per-session workspace, not a design deliverable; tracking it would add churn and stale notes to every commit. |
| Remove gates that read local state | Gates are the task contract; removing them loses the check instead of making it reproducible. |
| Mark CI-only rows `Prototype` | The vocabulary means a throwaway spike, not infrastructure that is written but not yet executed; the map would become less honest. |

## Consequences

**Positive**

- Bootstrap, build and fast tests are proven from a tree with no local state.
- The map cannot silently drift on paths, ADR status or ignored-file assumptions.
- Later tasks inherit a gate (`T08`) and a rule for writing gates that work in CI and clean clones.

**Negative**

- Every task gate now needs a clean-checkout sanity run at least once; that is extra local time.
- CI-only infrastructure stays `Implemented` with a named residual risk until a hosted run is observed.
- The map needs maintenance when a path or ADR status changes; `check-design-links` and the T08 gate
  catch only the mechanical part.

**Neutral**

- The audit adds no product code and does not change the Phase 1 sequence.
- `tests/gates/T08.ps1` runs only through `verify -Task T08`; it is not part of the `fast` label.

## Follow-ups

1. T09+: run `verify -Fast -Task TNN` from a clean worktree when a task changes bootstrap, CMake or a
   gate; record the result in the task note.
2. The first hosted CI run after T08 must be observed and recorded; if a job fails, fix the workflow
   before Phase 1 feature work continues.
3. T43a/T60: add the per-asset provenance manifest for `assets/models/avocado_half.glb` and the bundled
   example set; then flip the legal-posture row in the implementation map.
4. Extend `tests/gates/T08.ps1` if a future task adds a new generated tree or a new ignored workspace
   directory.
