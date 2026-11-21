# Security and Privacy

**Status:** proposal · **Last updated:** 2026-11-20

The application's most sensitive input is a continuous live video feed of a person's face. The design
principle that follows is simple: **frames stay on the machine, in memory, and are never persisted or
transmitted unless the user explicitly asks for it.**

---

## Data inventory

| Data | Where it lives | Retention | Leaves the machine? |
|---|---|---|---|
| Camera frames | RAM only (frame ring, GPU textures) | Milliseconds | No |
| Tracking results (landmarks, pose) | RAM only | Milliseconds | No |
| Segmentation / matte masks | RAM only | Milliseconds | No |
| Avatar packages | Managed copy in the user data dir; originals stay where the user chose | Until deleted | Only if the user shares the file |
| FaceProfile (biometric landmark template) | User data dir, user-only permissions | Until deleted | No |
| Shows, outfits, settings | User data dir | Until deleted | No |
| Library index, thumbnails | User data dir | Rebuildable | No |
| Logs | User data dir, rotated | 7 days | Only in opt-in diagnostics |
| Crash reports | Not sent by default | — | Only with explicit opt-in, redacted |
| Debug recordings | User-chosen folder | Until deleted | No |

The storage layout, per-platform paths, permissions, and deletion rules are in
[`user-data.md`](user-data.md).

No account, no cloud, no analytics SDK, no ad SDK, no third-party network calls in the default path.
This is checkable: the release build's CI asserts that no unexpected network-capable dependency is
linked.

---

## Threat model

Assets ranked by what an attacker gains: the video feed (highest), code execution via a malicious
package (high), local privilege via the installer (medium), and metadata leakage (low).

| # | Threat | Vector | Mitigation |
|---|---|---|---|
| T1 | Exfiltration of camera frames | Malicious dependency, injected library, or a "helpful" feature that uploads | No network code in the capture path; dependency allow-list; no telemetry by default; frames never written to disk without an explicit action |
| T2 | Code execution via an avatar package | Crafted `.glb` (parser bug), path traversal, external URI fetch, decompression bomb | Strict importer rules, allow-listed glTF extensions, size caps, no external URIs, no symlinks; fuzzed glTF path |
| T3 | Local privilege escalation via the Windows virtual-camera installer | COM registration in `HKLM` as SYSTEM | Register only a signed binary from a fixed path; no user-controlled arguments to `regsvr32`; installer verifies signatures before writing |
| T4 | Another process opens the virtual camera and records the user silently | By design, any app can open a camera | OS-level camera indicators are respected; the app shows an "in use" indicator; users can uninstall the device from Settings; document that virtual cameras are visible to all apps |
| T5 | Malicious prop/model renders arbitrary shaders | Custom shader in glTF (not supported by glTF core, but extensions exist) | Shaders are never loaded from packages; only the app's own shader set is used |
| T6 | Log leakage of biometric data | Debug logs containing landmarks or frames | Logs contain counts, timings and codes only; landmark values are never logged |
| T7 | Update hijack | Tampered update artifact | Signed updates; signature verified before install; no downgrade unless signed |
| T8 | Supply-chain compromise of the tracker or transport libraries | Upstream compromise | Pinned versions, hash-pinned model files, reproducible builds where possible, dependency review at upgrade time |
| T9 | Denial of service via a hostile package | 500k-triangle model, 16k textures, cyclic node graph | Hard limits at import (triangles, texture size, node count, total size); loader timeout |
| T10 | Screen-share leakage of the user's real face | Preview shows the raw camera feed in calibration mode | Raw-feed overlay is opt-in, off by default, and never shown in Live mode |
| T11 | Another local process reads the virtual-camera frames | Named shared memory (Windows), Mach service + `IOSurface` (macOS), PipeWire node (Linux) | Per-user DACL on Windows (packaged-consumer caveat in the output doc); app-group entitlement plus code-signature validation on macOS; PipeWire session policy on Linux. Residual risk: a same-user process can read the frames, exactly as it can open a physical camera; documented, not hidden |
| T12 | FaceProfile leakage | World-readable file, backup/sync tool, diagnostics bundle | User-only permissions; never logged, exported, or included in diagnostics; explicit delete action; no cloud path |
| T13 | Camera keeps running while the screen is locked | Session lock leaves the app live | Stop capture and output on lock; emit no-signal; resume on unlock (`SEC-002`) |
| T14 | A foreign process injects frames into the virtual camera | Fake producer writing to the ring | Per-session nonce plus producer signature check; Windows requires administrator to register a competing media source |
| T15 | The real camera face, or a frozen face, reaches the stream | A passthrough or a hold-last-pose fallback | The output is the rendered avatar or a blank frame ([ADR-0010](adr/0010-output-is-avatar-or-nothing.md)). The decal is the only camera consumer and is masked; a lost face blanks within 150 ms. Enforced by the output gate and the no-passthrough test |

Explicit non-threats: a local attacker with debugger access, physical access to the machine, and
malicious GPU drivers are out of scope. A same-user process reading the camera is outside the
sandbox model on all three platforms and is stated as such in the privacy page.

---

## Untrusted avatar packages

A package is treated as hostile input. Enforcement points:

| Layer | Control |
|---|---|
| Archive | No symlinks, no absolute paths, no `..`, no NTFS alternate data streams, no hard links |
| glTF | Embedded buffers only; no external URIs; extension allow-list; node/depth limits; no animations driving the root transform |
| Textures | Max 8192², max 32 textures, decode with memory cap, no SVG |
| JSON | Schema-validated before use; strings length-capped; no HTML rendering of metadata |
| Shaders | Never loaded from packages |
| Scripts | Not a supported concept anywhere in the format |

Fuzzing: the glTF import path and the JSON loader are fuzzed in CI (libFuzzer, 30 min nightly per
target). A crash in either is a release blocker, not a bug.

---

## Dependency and asset manifest

A single machine-readable manifest (`native/licenses.json`, the source of truth, plus a generated
`NOTICE`) records, for every third-party component and every bundled asset:

- name, version/commit, license SPDX id, upstream URL, whether it is modified, and its hash where
  relevant.

CI fails when:

- a new dependency appears without a manifest entry,
- a license is on the deny list (GPL/AGPL in the shipped binary, anything non-commercial),
- a bundled asset lacks provenance,
- a model weight file's hash does not match.

The deny list has one deliberate exception process: a GPL component may be used as a *separate
process* (never linked) with legal sign-off, recorded in an ADR.

**Implemented (T06).** `scripts/check-licenses.mjs` is the build gate. It scans C++ include directives:
an angle-bracket third-party include with no matching `include_prefixes` entry fails, as does a
manifest entry missing name/version/licence/url/modified or carrying a licence on the deny list.
Reviewed exceptions are `{id, reason, adr}` entries in `policy.exceptions`; the gate warns and allows
only those. Standard-library and platform headers are allow-listed in the script so a new SDK header is
a reviewed change. The manifest currently records nlohmann/json, Godot and godot-cpp as active, and
Eigen, MediaPipe, ONNX Runtime, Spout2 and Syphon as planned. CTest runs the gate as
`license_manifest_check`; `scripts/ci.ps1|sh` runs it before configure. Asset provenance and model
hashes remain later tasks (`assets.json`, tracker model checks).

---

## Privileges

| Platform | What needs elevation | What we do |
|---|---|---|
| Windows | `HKLM` COM registration for the virtual-camera media source | Per-machine installer, signed payload, fixed paths, explicit consent screen listing exactly what is written |
| macOS | System extension installation approval | In-app flow, `SystemExtensions` framework, user approves in System Settings; no admin password required for the extension itself |
| Linux | `v4l2loopback` module (fallback only) | `pkexec` prompt with explanation; the default PipeWire path needs no privileges at all |

The app never asks for administrator rights to *run*. Elevation is confined to installation of an
optional output device, and the app is fully functional without it. The exact files, registry keys,
and services written by each installer are in
[`architecture.md`](architecture.md#install-and-uninstall-contract).

---

## Network policy

| Feature | Network use |
|---|---|
| Core app | None |
| Update check | HTTPS, opt-out, no identifiers beyond version and OS |
| Crash reporting | Opt-in, redacted, HTTPS |
| Avatar library browsing | Not in 1.0 (no store, no downloads) |
| Model weights | Bundled with the installer, not downloaded at runtime |

If a future feature needs the network, it must be off by default and documented here.

---

## Logging and diagnostics

- Log levels: error/warn/info/debug. Debug is off in release unless the user enables it.
- Never logged: frame data, landmark coordinates, FaceProfile contents, file contents, window titles
  of other apps, IP addresses.
- Always logged: app/engine/tracker versions, OS build, GPU and driver, camera device name and
  negotiated format, transport states, error codes, timing histograms.
- Diagnostics export is a single archive the user can inspect before sending. It contains logs,
  settings (camera device *name* only), and a hardware summary. It never contains frames.
- Debug video recording is explicit, time-boxed (max 60 s per capture), stored in a visible folder,
  and never attached automatically.

---

## Updates and signing

| Platform | Signing |
|---|---|
| Windows | Authenticode on the app and the media source DLL; installer signed |
| macOS | Developer ID, hardened runtime, notarised; extension signed with the same team id |
| Linux | Signed AppImage (embedded signature) and distro packages; checksums published |

Auto-update is `Planned` for 1.1. In 1.0, updates are manual downloads with signature verification
on install. Shipping an auto-updater is a security commitment, not a convenience feature, and is not
worth doing badly in the first release.

---

## Privacy commitments (user-facing)

These become the privacy page in-app and the README section:

1. Your camera feed never leaves your computer.
2. Nothing is recorded unless you press record.
3. No account, no analytics, no ads.
4. Diagnostics are opt-in and never include images.
5. Your face profile stays on this machine and can be deleted at any time.
6. Capture stops when your screen is locked.
7. The output is the rendered avatar or nothing. Your camera is never passed through, and a lost
   face blanks the output rather than freezing it.
8. You can delete everything the app stores from Settings → Privacy.

---

## Incident response

- A `SECURITY.md` with a private reporting channel and a 90-day disclosure window.
- A CVE process for the app and for the media-source/extension components.
- A documented "disable the virtual camera" emergency path that does not require an update: the
  app can unregister its device and the installer can remove it.

---

## Open questions

| Question | Resolution path |
|---|---|
| Does the macOS extension need its own privacy manifest (`PrivacyInfo.xcprivacy`)? | Confirm against current App Store requirements before M4 |
| Can we detect another app recording the virtual camera, to warn the user? | OS-dependent; likely no. Document the limitation instead |
| Is per-user COM registration viable via MSIX packaged COM, avoiding `HKLM`? | M4 spike; would materially improve install UX |
| Can the Windows DACL be scoped to the app's package SID for packaged consumers? | M4 test with an AppContainer consumer; if not, document the limitation |
| Does the macOS code-signature check add measurable latency to the first frame? | M4 measurement; cache the validation result per connection |
