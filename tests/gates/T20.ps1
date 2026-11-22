#requires -Version 5.1
<# T20: flat decal mode. Headless component contract plus a real-window render
that compares the avocado scene to an independent analytic reference, checks
the black-camera mask diff, edge clamp, mirroring and linear colour, and
compares four committed goldens. Dummy rendering is not image evidence.
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $root
try {
    $godot = Join-Path $root '.tools/godot/Godot_v4.7.2-stable_win64_console.exe'
    if (-not (Test-Path -LiteralPath $godot)) { throw 'Pinned Windows Godot required for the T20 render gate' }

    & $godot --headless --path app --script res://tests/face_decal_flat.gd --quit-after 3000
    if ($LASTEXITCODE -ne 0) { throw 'Flat-decal headless component suite failed' }
    $headless = Get-Content .tools/T20-face-decal.json -Raw | ConvertFrom-Json
    if (-not $headless.passed -or $headless.rendered) { throw 'Flat-decal headless result is incomplete' }

    $output = Join-Path $root '.tools/T20-render.log'
    $errors = Join-Path $root '.tools/T20-render-error.log'
    $process = Start-Process -FilePath $godot -WindowStyle Hidden -ArgumentList @(
        '--path', 'app', '--rendering-method', 'gl_compatibility', '--resolution', '320x320',
        '--script', 'res://tests/face_decal_flat.gd', '--quit-after', '3000', '--', '--render'
    ) -RedirectStandardOutput $output -RedirectStandardError $errors -PassThru -Wait
    Get-Content -LiteralPath $output | Write-Host
    $errorText = Get-Content -LiteralPath $errors -Raw
    if ($errorText) { Write-Host $errorText }
    if ($process.ExitCode -ne 0 -or $errorText -match '(?im)(SCRIPT ERROR|SHADER ERROR|^ERROR:|FAIL:)') {
        throw 'Flat-decal rendered goldens failed'
    }

    $measurement = Get-Content .tools/T20-face-decal-render.json -Raw | ConvertFrom-Json
    if (-not $measurement.passed -or -not $measurement.rendered -or
        $measurement.scenarios -ne 4 -or $measurement.goldens -ne 4 -or
        -not $measurement.mask_diff -or $measurement.mask_outside -ne 0 -or
        -not $measurement.edge_clamp -or -not $measurement.mirroring -or
        -not $measurement.colour_space -or -not $measurement.opacity -or
        $measurement.avocado_pixels -lt 1000) {
        throw 'Incomplete flat-decal render evidence'
    }
    foreach ($name in @('coverage', 'edge-clamp', 'mirroring', 'colour-space')) {
        $golden = Join-Path $root "tests/golden/face-decal-flat/$name.png"
        if (-not (Test-Path -LiteralPath $golden)) { throw "Missing flat-decal golden $name.png" }
    }

    $shader = Get-Content app/shaders/face_decal_flat.gdshader -Raw
    foreach ($token in @('depth_draw_never', 'blend_mix', 'repeat_disable', 'srgb_to_linear',
            'canonical_to_image', 'region_uv', 'region_anchor', 'region_rot', 'feather',
            'colour_gain', 'colour_lift', 'opacity')) {
        if ($shader -notmatch [regex]::Escape($token)) { throw "Flat shader is missing $token" }
    }
    if ($shader -match 'repeat_enable') { throw 'Flat shader must never wrap the camera sampler' }

    $compositing = Get-Content docs/design/face-compositing.md -Raw
    if ($compositing -notmatch 'Implemented flat decal \(T20\)') {
        throw 'face-compositing.md does not record the realised flat decal'
    }
    if (-not (Test-Path -LiteralPath docs/design/adr/0030-flat-decal-quad-pass.md)) {
        throw 'ADR-0030 is missing'
    }
    $map = Get-Content docs/design/implementation-map.md -Raw
    if ($map -notmatch 'T20') { throw 'The implementation map does not mention T20' }

    & node scripts/check-design-links.mjs
    if ($LASTEXITCODE -ne 0) { throw 'Design-link check failed' }
    & powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format-check.ps1 -Require
    if ($LASTEXITCODE -ne 0) { throw 'Formatting check failed' }
    Write-Host 'T20 gate: PASS (component contract, avocado render, analytic reference, mask diff, clamp, mirroring, linear colour, 4 goldens)'
} finally { Pop-Location }
