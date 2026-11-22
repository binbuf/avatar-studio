#requires -Version 5.1
<# T23: output gate and window output.
The native suite owns the gate state machine, alpha/colour conversion, PNG
writer and Null/File transports. The headless Godot phase exercises the same
gate through the GDExtension adapter. The real Compatibility window phase adds
the no-passthrough diff (camera pattern vs black camera) and the blank deadline
on the window transport.
#>
[CmdletBinding()]
param([string]$BuildDir = 'build', [string]$Config = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $root
try {
    & ctest --test-dir $BuildDir -C $Config -R '^avatar_output_unit$' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Output gate unit suite failed' }
    $tests = (& ctest --test-dir $BuildDir -C $Config -N -R '^avatar_output_unit$') -join "`n"
    if ($tests -notmatch 'Total Tests: 1') { throw 'Output gate unit suite is missing' }

    $godot = Join-Path $root '.tools/godot/Godot_v4.7.2-stable_win64_console.exe'
    if (-not (Test-Path -LiteralPath $godot)) { throw 'Pinned Windows Godot required for T23' }

    if (Test-Path -LiteralPath .tools/T23-output.json) { Remove-Item -LiteralPath .tools/T23-output.json }
    & $godot --headless --path app --script res://tests/output_gate.gd --quit-after 20000
    if ($LASTEXITCODE -ne 0) { throw 'Output gate headless contract failed' }
    $headless = Get-Content .tools/T23-output.json -Raw | ConvertFrom-Json
    if (-not $headless.passed -or $headless.rendered) { throw 'Incomplete output gate headless contract' }
    if (-not $headless.transports.null -or -not $headless.transports.file) {
        throw 'NullOutput and FileOutput did not follow the gate'
    }
    if (-not $headless.alpha -or -not $headless.colour_space) {
        throw 'Alpha or colour conversion failed'
    }
    if ([int]$headless.file_frames -lt 4) { throw 'FileOutput did not write the expected PNG sequence' }
    $nullDiagnostics = @($headless.diagnostics.null)
    if ($nullDiagnostics.Count -ne 2 -or
        $nullDiagnostics[0] -ne 'TRK-008' -or $nullDiagnostics[1] -ne 'OUT-007') {
        throw 'NullOutput diagnostics are not TRK-008 then OUT-007 once'
    }

    if (Test-Path -LiteralPath .tools/T23-output-render.json) {
        Remove-Item -LiteralPath .tools/T23-output-render.json
    }
    $output = Join-Path $root '.tools/T23-render.log'
    $errors = Join-Path $root '.tools/T23-render-error.log'
    $process = Start-Process -FilePath $godot -WindowStyle Hidden -ArgumentList @(
        '--path', 'app', '--rendering-method', 'gl_compatibility', '--resolution', '320x320',
        '--script', 'res://tests/output_gate.gd', '--quit-after', '30000', '--', '--render'
    ) -RedirectStandardOutput $output -RedirectStandardError $errors -PassThru -Wait
    Get-Content -LiteralPath $output | Write-Host
    $errorText = Get-Content -LiteralPath $errors -Raw
    if ($errorText) { Write-Host $errorText }
    if ($process.ExitCode -ne 0 -or
        $errorText -match '(?im)(SCRIPT ERROR|SHADER ERROR|^ERROR:|FAIL:)') {
        throw 'Output gate rendered phase failed'
    }
    $render = Get-Content .tools/T23-output-render.json -Raw | ConvertFrom-Json
    if (-not $render.passed -or -not $render.rendered) {
        throw 'Incomplete output gate render evidence'
    }
    if (-not $render.no_passthrough) { throw 'No-passthrough invariant failed' }
    if ([int]$render.mask_outside -ne 0) { throw 'Changed pixels escaped the region masks' }
    if ([int]$render.changed_pixels -lt 200) { throw 'Black-camera diff changed too few pixels' }
    foreach ($region in @('eye_l', 'eye_r', 'mouth')) {
        if ([int]$render.per_region.$region -le 20) {
            throw "Changed pixels do not reach region $region"
        }
    }
    if (-not $render.transports.window -or -not $render.transports.file_render -or
        -not $render.transports.null_render) {
        throw 'The gate invariant did not run on window, File and Null transports'
    }
    if (-not $render.dropped_mouth -or -not $render.tracker_timeout -or
        -not $render.forced_invalid) {
        throw 'A missing-feature, timeout or forced-invalid case did not blank'
    }
    if ([double]$render.blank_latency_ms -gt 150.0) { throw 'Blank latency exceeds 150 ms' }
    if ([double]$render.blank_submit_us -gt 150000.0) {
        throw 'Blank submission took longer than 150 ms'
    }
    if (-not $render.window_alive) { throw 'Window output did not stay alive while blank' }
    if (-not $render.alpha -or -not $render.colour_space) {
        throw 'Rendered alpha or colour conversion failed'
    }

    $outputDoc = Get-Content docs/design/output-and-virtual-camera.md -Raw
    if ($outputDoc -notmatch 'Implemented output gate \(T23\)') {
        throw 'output-and-virtual-camera.md does not record the realised gate'
    }
    $faceDoc = Get-Content docs/design/face-compositing.md -Raw
    if ($faceDoc -notmatch 'Implemented output gate \(T23\)') {
        throw 'face-compositing.md does not record the realised gate'
    }
    $qualityDoc = Get-Content docs/design/testing-and-quality.md -Raw
    if ($qualityDoc -notmatch 'T23 output gate') {
        throw 'testing-and-quality.md does not record the T23 gate'
    }
    if (-not (Test-Path -LiteralPath docs/design/adr/0033-output-gate-and-window-transport.md)) {
        throw 'Missing ADR-0033'
    }

    & node scripts/check-design-links.mjs
    if ($LASTEXITCODE -ne 0) { throw 'Design-link check failed' }
    & powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format-check.ps1 -Require
    if ($LASTEXITCODE -ne 0) { throw 'Formatting check failed' }
    Write-Host 'T23 gate: PASS (gate unit suite, headless adapter, window no-passthrough, blank deadline, diagnostics)'
} finally { Pop-Location }
