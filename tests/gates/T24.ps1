#requires -Version 5.1
<# T24: timestamp-paired camera ring.
The native suite owns the four-slot ring, exact timestamp pairing, wrap/drop/
format-change policy, the drop-before-inference gate, FaceRig newest-not-after
selection and the synthetic sliding regression. The headless Godot phase drives
the same ring through the GDExtension adapter. The rendered phase checks the
shared YUV shader against BT.601/709 and limited/full colour bars, proves the
flat decal samples the selected older ring slot, and measures upload/downscale
cost.
#>
[CmdletBinding()]
param([string]$BuildDir = 'build', [string]$Config = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $root
try {
    & ctest --test-dir $BuildDir -C $Config -R '^avatar_camera_ring_unit$' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Camera ring unit suite failed' }
    $tests = (& ctest --test-dir $BuildDir -C $Config -N -R '^avatar_camera_ring_unit$') -join "`n"
    if ($tests -notmatch 'Total Tests: 1') { throw 'Camera ring unit suite is missing' }

    $binary = Join-Path $BuildDir "$Config/avatar_camera_ring_tests.exe"
    if (-not (Test-Path -LiteralPath $binary)) { throw "Missing $binary" }
    $unitOutput = (& $binary 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "Camera ring unit binary failed`n$unitOutput" }
    if ($unitOutput -notmatch 'T24 sliding: paired=([0-9.]+) px naive=([0-9.]+) px') {
        throw 'Sliding regression did not report paired/naive error'
    }
    $paired = [double]$Matches[1]
    $naive = [double]$Matches[2]
    if ($paired -ge 1.0) { throw "Paired sliding error is not sub-pixel: $paired px" }
    if ($naive -le 5.0) { throw "Naive sliding error did not fail: $naive px" }

    $godot = Join-Path $root '.tools/godot/Godot_v4.7.2-stable_win64_console.exe'
    if (-not (Test-Path -LiteralPath $godot)) { throw 'Pinned Windows Godot required for T24' }

    if (Test-Path -LiteralPath .tools/T24-camera-ring.json) {
        Remove-Item -LiteralPath .tools/T24-camera-ring.json
    }
    & $godot --headless --path app --script res://tests/camera_ring.gd --quit-after 30000
    if ($LASTEXITCODE -ne 0) { throw 'Camera ring headless contract failed' }
    $headless = Get-Content .tools/T24-camera-ring.json -Raw | ConvertFrom-Json
    if (-not $headless.passed -or $headless.rendered) { throw 'Incomplete headless ring contract' }
    if (-not $headless.ring.passed) { throw 'Headless ring policy failed' }
    if ([int]$headless.ring.wraps -lt 1) { throw 'Headless ring did not wrap' }
    if ([int]$headless.ring.format_changes -lt 1) { throw 'Headless ring did not change format' }
    if ([int]$headless.ring.dropped -lt 1) { throw 'Headless ring did not drop a pinned frame' }

    if (Test-Path -LiteralPath .tools/T24-camera-ring-render.json) {
        Remove-Item -LiteralPath .tools/T24-camera-ring-render.json
    }
    $output = Join-Path $root '.tools/T24-render.log'
    $errors = Join-Path $root '.tools/T24-render-error.log'
    $process = Start-Process -FilePath $godot -WindowStyle Hidden -ArgumentList @(
        '--path', 'app', '--rendering-method', 'gl_compatibility', '--resolution', '320x320',
        '--script', 'res://tests/camera_ring.gd', '--quit-after', '60000', '--', '--render'
    ) -RedirectStandardOutput $output -RedirectStandardError $errors -PassThru -Wait
    Get-Content -LiteralPath $output | Write-Host
    $errorText = Get-Content -LiteralPath $errors -Raw
    if ($errorText) { Write-Host $errorText }
    if ($process.ExitCode -ne 0 -or
        $errorText -match '(?im)(SCRIPT ERROR|SHADER ERROR|^ERROR:|FAIL:)') {
        throw 'Camera ring rendered phase failed'
    }
    $render = Get-Content .tools/T24-camera-ring-render.json -Raw | ConvertFrom-Json
    if (-not $render.passed -or -not $render.rendered) {
        throw 'Incomplete camera ring render evidence'
    }
    if (-not $render.yuv_bars.passed) { throw 'YUV colour-bar probe failed' }
    foreach ($space in @('bt601_limited', 'bt601_full', 'bt709_limited', 'bt709_full')) {
        if (-not $render.yuv_bars.$space.passed) {
            throw "YUV colour bars failed for $space"
        }
        if ([double]$render.yuv_bars.$space.max_error -gt 0.02) {
            throw "YUV colour-bar error too high for $space"
        }
    }
    if (-not $render.decal.passed) { throw 'The decal did not sample the paired ring slot' }
    if ([int]$render.decal.red_pixels -lt 100 -or [int]$render.decal.blue_pixels -lt 100) {
        throw 'Decal slot-switch evidence is missing'
    }
    if ([double]$render.measurements.upload_p50_us -le 0.0) {
        throw 'Ring upload cost was not measured'
    }
    if ([double]$render.measurements.downscale_p50_us -le 0.0) {
        throw 'Downscale cost was not measured'
    }

    $captureDoc = Get-Content docs/design/camera-capture.md -Raw
    if ($captureDoc -notmatch 'Implemented camera texture ring \(T24\)') {
        throw 'camera-capture.md does not record the realised ring'
    }
    $faceDoc = Get-Content docs/design/face-compositing.md -Raw
    if ($faceDoc -notmatch 'T24 timestamp pairing') {
        throw 'face-compositing.md does not record timestamp pairing'
    }
    $qualityDoc = Get-Content docs/design/testing-and-quality.md -Raw
    if ($qualityDoc -notmatch 'T24 timestamp pairing') {
        throw 'testing-and-quality.md does not record the T24 tests'
    }
    if (-not (Test-Path -LiteralPath docs/design/adr/0034-timestamp-paired-camera-ring.md)) {
        throw 'Missing ADR-0034'
    }

    & node scripts/check-design-links.mjs
    if ($LASTEXITCODE -ne 0) { throw 'Design-link check failed' }
    & powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format-check.ps1 -Require
    if ($LASTEXITCODE -ne 0) { throw 'Formatting check failed' }
    Write-Host 'T24 gate: PASS (ring policy, FaceRig pairing, sliding, YUV bars, decal slot, measurements)'
} finally { Pop-Location }
