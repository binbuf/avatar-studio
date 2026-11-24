#requires -Version 5.1
<#
T25: Live HUD, panic blank and edit guard.

The native suite owns the Live state machine, the four degraded-state sentences
and the output gate panic. The headless Godot phase drives the real main scene:
HUD numbers, Ctrl+Shift+H, the window output blank, restore, edit refusal and
the debug state triggers. The rendered phase checks the surface at 100% and 200%
UI scale and captures local evidence.
#>
[CmdletBinding()]
param([string]$BuildDir = 'build', [string]$Config = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $root
try {
    & ctest --test-dir $BuildDir -C $Config -R '^avatar_live_unit$' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Live session unit suite failed' }
    & ctest --test-dir $BuildDir -C $Config -R '^avatar_output_unit$' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Output gate unit suite failed' }
    & ctest --test-dir $BuildDir -C $Config -R '^avatar_capture_unit$' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Capture/StillImageSource unit suite failed' }

    $tests = (& ctest --test-dir $BuildDir -C $Config -N -R '^(avatar_live_unit|avatar_output_unit|avatar_capture_unit)$') -join "`n"
    if ($tests -notmatch 'Total Tests: 3') { throw 'T25 unit suites are missing' }

    $godot = Join-Path $root '.tools/godot/Godot_v4.7.2-stable_win64_console.exe'
    if (-not (Test-Path -LiteralPath $godot)) { throw 'Pinned Windows Godot required for T25' }

    if (Test-Path -LiteralPath .tools/T25-live.json) {
        Remove-Item -LiteralPath .tools/T25-live.json
    }
    & $godot --headless --path app --script res://tests/live_hud.gd --quit-after 60000 -- --live-source synthetic --live-tracker scripted
    if ($LASTEXITCODE -ne 0) { throw 'Live HUD headless contract failed' }
    $headless = Get-Content .tools/T25-live.json -Raw | ConvertFrom-Json
    if (-not $headless.passed -or $headless.rendered) { throw 'Incomplete headless Live evidence' }
    if (-not $headless.live) { throw 'Live did not start' }
    if ([double]$headless.hud.fps -le 0.0) { throw 'Actual fps was not reported' }
    if ([double]$headless.hud.elapsed_s -le 0.0) { throw 'Elapsed time did not advance' }
    if ([double]$headless.hud.confidence -le 0.0) { throw 'Tracking confidence was not reported' }
    if ([string]$headless.hud.face_state -ne 'tracked') { throw 'Face state is not tracked' }
    if ([string]$headless.hotkey.detail -eq '') { throw 'Global hotkey status has no sentence' }
    if (-not [bool]$headless.hotkey.registered -and
        -not ([string]$headless.hotkey.detail -match 'in-app')) {
        throw 'Unavailable global hotkey does not name the in-app fallback'
    }
    if (-not [bool]$headless.panic.blank) { throw 'Panic output was not blank' }
    if (-not [bool]$headless.panic.restored) { throw 'Panic did not restore' }
    if ([string]$headless.panic.code -ne 'OUT-007') { throw 'Panic banner code is missing' }
    if (-not [bool]$headless.edit_guard.refused) { throw 'Live edit refusal was not recorded' }
    if (-not [bool]$headless.edit_guard.allowed_after_stop) {
        throw 'Edit was not allowed after Stop'
    }
    if (-not [bool]$headless.edit_guard.restarted) { throw 'Go Live did not restart' }

    # ---- T25c Setup window: three panes, freeze and still source --------
    if (Test-Path -LiteralPath .tools/T25c-setup.json) {
        Remove-Item -LiteralPath .tools/T25c-setup.json
    }
    & $godot --headless --path app --script res://tests/setup_window.gd --quit-after 60000 -- --live-source synthetic --live-tracker scripted
    if ($LASTEXITCODE -ne 0) { throw 'Setup window headless test failed' }
    $setup = Get-Content .tools/T25c-setup.json -Raw | ConvertFrom-Json
    if (-not $setup.passed -or $setup.rendered) { throw 'Incomplete headless Setup evidence' }
    foreach ($pane in @('camera', 'mapping', 'final', 'status', 'controls')) {
        if ($null -eq $setup.panes.$pane) { throw "Missing Setup pane: $pane" }
    }
    if (-not [bool]$setup.overlay.has_frame) { throw 'Camera pane has no raw frame' }
    if ([int]$setup.overlay.landmark_count -le 0) { throw 'Camera pane has no landmarks' }
    if ([int]$setup.overlay.quad_points -ne 4) { throw 'Camera pane has no homography quad' }
    if ([int]$setup.overlay.anchor_count -ne 3) { throw 'Camera pane has no region anchors' }
    if ([int]$setup.mapping.region_count -ne 3) { throw 'Mapping pane has no regions' }
    if ([double]$setup.mapping.head_distance -le 0.0) { throw 'Mapping pane has no head cut' }
    if ([double]$setup.mapping.head_falloff -le 0.0) { throw 'Mapping pane has no falloff' }
    if ([double]$setup.mapping.face_scale -le 0.0) { throw 'Mapping pane has no face scale' }
    if (-not [bool]$setup.persist.file -or -not [bool]$setup.persist.loaded -or
        -not [bool]$setup.persist.reloaded) { throw 'Calibration did not apply and persist' }
    if (-not [bool]$setup.hold.setup_holds -or -not [bool]$setup.hold.expires) {
        throw 'Feature-loss hold did not hold then expire'
    }
    if ([string]$setup.hold.obs_blank -ne 'blank') { throw 'OBS gate did not blank on loss' }
    if (-not [bool]$setup.background.white -or -not [bool]$setup.background.black) {
        throw 'Final pane background toggle is incomplete'
    }
    if ([int]$setup.selectors.capture_items -ne 4) { throw 'Capture selector is missing modes' }
    if ([int]$setup.selectors.tracker_items -ne 2) { throw 'Tracker selector is missing inputs' }
    if ([string]$setup.source.kind -ne 'synthetic' -or
        [string]$setup.source.label -ne 'synthetic (dev)') {
        throw 'Setup source label is wrong for the synthetic run'
    }
    if ([string]$setup.freeze.held_face -ne [string]$setup.freeze.face) {
        throw 'Frozen pose did not hold the face state'
    }
    if ([math]::Abs([double]$setup.freeze.held_confidence -
            [double]$setup.freeze.confidence) -gt 0.02) {
        throw 'Frozen pose did not hold the confidence'
    }
    if ([int64]$setup.freeze.held_timestamp -ne [int64]$setup.freeze.timestamp) {
        throw 'Frozen pose did not hold the source timestamp'
    }
    if ([string]$setup.freeze.obs_frozen -ne 'blank') { throw 'Frozen diagnostic reached the OBS output' }
    if ([string]$setup.freeze.obs_resumed -ne 'avatar') { throw 'OBS output did not resume after unfreeze' }

    $stillFixture = Join-Path $root 'tests/fixtures/images/still_setup.png'
    if (-not (Test-Path -LiteralPath $stillFixture)) { throw 'Missing still fixture image' }
    if (Test-Path -LiteralPath .tools/T25c-setup-still.json) {
        Remove-Item -LiteralPath .tools/T25c-setup-still.json
    }
    & $godot --headless --path app --script res://tests/setup_window.gd --quit-after 60000 -- --live-source still --still $stillFixture --live-tracker scripted
    if ($LASTEXITCODE -ne 0) { throw 'Setup window still-source headless test failed' }
    $still = Get-Content .tools/T25c-setup-still.json -Raw | ConvertFrom-Json
    if (-not $still.passed) { throw 'Still-source Setup run failed' }
    if ([string]$still.source.kind -ne 'still') { throw 'Still source did not load' }
    if ([string]$still.source.label -ne 'still (file)') { throw 'Still source label is wrong' }
    if ([string]$still.source.still_image -eq '') { throw 'Still source path is not reported' }
    if (-not [bool]$still.overlay.has_frame) { throw 'Still source produced no camera frame' }
    if ([string]$still.freeze.held_face -ne [string]$still.freeze.face) {
        throw 'Still-source frozen pose did not hold the face state'
    }
    if ([string]$still.freeze.obs_frozen -ne 'blank') { throw 'Still frozen frame reached the OBS output' }

    # The Snapshot action must leave a reloadable still. The synthetic run above
    # wrote .tools/T25c-snapshot.png; load it through the still source.
    $snapshotStill = Join-Path $root '.tools/T25c-snapshot.png'
    if (-not (Test-Path -LiteralPath $snapshotStill)) {
        throw 'Snapshot did not write .tools/T25c-snapshot.png'
    }
    if (Test-Path -LiteralPath .tools/T25c-setup-snapshot.json) {
        Remove-Item -LiteralPath .tools/T25c-setup-snapshot.json
    }
    & $godot --headless --path app --script res://tests/setup_window.gd --quit-after 60000 -- --live-source still --still $snapshotStill --live-tracker scripted --result-tag=snapshot
    if ($LASTEXITCODE -ne 0) { throw 'Reloaded-snapshot Setup test failed' }
    $snapshot = Get-Content .tools/T25c-setup-snapshot.json -Raw | ConvertFrom-Json
    if (-not $snapshot.passed) { throw 'Reloaded-snapshot Setup run failed' }
    if ([string]$snapshot.source.kind -ne 'still') { throw 'Snapshot did not reload as a still source' }
    if (-not [bool]$snapshot.overlay.has_frame) { throw 'Reloaded snapshot produced no frame' }

    $states = @{
        camera_lost = @('CAP-001', 'Camera disconnected', 'Reconnect')
        face_not_visible = @('TRK-001', "Can't see your face", 'lighting')
        features_hidden = @('TRK-008', "Can't see your mouth or both eyes", 'sunglasses')
        degraded = @('TRK-004', 'Face tracking is struggling', 'resolution')
        tracker_unavailable = @('TRK-007', "Face tracking isn't running", 'tracker')
    }
    foreach ($name in $states.Keys) {
        $state = $headless.states.$name
        if ($null -eq $state) { throw "Missing Live state evidence: $name" }
        if ([string]$state.code -ne $states[$name][0]) { throw "$name has the wrong code" }
        if ([string]$state.sentence -ne $states[$name][1]) { throw "$name has the wrong sentence" }
        if (-not ([string]$state.action -match [regex]::Escape($states[$name][2]))) {
            throw "$name has the wrong action"
        }
        if ($name -eq 'degraded') {
            if ([string]$state.output -ne 'avatar') { throw 'Degraded state blanked a valid avatar' }
        } elseif ([string]$state.output -ne 'blank') {
            throw "$name did not blank the output"
        }
    }

    if (Test-Path -LiteralPath .tools/T25-live-render.json) {
        Remove-Item -LiteralPath .tools/T25-live-render.json
    }
    $output = Join-Path $root '.tools/T25-render.log'
    $errors = Join-Path $root '.tools/T25-render-error.log'
    $process = Start-Process -FilePath $godot -WindowStyle Hidden -ArgumentList @(
        '--path', 'app', '--resolution', '1280x720',
        '--script', 'res://tests/live_hud.gd', '--quit-after', 60000, '--', '--render', '--live-source', 'synthetic', '--live-tracker', 'scripted'
    ) -RedirectStandardOutput $output -RedirectStandardError $errors -PassThru -Wait
    Get-Content -LiteralPath $output | Write-Host
    $errorText = Get-Content -LiteralPath $errors -Raw
    if ($errorText) { Write-Host $errorText }
    if ($process.ExitCode -ne 0 -or
        $errorText -match '(?im)(SCRIPT ERROR|SHADER ERROR|^ERROR:|FAIL:)') {
        throw 'Live HUD rendered phase failed'
    }
    $render = Get-Content .tools/T25-live-render.json -Raw | ConvertFrom-Json
    if (-not $render.passed -or -not $render.rendered) {
        throw 'Incomplete Live HUD render evidence'
    }
    if ($null -eq $render.layout.'100' -or $null -eq $render.layout.'200') {
        throw 'UI scale layout evidence is missing'
    }
    if ($null -eq $render.layout.'200_banner') {
        throw 'UI scale banner layout evidence is missing'
    }
    foreach ($scale in @('100', '200', '200_banner')) {
        if ([string]$render.layout.$scale.top -eq '' -or [string]$render.layout.$scale.bottom -eq '') {
            throw "UI layout is missing at $scale"
        }
    }
    foreach ($shot in @('.tools/T25-live-100.png', '.tools/T25-live-200.png',
            '.tools/T25-live-200-banner.png')) {
        if (-not (Test-Path -LiteralPath $shot)) { throw "Missing render evidence: $shot" }
    }

    # ---- T25c rendered Setup window (100% and 200% UI scale) -----------
    if (Test-Path -LiteralPath .tools/T25c-setup-render.json) {
        Remove-Item -LiteralPath .tools/T25c-setup-render.json
    }
    $setupOut = Join-Path $root '.tools/T25c-setup-render.log'
    $setupErr = Join-Path $root '.tools/T25c-setup-render-error.log'
    $setupProcess = Start-Process -FilePath $godot -WindowStyle Hidden -ArgumentList @(
        '--path', 'app', '--resolution', '1280x720',
        '--script', 'res://tests/setup_window.gd', '--quit-after', 120000, '--',
        '--render', '--live-source', 'synthetic', '--live-tracker', 'scripted'
    ) -RedirectStandardOutput $setupOut -RedirectStandardError $setupErr -PassThru -Wait
    Get-Content -LiteralPath $setupOut | Write-Host
    $setupErrorText = Get-Content -LiteralPath $setupErr -Raw
    if ($setupErrorText) { Write-Host $setupErrorText }
    if ($setupProcess.ExitCode -ne 0 -or
        $setupErrorText -match '(?im)(SCRIPT ERROR|SHADER ERROR|^ERROR:|FAIL:)') {
        throw 'Rendered Setup window phase failed'
    }
    $setupRender = Get-Content .tools/T25c-setup-render.json -Raw | ConvertFrom-Json
    if (-not $setupRender.passed -or -not $setupRender.rendered) {
        throw 'Incomplete rendered Setup evidence'
    }
    foreach ($scale in @('100', '200')) {
        if ($null -eq $setupRender.layout.$scale) {
            throw "Setup layout evidence is missing at $scale"
        }
    }
    foreach ($shot in @('.tools/T25c-setup-100.png', '.tools/T25c-setup-200.png',
            '.tools/T25c-setup-white.png', '.tools/T25c-setup-black.png')) {
        if (-not (Test-Path -LiteralPath $shot)) { throw "Missing Setup render evidence: $shot" }
    }

    $uxDoc = Get-Content docs/design/editor-ux.md -Raw
    if ($uxDoc -notmatch 'Implemented Live surface \(T25\)') {
        throw 'editor-ux.md does not record the realised Live surface'
    }
    if ($uxDoc -notmatch 'Setup window \(T25c\)') {
        throw 'editor-ux.md does not record the Setup window'
    }
    $faceDoc = Get-Content docs/design/face-compositing.md -Raw
    if ($faceDoc -notmatch 'Panic blank \(T25\)') {
        throw 'face-compositing.md does not record the panic blank'
    }
    if ($faceDoc -notmatch 'T25c diagnostic freeze') {
        throw 'face-compositing.md does not record the diagnostic freeze'
    }
    $qualityDoc = Get-Content docs/design/testing-and-quality.md -Raw
    if ($qualityDoc -notmatch 'T25 Live surface') {
        throw 'testing-and-quality.md does not record the T25 tests'
    }
    if ($qualityDoc -notmatch 'T25c Setup window') {
        throw 'testing-and-quality.md does not record the T25c tests'
    }
    $captureDoc = Get-Content docs/design/camera-capture.md -Raw
    if ($captureDoc -notmatch 'T25c') {
        throw 'camera-capture.md does not record the still/freeze sources'
    }
    $mapDoc = Get-Content docs/design/implementation-map.md -Raw
    if ($mapDoc -notmatch 'T25c') {
        throw 'implementation-map.md does not record T25c'
    }
    if (-not (Test-Path -LiteralPath docs/design/adr/0035-live-hud-panic-and-edit-guard.md)) {
        throw 'Missing ADR-0035'
    }
    if (-not (Test-Path -LiteralPath docs/design/adr/0038-setup-freeze-and-still-source.md)) {
        throw 'Missing ADR-0038'
    }

    & node scripts/check-design-links.mjs
    if ($LASTEXITCODE -ne 0) { throw 'Design-link check failed' }
    & powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format-check.ps1 -Require
    if ($LASTEXITCODE -ne 0) { throw 'Formatting check failed' }
    Write-Host 'T25 gate: PASS (Live state, panic blank, restore, hotkey, edit guard, four states, 100/200% layout)'
} finally { Pop-Location }

