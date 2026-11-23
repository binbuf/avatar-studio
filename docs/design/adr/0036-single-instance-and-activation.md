# ADR-0036 - Single instance, local activation, and the `--new-instance` escape hatch

- **Status:** Accepted
- **Date:** 2026-11-22
- **Supersedes:** -
- **Related:** [architecture.md](../architecture.md), [editor-ux.md](../editor-ux.md), [diagnostics.md](../diagnostics.md)
- **Tasks:** T25a, T58

## Context

The application owns an exclusive camera and a virtual camera. A second launch that opens the same
device is a support ticket: one instance loses the camera, the output device is registered twice, and
the user sees a failure instead of the window they already had. The design already says a second
launch forwards `activate` and exits, and that `--new-instance` is a testing escape hatch. T25a has
to realise that contract before the real camera and virtual camera land (T26/T57).

The activation message is local IPC. It must not open a network port, must not be able to start a
second camera, and must not depend on a reboot or manual cleanup after a crash. Windows and
macOS/Linux have different natural primitives; the interface has to stay the same.

## Decision

1. **Lock.** A per-user `InstanceLock` in `native/instance/`. Windows uses a named mutex in the
   session `Local\` namespace, named from a per-user scope hash and the application id. macOS/Linux
   use an exclusive non-blocking `flock` on a per-user runtime file (`$XDG_RUNTIME_DIR` when short
   enough, otherwise `/tmp`). Both are released by the OS on process death, so a crash is reclaimed
   by the next launch. A diagnostic pid marker is written on acquire and removed/truncated on clean
   release; it reports `reclaimed_stale` but is never authoritative.
2. **Activation.** A local named pipe (Windows) or Unix socket (macOS/Linux) per user and application
   id. A length-prefixed JSON message (`version`, `command = activate`, optional `avatar` and `show`)
   is queued by a listener thread and drained by the main thread. The server acknowledges after it
   queues; the second launch exits 0 only after that acknowledgement. The running instance raises and
   focuses its window and shows `CFG-007` ("Avatar Studio is already running"). No network listener.
3. **`--new-instance`.** A distinct identity: the process id is appended to the lock, activation and
   shared-memory names, and `virtual_camera_enabled` / `output_registration_enabled` are false. The
   camera is not special-cased; opening a device the primary holds produces the existing `CAP-002`
   busy state. It is a testing escape hatch, not a user-facing second-avatar mode.
4. **Startup order.** The shell claims the lock before building the Live surface or opening a device.
   A secondary launch sends and exits without creating a window. A lock error is reported and the app
   continues without the single-instance guarantee rather than refusing to start.

## Alternatives considered

| Option | Why not |
|---|---|
| Let both instances start and rely on `CAP-002` | The second instance still registers output and confuses the user; the design requires activation, not a race |
| A TCP loopback port | Opens a network surface for a local UI action; blocked by firewalls and the security review |
| A window message (`FindWindow` + `WM_COPYDATA`) | Windows-only, fragile with Godot's window class, and does not carry a cross-platform contract |
| An `O_EXCL` lock file without an OS lock | A crash leaves the file behind; reclaiming it requires a pid check that is racy and can steal a live lock |
| A global (`Global\`) mutex | Needs cross-session semantics and privileges that a desktop app should not require; T58 owns the RDP matrix |
| `--new-instance` as a user feature | Two live avatars compete for one camera and one output device; explicitly out of scope |

## Consequences

Positive: a second launch cannot contend for the camera or the virtual camera; a crash needs no
reboot or manual cleanup; the activation path is testable without a second engine; the message is
local and carries optional avatar/show paths for T42.

Negative: `Local\` is per-session, so two RDP sessions for one user are not mutually exclusive yet
(T58). The Windows pipe uses the default DACL and a per-user name rather than an explicit security
descriptor; T58/T61 harden it. The diagnostic marker is best-effort and must never be used for
correctness. The `--new-instance` identity is a testing surface that must not become a product
feature.

Neutral: the activation channel is a second thread per process, idle until a message arrives. The
adapter keeps the queue and the window action on the main thread, so no scene state crosses threads.
