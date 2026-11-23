#requires -Version 5.1
<#
T26: M1 golden suite, latency instrumentation, and the app soak.

The native suite owns the framework-free live pipeline (camera -> tracker ->
FaceRig -> paired timestamp and the capture-to-submit histogram). The rendered
phase builds the reviewed golden matrix through the real surface decal and
compares it with SSIM >= 0.995 plus a decal-bbox max-delta guard. The soak runs
the real main scene at 1080p60 and writes the T05-shaped JSON; T27.ps1 applies
the thresholds independently of the harness verdict.

-Fast runs a 20 s soak (plumbing only). -Full runs the documented 10 minutes.
#>
[CmdletBinding()]
param([string]$BuildDir = 'build', [string]$Config = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$mode = if ($env:AVATAR_VERIFY_MODE) { $env:AVATAR_VERIFY_MODE } else { 'fast' }
$soakSeconds = if ($mode -eq 'full') { 600 } else { 20 }
$soakWarmup = if ($mode -eq 'full') { 30 } else { 10 }
Push-Location $root
try {
    & ctest --test-dir $BuildDir -C $Config -R '^avatar_live_pipeline_unit$' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Live pipeline unit suite failed' }
    & ctest --test-dir $BuildDir -C $Config -R '^avatar_live_unit$' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Live session unit suite failed' }

    $godot = Join-Path $root '.tools/godot/Godot_v4.7.2-stable_win64_console.exe'
    if (-not (Test-Path -LiteralPath $godot)) { throw 'Pinned Windows Godot required for T26' }

    # ---- Golden matrix -------------------------------------------------
    if (Test-Path -LiteralPath .tools/T26-golden.json) {
        Remove-Item -LiteralPath .tools/T26-golden.json
    }
    if (Test-Path -LiteralPath .tools/T26-golden-artifacts) {
        Remove-Item -LiteralPath .tools/T26-golden-artifacts -Recurse -Force
    }
    $goldenOut = Join-Path $root '.tools/T26-golden.log'
    $goldenErr = Join-Path $root '.tools/T26-golden-error.log'
    $golden = Start-Process -FilePath $godot -WindowStyle Hidden -ArgumentList @(
        '--path', 'app', '--resolution', '320x320',
        '--script', 'res://tests/m1_golden.gd', '--quit-after', 180000, '--', '--render'
    ) -RedirectStandardOutput $goldenOut -RedirectStandardError $goldenErr -PassThru -Wait
    Get-Content -LiteralPath $goldenOut | Write-Host
    $goldenText = Get-Content -LiteralPath $goldenErr -Raw
    if ($goldenText) { Write-Host $goldenText }
    if ($golden.ExitCode -ne 0 -or
        $goldenText -match '(?im)(SCRIPT ERROR|SHADER ERROR|^ERROR:)') {
        throw 'M1 golden suite failed (see .tools/T26-golden-artifacts)'
    }
    if (-not (Test-Path -LiteralPath .tools/T26-golden.json)) {
        throw 'M1 golden suite wrote no JSON'
    }

    # ---- App soak ------------------------------------------------------
    if (Test-Path -LiteralPath .tools/T26-soak.json) {
        Remove-Item -LiteralPath .tools/T26-soak.json
    }
    $soakOut = Join-Path $root '.tools/T26-soak.log'
    $soakErr = Join-Path $root '.tools/T26-soak-error.log'
    # The soak script quits itself after the measured window; --quit-after is
    # only a runaway guard. A duration-derived frame count can fire first when
    # the engine's idle-frame rate differs from the paced rate.
    $frames = 10000000
    $soak = Start-Process -FilePath $godot -WindowStyle Hidden -ArgumentList @(
        '--path', 'app', '--resolution', '1280x720',
        '--script', 'res://tests/m1_soak.gd', '--quit-after', $frames, '--',
        '--render', '--seconds', $soakSeconds, '--warmup', $soakWarmup
    ) -RedirectStandardOutput $soakOut -RedirectStandardError $soakErr -PassThru -Wait
    Get-Content -LiteralPath $soakOut | Write-Host
    $soakText = Get-Content -LiteralPath $soakErr -Raw
    if ($soakText) { Write-Host $soakText }
    if ($soakText -match '(?im)(SCRIPT ERROR|SHADER ERROR|^ERROR:)') {
        throw 'M1 soak reported an engine error'
    }
    if ($soak.ExitCode -ne 0) { throw "M1 soak failed its own gates (exit $($soak.ExitCode))" }
    if (-not (Test-Path -LiteralPath .tools/T26-soak.json)) {
        throw 'M1 soak wrote no JSON'
    }

    # ---- Independent threshold reader (T27) ---------------------------
    $thresholdArgs = @('-SoakPath', '.tools/T26-soak.json', '-GoldenPath',
        '.tools/T26-golden.json', '-MinSeconds', "$soakSeconds")
    if ($mode -ne 'full') { $thresholdArgs += '-AllowShort' }
    & powershell -NoProfile -ExecutionPolicy Bypass -File tests/gates/T27.ps1 @thresholdArgs
    if ($LASTEXITCODE -ne 0) { throw 'T27 threshold reader failed' }

    # ---- Design markers and hygiene -----------------------------------
    $qualityDoc = Get-Content docs/design/testing-and-quality.md -Raw
    if ($qualityDoc -notmatch 'T26 M1 golden suite') {
        throw 'testing-and-quality.md does not record the M1 golden suite'
    }
    $faceDoc = Get-Content docs/design/face-compositing.md -Raw
    if ($faceDoc -notmatch 'T26 live pipeline') {
        throw 'face-compositing.md does not record the live pipeline wiring'
    }
    $captureDoc = Get-Content docs/design/camera-capture.md -Raw
    if ($captureDoc -notmatch 'T26') {
        throw 'camera-capture.md does not record the app capture preferences'
    }
    $mapDoc = Get-Content docs/design/implementation-map.md -Raw
    if ($mapDoc -notmatch 'T26') {
        throw 'implementation-map.md does not record T26'
    }
    if (-not (Test-Path -LiteralPath docs/design/adr/0037-m1-golden-matrix-and-live-soak.md)) {
        throw 'Missing ADR-0037'
    }

    & node scripts/check-design-links.mjs
    if ($LASTEXITCODE -ne 0) { throw 'Design-link check failed' }
    & powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format-check.ps1 -Require
    if ($LASTEXITCODE -ne 0) { throw 'Formatting check failed' }
    Write-Host ("T26 gate: PASS ({0} s soak, T27 thresholds)" -f $soakSeconds)
} finally { Pop-Location }
