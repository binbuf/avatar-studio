### Git

- Prefer to squash branches when merging to `main`
- Don't check anything into git that shouldn't be there (e.g. logs, telemetry, possibly configs, credentials, etc)
  - Git add and commit when a task session is completing
  - Temporary files can be checked into the non-main branches as long as they're removed before merging to `main` via squash


### Architecture & Documentation

- Keep `docs/design/*.md` aligned with the application's current and intended architecture.
- Don't add anything into git that isn't part of core design and ADRs. Proposals, tracks, diagnostics, logs should be gitignored.
  - Sometimes temporary docs are used like tasks, research, proposals but most of these are temporary workspace files. 
- Permanent design files should only be in `docs/design`.
- Update design documents when requirements, implementation direction, or architecture change.
- Track incomplete design goals accurately; distinguish planned functionality from implemented functionality.
- Create or update `docs/design/adr/*.md` for significant architectural decisions and pivots.
- Preserve decision history; supersede outdated ADRs rather than deleting them.

### Quality

- Never silently substitute a partial implementation for the requested functionality. Document blockers and incomplete requirements explicitly.
- A task that needs a live camera and a person says so in three places: `requires: camera+person`
  front matter, a `[camera+person]` suffix in its ROADMAP title (so `symphony status` and the
  task-start line show it), and a `## Preflight` section at the top of the task body.
- Announce that preflight in the session's first message and report `blocked` when the hardware or
  person is absent. A test pattern, recorded clip, or scripted tracker is not a substitute for the
  human-review items.
- `node scripts/check-task-preflight.mjs` keeps the three in sync; `scripts/verify.ps1` runs it
  whenever the local planning workspace is present.