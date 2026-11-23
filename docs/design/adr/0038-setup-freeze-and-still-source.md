# ADR-0038 - Diagnostic freeze and still-image source for Setup

- **Status:** Accepted
- **Date:** 2026-11-23
- **Supersedes:** -
- **Related:** [editor-ux.md](../editor-ux.md), [camera-capture.md](../camera-capture.md),
  [face-compositing.md](../face-compositing.md)

## Context

The T25c Setup window needs live three-pane mapping work, but the operator cannot always sit in
front of the camera and the automated suite must run without a camera or person. The production
path must stay the real camera and the OBS output must never show a camera frame, a still image or a
frozen diagnostic pose. The 500 ms tracker-result timeout and the 150 ms FaceRig feature-loss hold
would otherwise turn a held still into `no face`.

## Decision

1. **Freeze in the framework-free live pipeline.** `avatar::live::LivePipeline::set_frozen(true)`
   stops producing, submitting and polling, returns the last `FaceRigOutput` verbatim, and maps the
   held tracker result without the 500 ms timeout. `FaceRig::sample` is not called, so the 150 ms
   hold cannot expire. `AvatarLivePipeline::set_frozen` captures one RGB copy of the newest source
   frame for the snapshot and keeps the last ring slot and preview pin.
2. **Snapshot is a development action.** The Setup window writes the captured frame to
   `user://T25c-snapshot.png` (and a `.tools/` copy) and labels the source `still (dev)`. `Use
   camera` clears the freeze; a frozen camera does not reopen the device.
3. **The OBS gate blanks while frozen.** The HUD feeds the Live session `face not_visible` while the
   diagnostic surface is frozen, so the separate output gate cannot show the held pose or the
   still. The Setup final pane samples the avatar viewport directly and still shows the held pose
   for mapping.
4. **A separate still-file source.** `StillImageSource` publishes a decoded RGB image as camera
   frames at the pipeline fps. `--live-source still --still <path>` and `Load still...` use it and
   label the source `still (file)` with its path.
5. **Neither source is production evidence.** The OBS output never samples them and they are not
   used for the M1 gate. The human review and the capture measurements use the real camera.

## Consequences

- The setup screen can be exercised headless and without a person; the gate runs the Setup test in
  synthetic, still and rendered modes.
- The freeze holds a pose indefinitely, so the operator must remember `Use camera`; the status
  banner says `FROZEN` and the OBS output is blank meanwhile.
- The snapshot converts NV12/BGRA to RGB on the Snapshot press only, not on the frame path.
- The still source and freeze are extra code in the diagnostic path; the production camera and OBS
  transports are unchanged.

## Follow-ups

- The human review (T25c HITL) still uses the real C920 and a person; the still source is not a
  substitute.
- T34 replaces the temporary mapping sliders with the calibration gizmo; the freeze and still
  sources can remain as diagnostics.
