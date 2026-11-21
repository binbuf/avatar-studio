<#
.SYNOPSIS
    T17 gate: runtime GLB import, bind pose, units/axes and the avatar scene.

.DESCRIPTION
    Runs after the native build, the fast CTest label and the headless Godot
    tests. The import test is the measurement; this gate re-checks the static
    implementation, the measured JSON, the fixtures, the design documents and
    the formatting.

      1. The GDExtension builds AvatarModel and registers it.
      2. The importer uses GLTFDocument/GLTFState, drops bones/weights, never
         creates AnimationPlayer or Skeleton3D, and fails with AVT-102.
      3. main.tscn keeps one SubViewport and adds a camera plus key/fill lights.
      4. app/tests/model_import.gd encodes the avocado, fixture, AVT-102 and
         app-scene checks.
      5. .tools/T17-model-import.json (written by scripts/smoke.ps1 -Model)
         passes and carries the measured time/memory.
      6. The fixtures and generator exist.
      7. The implementation map, architecture, testing, package-format, ADR and
         progress documents record the realised import.
      8. The design-link and formatting checks pass.

.PARAMETER BuildDir
    Build directory used by scripts/verify.ps1.

.PARAMETER Config
    Build configuration used by scripts/verify.ps1.
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build',
    [string]$Config = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$failures = New-Object System.Collections.Generic.List[string]

function Add-Failure([string]$Message) {
    $script:failures.Add($Message)
}

function Read-Required([string]$RelativePath) {
    $path = Join-Path $root $RelativePath
    if (-not (Test-Path -LiteralPath $path)) {
        Add-Failure "missing file: $RelativePath"
        return ''
    }
    return (Get-Content -LiteralPath $path -Raw)
}

function Require-Text([string]$Text, [string]$Path, [string[]]$Needles) {
    if ($Text -eq '') { return }
    $normalised = ($Text -replace '\s+', ' ')
    foreach ($needle in $Needles) {
        $wanted = ($needle -replace '\s+', ' ')
        if (-not $normalised.Contains($wanted)) {
            Add-Failure "$Path is missing '$needle'"
        }
    }
}

function Require-File([string]$RelativePath) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $RelativePath))) {
        Add-Failure "missing file: $RelativePath"
    }
}

# 1. Build and registration.
$cmake = Read-Required 'CMakeLists.txt'
Require-Text $cmake 'CMakeLists.txt' @('native/gdextension')

$gdeCmake = Read-Required 'native/gdextension/CMakeLists.txt'
Require-Text $gdeCmake 'native/gdextension/CMakeLists.txt' @(
    'src/avatar_model.cpp', 'avatar_json'
)

$register = Read-Required 'native/gdextension/src/register_types.cpp'
Require-Text $register 'register_types.cpp' @('avatar_model.h', 'GDREGISTER_CLASS(AvatarModel)')

# 2. Importer behaviour.
$model = Read-Required 'native/gdextension/src/avatar_model.cpp'
Require-Text $model 'avatar_model.cpp' @(
    'GLTFDocument', 'GLTFState', 'append_from_file', 'set_create_animations(false)',
    'set_import_as_skeleton_bones(false)', 'ARRAY_BONES', 'ARRAY_WEIGHTS',
    'face_submesh', 'AVT-102', 'AVT-101', 'kMaxTriangles', 'set_uniform_scale',
    'get_decal_vertices', 'get_decal_normals', 'get_decal_indices',
    'get_candidate_head_nodes', 'get_import_time_ms', 'get_peak_memory_bytes',
    'get_realised_facing', 'get_realised_up'
)
if ($model -match 'memnew\(\s*AnimationPlayer') {
    Add-Failure 'avatar_model.cpp instantiates an AnimationPlayer'
}
if ($model -match 'memnew\(\s*Skeleton3D') {
    Add-Failure 'avatar_model.cpp instantiates a Skeleton3D'
}

$runtime = Read-Required 'native/gdextension/src/avatar_runtime.cpp'
Require-Text $runtime 'avatar_runtime.cpp' @(
    'load_default_avatar', 'frame_avatar_camera', 'avatar_model_', 'AvatarCamera'
)

# 3. Scene configuration. One SubViewport stays the invariant.
$scene = Read-Required 'app/scenes/main.tscn'
Require-Text $scene 'app/scenes/main.tscn' @(
    'type="SubViewport"', 'type="Node3D"', 'type="Camera3D"', 'type="DirectionalLight3D"',
    'type="AvatarRuntime"', 'transform = Transform3D(-1, 0, 0, 0, 1, 0, 0, 0, -1, 0, 0, 0)'
)
if ($scene -match '(?m)^\[node[^\]]*type="SubViewport"') {
    $subViewports = [regex]::Matches($scene, 'type="SubViewport"').Count
    if ($subViewports -ne 1) {
        Add-Failure "main.tscn declares $subViewports SubViewports, expected exactly 1"
    }
}
if ($scene -match 'type="AnimationPlayer"') {
    Add-Failure 'main.tscn contains an AnimationPlayer'
}

# 4. The import test encodes the acceptance checks.
$modelTest = Read-Required 'app/tests/model_import.gd'
Require-Text $modelTest 'app/tests/model_import.gd' @(
    'avocado_half.glb', 'feet_origin.glb', 'axis_variant.glb', 'animated_rig.glb',
    'invalid.glb', 'AVT-102', 'face_submesh', 'get_decal_indices',
    'get_candidate_head_nodes', 'get_import_time_ms', 'get_peak_memory_bytes',
    '_test_app_scene', 'get_static_memory_usage'
)

# 5. The measured JSON from scripts/smoke.ps1 -Model.
$jsonPath = Join-Path $root '.tools/T17-model-import.json'
if (-not (Test-Path -LiteralPath $jsonPath)) {
    Add-Failure "missing import JSON: $jsonPath (scripts/smoke.ps1 -Model must run first)"
} else {
    try {
        $summary = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json
    } catch {
        Add-Failure "import JSON is not parseable: $($_.Exception.Message)"
        $summary = $null
    }
    if ($null -ne $summary) {
        if ($summary.schema -ne 'avatar_model.result.v1') {
            Add-Failure "import JSON schema is '$($summary.schema)', expected avatar_model.result.v1"
        }
        if ($summary.passed -ne $true) {
            Add-Failure 'import JSON reports passed=false'
        }
        foreach ($check in @('avocado', 'mesh_and_material_counts', 'aabb', 'animations_ignored',
                             'feet_origin', 'axis_variant', 'face_submesh', 'invalid_avt_102',
                             'app_render')) {
            if ($summary.checks.$check -ne $true) {
                Add-Failure "import JSON check '$check' is not true"
            }
        }
        $wall = [double]$summary.metrics.wall_ms
        $reported = [double]$summary.metrics.reported_import_ms
        if ($wall -le 0.0 -or $wall -gt 5000.0) {
            Add-Failure "import wall time is $wall ms, expected 0 < ms <= 5000"
        }
        if ($reported -le 0.0 -or $reported -gt 5000.0) {
            Add-Failure "reported import time is $reported ms, expected 0 < ms <= 5000"
        }
        $peak = [int64]$summary.metrics.peak_static_memory_bytes
        if ($peak -le 0) {
            Add-Failure "peak static memory is $peak bytes, expected a positive measurement"
        }
        if ($null -eq $summary.metrics.static_memory_delta_bytes) {
            Add-Failure 'import JSON has no static memory delta'
        }
        Write-Host (("T17 gate: measured import {0:N2} ms wall, {1:N2} ms reported, " +
            "static delta {2} bytes, peak {3} bytes") -f $wall, $reported,
            [int64]$summary.metrics.static_memory_delta_bytes, $peak)
    }
}

# 6. Fixtures and generator.
Require-File 'assets/models/avocado_half.glb'
Require-File 'tests/fixtures/models/feet_origin.glb'
Require-File 'tests/fixtures/models/axis_variant.glb'
Require-File 'tests/fixtures/models/animated_rig.glb'
Require-File 'tests/fixtures/models/invalid.glb'
Require-File 'scripts/gen_model_fixtures.py'
$invalid = Join-Path $root 'tests/fixtures/models/invalid.glb'
if ((Test-Path -LiteralPath $invalid) -and (Get-Item -LiteralPath $invalid).Length -ge 100) {
    Add-Failure 'invalid.glb is unexpectedly large; it should be a deliberately broken file'
}

# 7. Design and progress documents.
$package = Read-Required 'docs/design/avatar-package-format.md'
Require-Text $package 'docs/design/avatar-package-format.md' @(
    'Runtime import (T17)', 'GLTFState', 'AVT-102', 'face_submesh', 'bind pose',
    'metres, Y up', 'uniform scale'
)

$architecture = Read-Required 'docs/design/architecture.md'
Require-Text $architecture 'docs/design/architecture.md' @(
    'Runtime GLB import (T17)', 'AvatarModel', 'GLTFState', 'bind pose', 'T17'
)

$testing = Read-Required 'docs/design/testing-and-quality.md'
Require-Text $testing 'docs/design/testing-and-quality.md' @(
    'Runtime GLB import (T17)', 'tests/gates/T17.ps1', 'T17', 'AVT-102',
    'feet-origin', 'axis-variant'
)

$map = Read-Required 'docs/design/implementation-map.md'
if ($map -ne '') {
    if ($map -notmatch 'current as of T(\d{2,3})') {
        Add-Failure 'implementation-map.md has no "current as of TNN" marker'
    } elseif ([int]$Matches[1] -lt 17) {
        Add-Failure "implementation-map.md is stale (current as of T$($Matches[1]), expected T17 or later)"
    }
    if ($map -notmatch '(?m)^\|\s*Rendering strategy: runtime GLB import[^\r\n]*\|\s*`Implemented`[^\r\n]*\|') {
        Add-Failure 'implementation-map.md does not mark the runtime GLB import row Implemented'
    }
    if ($map -notmatch '(?m)^\|\s*\[ADR-0027\]\(adr/0027-runtime-glb-import\.md\)[^\r\n]*\|\s*Accepted\s*\|') {
        Add-Failure 'implementation-map.md does not list ADR-0027 as Accepted'
    }
}

$adr = Read-Required 'docs/design/adr/0027-runtime-glb-import.md'
Require-Text $adr 'ADR-0027' @(
    '# ADR-0027', '**Status:** Accepted', 'GLTFState', 'bind pose', 'face_submesh'
)

$adrIndex = Read-Required 'docs/design/adr/README.md'
Require-Text $adrIndex 'docs/design/adr/README.md' @('0027', 'Runtime GLB import')

$progressPath = Join-Path $root 'docs/progress/T17.md'
if (Test-Path -LiteralPath $progressPath) {
    $progress = Get-Content -LiteralPath $progressPath -Raw
    foreach ($section in @('## What landed', '## Import pipeline', '## Fixtures',
                           '## Measured result', '## Tests', '## Gotchas',
                           '## Follow-ups', '## Hand-off')) {
        if ($progress -notmatch [regex]::Escape($section)) {
            Add-Failure "docs/progress/T17.md is missing section '$section'"
        }
    }
    foreach ($needle in @('wall time', 'static memory', 'peak', 'AVT-102', '48,688',
                          'face_submesh')) {
        if ($progress -notmatch [regex]::Escape($needle)) {
            Add-Failure "docs/progress/T17.md is missing '$needle'"
        }
    }
} else {
    Write-Host 'T17 gate: note - docs/progress/T17.md not present (gitignored workspace note); skipped'
}

# 8. Design links and formatting.
& node (Join-Path $root 'scripts/check-design-links.mjs')
if ($LASTEXITCODE -ne 0) {
    Add-Failure 'design-link check failed after the T17 documentation change'
}

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts/format-check.ps1')
if ($LASTEXITCODE -ne 0) {
    Add-Failure 'formatting drift after the T17 implementation'
}

if ($failures.Count -gt 0) {
    Write-Host 'T17 gate: FAIL' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

$mode = if ($env:AVATAR_VERIFY_MODE) { $env:AVATAR_VERIFY_MODE } else { 'gate-only' }
Write-Host ("T17 gate: PASS ($mode; runtime GLB import, bind pose, units/axes, fixtures, " +
    'AVT-102, app scene, design links and formatting)')
exit 0
