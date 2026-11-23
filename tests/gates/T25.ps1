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

    $tests = (& ctest --test-dir $BuildDir -C $Config -N -R '^(avatar_live_unit|avatar_output_unit)$') -join "`n"
    if ($tests -notmatch 'Total Tests: 2') { throw 'T25 unit suites are missing' }

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

    $states = @{
        camera_lost = @('CAP-001', 'Camera disconnected', 'Reconnect')
        face_not_visible = @('TRK-001', "Can't see your face", 'lighting')
        features_hidden = @('TRK-008', "Can't see your eyes and mouth", 'sunglasses')
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

    $uxDoc = Get-Content docs/design/editor-ux.md -Raw
    if ($uxDoc -notmatch 'Implemented Live surface \(T25\)') {
        throw 'editor-ux.md does not record the realised Live surface'
    }
    $faceDoc = Get-Content docs/design/face-compositing.md -Raw
    if ($faceDoc -notmatch 'Panic blank \(T25\)') {
        throw 'face-compositing.md does not record the panic blank'
    }
    $qualityDoc = Get-Content docs/design/testing-and-quality.md -Raw
    if ($qualityDoc -notmatch 'T25 Live surface') {
        throw 'testing-and-quality.md does not record the T25 tests'
    }
    if (-not (Test-Path -LiteralPath docs/design/adr/0035-live-hud-panic-and-edit-guard.md)) {
        throw 'Missing ADR-0035'
    }

    & node scripts/check-design-links.mjs
    if ($LASTEXITCODE -ne 0) { throw 'Design-link check failed' }
    & powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format-check.ps1 -Require
    if ($LASTEXITCODE -ne 0) { throw 'Formatting check failed' }
    Write-Host 'T25 gate: PASS (Live state, panic blank, restore, hotkey, edit guard, four states, 100/200% layout)'
} finally { Pop-Location }

