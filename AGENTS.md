### Git

- Don't check anything into git that shouldn't be there (e.g. logs, telemetry, possibly configs, credentials, etc)
  - Git add and commit when a task session is completing

### Architecture & Documentation

- Keep `docs/design/*.md` aligned with the application's current and intended architecture.
- Don't add unnecessary files into `docs/`. Sometimes temporary docs are used like tasks, research, proposals but most of these are temporary workspace files. Permanent design files should only be in `docs/design`.
- Update design documents when requirements, implementation direction, or architecture change.
- Track incomplete design goals accurately; distinguish planned functionality from implemented functionality.
- Create or update `docs/design/adr/*.md` for significant architectural decisions and pivots.
- Preserve decision history; supersede outdated ADRs rather than deleting them.

### Quality

- Never silently substitute a partial implementation for the requested functionality. Document blockers and incomplete requirements explicitly.