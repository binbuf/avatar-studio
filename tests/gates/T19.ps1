#requires -Version 5.1
<# T19: native reference math, engine deformation, independent rendered goldens and measured cost.
The cut CPU budget overrun is explicit in ADR-0029; this gate checks the measurement is present,
not that the 0.15 ms target was met. Real-window rendering is required (dummy rendering is insufficient).
#>
[CmdletBinding()]
param([string]$BuildDir = 'build', [string]$Config = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $root
try {
    & ctest --test-dir $BuildDir -C $Config -R '^avatar_head_unit$' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Head-region unit suite failed' }
    $tests = (& ctest --test-dir $BuildDir -C $Config -N -R '^avatar_head_unit$') -join "`n"
    if ($tests -notmatch 'Total Tests: 1') { throw 'Head-region unit suite is missing' }
    $godot = Join-Path $root '.tools/godot/Godot_v4.7.2-stable_win64_console.exe'
    if (-not (Test-Path -LiteralPath $godot)) { throw 'Pinned Windows Godot required for T19 rendered gate' }
    & $godot --headless --path app --script res://tests/head_region.gd --quit-after 3000
    if ($LASTEXITCODE -ne 0) { throw 'Head-region engine suite failed' }
    $output = Join-Path $root '.tools/T19-render.log'
    $errors = Join-Path $root '.tools/T19-render-error.log'
    $process = Start-Process -FilePath $godot -WindowStyle Hidden -ArgumentList @(
        '--path', 'app', '--rendering-method', 'gl_compatibility', '--resolution', '320x320',
        '--script', 'res://tests/head_region.gd', '--quit-after', '3000', '--', '--render'
    ) -RedirectStandardOutput $output -RedirectStandardError $errors -PassThru -Wait
    Get-Content -LiteralPath $output | Write-Host
    $errorText = Get-Content -LiteralPath $errors -Raw
    if ($errorText) { Write-Host $errorText }
    if ($process.ExitCode -ne 0 -or $errorText -match '(?im)(SCRIPT ERROR|SHADER ERROR|^ERROR:|FAIL:)') {
        throw 'Head-region rendered goldens failed'
    }
    $measurement = Get-Content .tools/T19-head-region-render.json -Raw | ConvertFrom-Json
    if (-not $measurement.passed -or -not $measurement.rendered -or $measurement.poses -ne 12 -or
        $measurement.triangles -ne 20000 -or $measurement.deformation_median_ms -le 0 -or
        $measurement.node_median_ms -lt 0) { throw 'Incomplete deformation/render measurement' }
    $adr = Get-Content docs/design/adr/0029-shared-cpu-head-deformation.md -Raw
    if ($adr -notmatch '0.15 ms' -or $adr -notmatch 'overrun') { throw 'Missing explicit CPU budget disposition' }
    & node scripts/check-design-links.mjs
    if ($LASTEXITCODE -ne 0) { throw 'Design-link check failed' }
    & powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format-check.ps1 -Require
    if ($LASTEXITCODE -ne 0) { throw 'Formatting check failed' }
    Write-Host 'T19 gate: PASS (unit math, cut/node geometry, 12 independent rendered goldens; CPU overrun documented)'
} finally { Pop-Location }
