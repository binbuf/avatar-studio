#requires -Version 5.1
<# T22: presentation, fit, distance, frustum, mirroring and framing goldens.
The unit suite owns the pure math. The Godot headless phase owns the adapter
contract and overlay. The real Forward+ window phase compares an independent
GDScript projection oracle with the live SubViewport and checks committed PNGs.
#>
[CmdletBinding()]
param([string]$BuildDir = 'build', [string]$Config = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $root
try {
    & ctest --test-dir $BuildDir -C $Config -R '^avatar_framing_unit$' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Framing unit suite failed' }
    $tests = (& ctest --test-dir $BuildDir -C $Config -N -R '^avatar_framing_unit$') -join "`n"
    if ($tests -notmatch 'Total Tests: 1') { throw 'Framing unit suite is missing' }

    $godot = Join-Path $root '.tools/godot/Godot_v4.7.2-stable_win64_console.exe'
    if (-not (Test-Path -LiteralPath $godot)) { throw 'Pinned Windows Godot required for T22' }

    if (Test-Path -LiteralPath .tools/T22-framing.json) { Remove-Item -LiteralPath .tools/T22-framing.json }
    & $godot --headless --path app --script res://tests/framing.gd --quit-after 10000
    if ($LASTEXITCODE -ne 0) { throw 'Framing headless contract failed' }
    $headless = Get-Content .tools/T22-framing.json -Raw | ConvertFrom-Json
    if (-not $headless.passed -or $headless.rendered) { throw 'Incomplete framing headless contract' }

    if (Test-Path -LiteralPath .tools/T22-framing-render.json) {
        Remove-Item -LiteralPath .tools/T22-framing-render.json
    }
    $output = Join-Path $root '.tools/T22-render.log'
    $errors = Join-Path $root '.tools/T22-render-error.log'
    $process = Start-Process -FilePath $godot -WindowStyle Hidden -ArgumentList @(
        '--path', 'app', '--rendering-method', 'forward_plus', '--resolution', '320x320',
        '--script', 'res://tests/framing.gd', '--quit-after', '30000', '--', '--render'
    ) -RedirectStandardOutput $output -RedirectStandardError $errors -PassThru -Wait
    Get-Content -LiteralPath $output | Write-Host
    $errorText = Get-Content -LiteralPath $errors -Raw
    if ($errorText) { Write-Host $errorText }
    if ($process.ExitCode -ne 0 -or
        $errorText -match '(?im)(SCRIPT ERROR|SHADER ERROR|^ERROR:|FAIL:)') {
        throw 'Framing rendered goldens failed'
    }
    $render = Get-Content .tools/T22-framing-render.json -Raw | ConvertFrom-Json
    if (-not $render.passed -or -not $render.rendered -or $render.cases.Count -ne 8) {
        throw 'Incomplete framing render evidence'
    }
    foreach ($case in $render.cases) {
        if ($case.marker_error -ge 2.5 -or $case.marker_pixels -le 4) {
            throw "Marker mismatch in $($case.name)"
        }
        if (-not (Test-Path -LiteralPath "tests/golden/framing/$($case.name).png")) {
            throw "Missing golden $($case.name).png"
        }
    }
    foreach ($name in @('model-default', 'model-tight', 'model-loose', 'model-zoom-out',
                        'model-offset', 'head-fit')) {
        $case = @($render.cases | Where-Object { $_.name -eq $name })
        if ($case.Count -ne 1 -or -not $case[0].inside) {
            throw "$name is not contained in the viewport"
        }
    }
    $zoomIn = @($render.cases | Where-Object { $_.name -eq 'model-zoom-in' })
    if ($zoomIn.Count -ne 1 -or $zoomIn[0].inside) {
        throw 'model-zoom-in must record the documented deliberate crop'
    }
    $mirror = @($render.cases | Where-Object { $_.name -eq 'mirror-output' })
    if ($mirror.Count -ne 1 -or -not $mirror[0].mirrored) {
        throw 'mirror-output case is missing'
    }
    $model = @($render.cases | Where-Object { $_.name -eq 'model-default' })
    $offset = @($render.cases | Where-Object { $_.name -eq 'model-offset' })
    if ($model.Count -ne 1 -or $offset.Count -ne 1 -or
        $offset[0].silhouette[0] -le $model[0].silhouette[0]) {
        throw 'Positive x offset did not move the avatar right'
    }

    $face = Get-Content docs/design/face-compositing.md -Raw
    if ($face -notmatch 'Implemented framing \(T22\)' -or $face -notmatch 'zoom > 1') {
        throw 'face-compositing.md does not record the realised framing'
    }
    if (-not (Test-Path -LiteralPath docs/design/adr/0032-presentation-framing-and-mirroring.md)) {
        throw 'Missing ADR-0032'
    }
    & node scripts/check-design-links.mjs
    if ($LASTEXITCODE -ne 0) { throw 'Design-link check failed' }
    & powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format-check.ps1 -Require
    if ($LASTEXITCODE -ne 0) { throw 'Formatting check failed' }
    Write-Host 'T22 gate: PASS (unit math, adapter contract, 8 independent framing goldens, overlay, mirroring)'
} finally { Pop-Location }
