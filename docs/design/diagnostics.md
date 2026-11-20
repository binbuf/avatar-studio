# Diagnostics and Support Codes

**Status:** proposal · **Last updated:** 2026-11-20

Every failure in this application has a stable code, a human sentence, and one recovery action.
Codes are the join between the UI, the log, the test suite, and a support conversation: a user can
report `TRK-002` without recording a video, and a maintainer can find the exact state machine
transition that produced it.

This document is the registry. The application's string table is keyed by these codes.

---

## Rules

1. **Format:** `AREA-NNN`, three digits, zero-padded. Areas are fixed and listed below.
2. **Stable forever.** A code's meaning never changes. A retired code is marked `Deprecated` and
   never reused.
3. **No bare codes in the UI.** The user sees `TRK-002` only alongside a sentence and an action.
4. **One code per state, not per exception.** An OS error maps to the user-visible state; the raw
   HRESULT/errno is attached as a log detail.
5. **Every code is emitted from one place.** A subsystem owns its range; the registry, the enum, and
   the string table are checked against each other in CI.
6. **Codes are not severity.** Severity is a separate field, so the same condition can be a warning
   in one build and an error in another.
7. **New code, new row.** A change that adds a failure path adds a registry row in the same commit.

Areas:

| Prefix | Subsystem | Owner |
|---|---|---|
| `CAP` | Camera capture | capture |
| `TRK` | Tracking backend | tracking |
| `RIG` | Face frame, filters, calibration | rig |
| `AVT` | Avatar package, import, library | content |
| `OUT` | Output manager and transports | output |
| `VCAM` | OS virtual camera | platform |
| `GPU` | Renderer and readback | rendering |
| `CFG` | Settings, profiles, user data | app |
| `SEC` | Security, signing, permissions | security |

Severity values: `Info`, `Warning`, `Error`, `Fatal`. `Fatal` means the affected feature stops;
the app itself keeps running.

---

## CAP — Capture

| Code | Severity | Meaning | Recovery |
|---|---|---|---|
| `CAP-001` | Error | Camera disappeared mid-stream | Hold last frame; poll for return; auto-reopen |
| `CAP-002` | Error | Camera is busy (exclusive access) | Name likely owners; Retry |
| `CAP-003` | Warning | Requested format rejected; stepped down | Show negotiated mode; continue |
| `CAP-004` | Error | Stream stalled without an OS error | Restart once; then report and stop |
| `CAP-005` | Warning | Timestamps went backwards or repeated | Reset ring; re-map to arrival time; log once |
| `CAP-006` | Error | Camera permission denied by the OS | Deep-link to the OS privacy page |
| `CAP-007` | Warning | Device returned but reopen failed | Retry with previous config; fall back to device picker |
| `CAP-008` | Info | No camera found | Rescan; open settings |

## TRK — Tracking

| Code | Severity | Meaning | Recovery |
|---|---|---|---|
| `TRK-001` | Info | No face in frame | Hint about lighting/position; hold last pose |
| `TRK-002` | Warning | Face lost beyond timeout (250 ms) | Hold pose; decay to neutral; freeze decal |
| `TRK-003` | Error | Tracker process/library crashed | Disable decals; render static face; offer restart |
| `TRK-004` | Warning | Tracking slower than the camera | Drop frames before inference; suggest quality step |
| `TRK-005` | Warning | Duplicate or non-monotonic timestamp | Increment by 1 ms; log once; reset graph on reconnect |
| `TRK-006` | Error | Model file missing or hash mismatch | Refuse to start tracking; name the file and expected hash |
| `TRK-007` | Fatal | Backend initialization failed | Report; disable tracking; keep the editor usable |

## RIG — Face frame and calibration

| Code | Severity | Meaning | Recovery |
|---|---|---|---|
| `RIG-001` | Warning | Homography solve failed | Reuse previous `H` with damped extrapolation; lower confidence |
| `RIG-002` | Warning | Inlier count below threshold | Hold last pose; do not emit a jump |
| `RIG-003` | Warning | FaceProfile invalid or missing required names | Create a provisional profile; prompt recalibration |
| `RIG-004` | Error | Degenerate face frame (collinear or zero scale) | Hold; ask the user to face the camera |
| `RIG-005` | Info | Calibration quality below target | Offer recalibration; do not block saving |

## AVT — Avatar packages and library

| Code | Severity | Meaning | Recovery |
|---|---|---|---|
| `AVT-101` | Error | Referenced file missing or outside the package | Show JSON path; refuse to load |
| `AVT-102` | Error | `model.glb` unparseable, no mesh, or over 200k triangles | Show the limit; refuse |
| `AVT-103` | Error | Invalid face surface (scale ≤ 0, parallel basis vectors) | Point at the field; refuse |
| `AVT-104` | Error | Region outside the allowed canonical range | Clamp or refuse with the field path |
| `AVT-105` | Error | Duplicate region id or missing `eye_l`/`eye_r`/`mouth` | Refuse |
| `AVT-106` | Error | Prop references an unknown anchor | Refuse |
| `AVT-107` | Error | Prop asset unparseable or outside `props/` | Refuse |
| `AVT-108` | Warning | `assets.json` does not cover every distributed binary | Warn; allow local use |
| `AVT-109` | Error | Package or single GLB exceeds the size limit | Refuse |
| `AVT-110` | Error | Library import copy failed | Remove `.incoming-*`; retry; check disk |
| `AVT-111` | Info | Duplicate package id on import | Replace / Keep both / Cancel |
| `AVT-112` | Error | Package rejected by the safety rules | Name the rule; refuse |
| `AVT-113` | Error | Invalid framing values | Point at the field; refuse |
| `AVT-114` | Error | Invalid background type, path, or decode limit | Point at the field; refuse |
| `AVT-115` | Error | Invalid appearance preset or override value | Point at the field; refuse |
| `AVT-120` | Error | Package version newer than the app | Name the app version that wrote it |

## OUT — Output and transports

| Code | Severity | Meaning | Recovery |
|---|---|---|---|
| `OUT-001` | Error | Transport initialization failed | Show transport name and platform hint |
| `OUT-002` | Warning | Consumer disconnected | Stop readback after grace; resume on connect |
| `OUT-003` | Warning | Async readback failed | Fall back to synchronous at reduced rate; log once |
| `OUT-004` | Error | Output resolution change failed | Restore previous size; restart transports |
| `OUT-005` | Warning | Requested alpha mode unsupported by transport | Use the transport default; explain the fringe risk |
| `OUT-006` | Info | No consumer is listening | Suppress readback; show idle state |

## VCAM — Virtual camera

| Code | Severity | Meaning | Recovery |
|---|---|---|---|
| `VCAM-001` | Error | Registration failed (permissions or OS version) | Explain admin/Windows 11 requirement; offer OBS fallback |
| `VCAM-002` | Warning | macOS extension not approved | Deep-link to System Settings; detection + repair |
| `VCAM-003` | Warning | Stale device found after a crash | Offer *Repair camera*; unregister/re-register |
| `VCAM-004` | Info | Producer absent; device is emitting no-signal | Show placeholder; start the app |
| `VCAM-005` | Error | Shared-memory layout version mismatch | Refuse to connect; name both versions |
| `VCAM-006` | Error | Transport access denied (DACL/entitlement) | Report the security descriptor/entitlement; do not fall back silently |
| `VCAM-007` | Error | Install or uninstall of the camera component failed | Roll back; show the exact path/registry key |

## GPU — Renderer and readback

| Code | Severity | Meaning | Recovery |
|---|---|---|---|
| `GPU-001` | Error | Graphics device lost/reset | Suspend output; rebuild viewport; resume |
| `GPU-002` | Fatal | Renderer unsupported (Compatibility/OpenGL) | Explain Forward+/Vulkan/Metal; offer to switch |
| `GPU-003` | Error | Readback unavailable on this device | Disable output; explain; keep preview |
| `GPU-004` | Error | Shader compilation failed | Disable decals; keep the avatar; report driver |

## CFG — Settings, profiles, user data

| Code | Severity | Meaning | Recovery |
|---|---|---|---|
| `CFG-001` | Warning | Settings or index parse failed | Rebuild from defaults/directory; keep a `.bad` copy |
| `CFG-002` | Error | Atomic write failed (disk full, permissions) | Keep the old file; name the path; retry |
| `CFG-003` | Error | Data directory not writable | Offer a different `--data-dir`; explain permissions |
| `CFG-004` | Error | File version newer than the app | Refuse; name the app version |
| `CFG-005` | Warning | FaceProfile corrupt or incomplete | Create provisional profile; prompt recalibration |
| `CFG-006` | Warning | Migration failed | Restore from `backups/`; keep the original |

## SEC — Security

| Code | Severity | Meaning | Recovery |
|---|---|---|---|
| `SEC-001` | Fatal | Update signature invalid | Refuse to install; delete the artifact |
| `SEC-002` | Warning | Session locked; capture and output paused | Resume on unlock; explain in the UI |
| `SEC-003` | Error | Diagnostics export failed | Keep the bundle local; name the path |
| `SEC-004` | Fatal | Package attempted an external URI or executable content | Refuse; log the package id, not the content |

---

## Logging contract

Every log record carries:

- `timestamp` (FrameClock ns, wall clock for display only),
- `level`, `code`, `subsystem`,
- `app`, `engine`, `tracker`, `OS build`, `GPU`/driver on startup,
- camera device name and negotiated format, transport state,
- a correlation id for the session.

Never logged: frame data, landmark coordinates, face profile contents, file contents, window titles
of other applications, IP addresses, or the user's name. The redaction rules are enforced by the
logging API, not by reviewer discipline.

At `Info` and above, a code is emitted once per state transition, not once per frame. A repeated
condition increments a counter and is summarised at the end of the session.

---

## Diagnostics bundle

*Export diagnostics* produces a single archive the user can inspect before sending:

```
diagnostics-<date>-<id>/
├── summary.json          # versions, OS, GPU, camera names, transport states
├── settings.json         # redacted: device names only, no paths outside app data
├── logs/                 # rotated logs, codes and timings
├── metrics.json          # latency/frame-time histograms, dropped-frame counters
└── README.txt            # what is included, what is not
```

It never contains frames, recordings, landmarks, profiles, avatar files, or credentials. The export
is a local file; the app does not upload it.

---

## CI enforcement

- `native/core/diagnostics.h` is the enum and metadata source of truth.
- The registry tables in this document are generated from the header and compared in CI; drift
  fails the build.
- A source scan rejects a literal `AREA-NNN` that is not in the enum, and a log call without a code.
- The string table must contain a message and an action for every code.
- Unit tests assert that every code has a unique value and a non-empty message.

---

## Open questions

| Question | Resolution path |
|---|---|
| Should codes be localized, or stay English in logs and UI? | Keep codes language-neutral; localize only the sentence. Confirm in M2 |
| Do we need a numeric severity for telemetry-free crash counters? | M5; local counters only |
| Is one code per state enough for the macOS extension's multi-step approval flow? | M4; split if the support burden proves it |
