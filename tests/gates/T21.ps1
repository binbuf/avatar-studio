#requires -Version 5.1
<# T21: real shared-surface decal evidence. Dummy rendering only checks the
component contract. Both real renderers compare independent CPU references and
committed goldens; Forward+ also measures the 1080p second-pass GPU cost.
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $root
try {
    $godot = Join-Path $root '.tools/godot/Godot_v4.7.2-stable_win64_console.exe'
    if (-not (Test-Path -LiteralPath $godot)) { throw 'Pinned Godot required for T21' }
    if (Test-Path -LiteralPath .tools/T21-surface.json) { Remove-Item -LiteralPath .tools/T21-surface.json }
    & $godot --headless --path app --script res://tests/face_decal_surface.gd --quit-after 3000
    if ($LASTEXITCODE -ne 0) { throw 'Surface headless contract failed' }
    $headless = Get-Content .tools/T21-surface.json -Raw | ConvertFrom-Json
    if (-not $headless.passed -or -not $headless.contract -or $headless.rendered) {
        throw 'Incomplete surface headless contract'
    }

    foreach ($renderer in @('gl_compatibility', 'forward_plus')) {
        if (Test-Path -LiteralPath .tools/T21-surface-render.json) { Remove-Item -LiteralPath .tools/T21-surface-render.json }
        $output = Join-Path $root ".tools/T21-$renderer.log"
        $errors = Join-Path $root ".tools/T21-$renderer-error.log"
        $process = Start-Process -FilePath $godot -WindowStyle Hidden -ArgumentList @(
            '--path', 'app', '--rendering-method', $renderer, '--resolution', '320x320',
            '--script', 'res://tests/face_decal_surface.gd', '--quit-after', '10000', '--', '--render'
        ) -RedirectStandardOutput $output -RedirectStandardError $errors -PassThru -Wait
        Get-Content -LiteralPath $output | Write-Host
        $errorText = Get-Content -LiteralPath $errors -Raw
        if ($errorText) { Write-Host $errorText }
        if ($process.ExitCode -ne 0 -or $errorText -match '(?im)(SCRIPT ERROR|SHADER ERROR|^ERROR:|FAIL:)') {
            throw "Surface $renderer goldens failed"
        }
        $render = Get-Content .tools/T21-surface-render.json -Raw | ConvertFrom-Json
        if (-not $render.passed -or -not $render.rendered -or -not $render.contract -or
            $render.renderer -ne $renderer -or $render.goldens -ne 22 -or $render.stable_frames -ne 16) {
            throw "Incomplete surface $renderer render evidence"
        }
        foreach ($model in @('avocado', 'flat', 'sphere', 'sculpted')) {
            foreach ($pose in @('front', 'yaw-left', 'yaw-right', 'deformed', 'back')) {
                $case = @($render.cases | Where-Object { $_.model -eq $model -and $_.pose -eq $pose })
                if ($case.Count -ne 1 -or $case[0].error -ge 0.003 -or $case[0].large_errors -ge 150) {
                    throw "Missing or failed $model/$pose oracle"
                }
                if ($pose -eq 'back' -and $case[0].changed_pixels -ne 0) { throw 'Rear decal leakage' }
                if ($model -eq 'sculpted' -and $pose -in @('yaw-left', 'yaw-right') -and
                    $case[0].occluded_eye_pixels -le 10) { throw 'Nose occlusion case is vacuous' }
                if (-not (Test-Path -LiteralPath "tests/golden/face-decal-surface/$renderer/$model-$pose.png")) {
                    throw "Missing $renderer/$model-$pose.png"
                }
            }
        }
        if (-not (Test-Path -LiteralPath "tests/golden/face-decal-surface/$renderer/sphere-sampling.png")) {
            throw 'Missing rotated, anchored, colour/opacity and clamped-sampling golden'
        }
        $node = @($render.cases | Where-Object { $_.model -eq 'node' -and $_.pose -eq 'deformed' })
        if ($node.Count -ne 1 -or $node[0].error -ge 0.003 -or
            -not (Test-Path -LiteralPath "tests/golden/face-decal-surface/$renderer/node-deformed.png")) {
            throw 'Missing node-deformation surface evidence'
        }
        Copy-Item -LiteralPath .tools/T21-surface-render.json -Destination ".tools/T21-surface-$renderer.json"
    }

    $output = Join-Path $root '.tools/T21-perf.log'
    $errors = Join-Path $root '.tools/T21-perf-error.log'
    if (Test-Path -LiteralPath .tools/T21-surface-perf.json) { Remove-Item -LiteralPath .tools/T21-surface-perf.json }
    $process = Start-Process -FilePath $godot -WindowStyle Hidden -ArgumentList @(
        '--path', 'app', '--rendering-method', 'forward_plus', '--resolution', '320x320',
        '--script', 'res://tests/face_decal_surface.gd', '--quit-after', '3000', '--', '--render', '--perf'
    ) -RedirectStandardOutput $output -RedirectStandardError $errors -PassThru -Wait
    Get-Content -LiteralPath $output | Write-Host
    $errorText = Get-Content -LiteralPath $errors -Raw
    if ($errorText) { Write-Host $errorText }
    if ($process.ExitCode -ne 0 -or $errorText -match '(?im)(SCRIPT ERROR|SHADER ERROR|^ERROR:|FAIL:)') {
        throw 'Surface GPU performance test failed'
    }
    $measurement = Get-Content .tools/T21-surface-perf.json -Raw | ConvertFrom-Json
    $perf = $measurement.performance
    if (-not $measurement.passed -or -not $measurement.rendered -or -not $perf.within_budget -or
        $perf.resolution -ne '1920x1080' -or $perf.triangles -ne 20000 -or
        $perf.samples_per_state -ne 180 -or $perf.base_gpu_median_ms -le 0 -or
        $perf.decal_gpu_median_ms -le 0 -or $perf.decal_draws -ne ($perf.base_draws + 1) -or
        $perf.second_pass_gpu_ms -ge 0.7 -or $perf.second_pass_upper_spread_ms -ge 0.7) {
        throw 'Incomplete or over-budget second-pass GPU evidence'
    }
    foreach ($path in @('app/scenes/face_decal_surface.gd', 'docs/design/adr/0031-surface-decal-shared-pass.md')) {
        if (-not (Test-Path -LiteralPath $path)) { throw "Missing $path" }
    }
    & node scripts/check-design-links.mjs
    if ($LASTEXITCODE -ne 0) { throw 'Design-link check failed' }
    & powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format-check.ps1 -Require
    if ($LASTEXITCODE -ne 0) { throw 'Formatting check failed' }
    Write-Host 'T21 gate: PASS (shared surface, 44 goldens in two renderers, occlusion, deformation, rear rejection, GPU budget)'
} finally { Pop-Location }
