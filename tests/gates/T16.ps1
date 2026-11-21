<#
.SYNOPSIS
    T16 gate: one viewport, preview copy, 60 Hz pacing and C++ stats.

.DESCRIPTION
    Runs after the native build, the fast CTest label and the headless Godot
    runtime test. The runtime test is the measurement; this gate re-checks the
    configuration, the measured JSON, the design documents and the formatting.

      1. project.godot keeps Forward+, a 60 fps cap and transparent windows.
      2. main.tscn has one 1920x1080 transparent SubViewport with UPDATE_ALWAYS,
         a fixed Presentation transform, a Preview TextureRect and AvatarRuntime.
      3. The runtime source pins Engine.max_fps, disables low-processor mode,
         selects vsync or the deadline scheduler, samples the viewport texture,
         and drives a hidden C++ HUD.
      4. app/tests/runtime.gd encodes the 10-second gate (p99 <= 16.6 ms, no
         burst longer than two frames, preview render count zero).
      5. .tools/T16-runtime.json (written by scripts/smoke.ps1) passes.
      6. The implementation map, architecture, testing and progress documents
         record the realised pacing and the measured result.
      7. The design-link and formatting checks pass.

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

# 1. Project settings. Godot may normalise project.godot and omit a setting
# that equals its default (forward_plus is the desktop default), so accept the
# feature marker as well as the explicit key; the runtime JSON is the
# authoritative effective-renderer check.
$project = Read-Required 'app/project.godot'
Require-Text $project 'app/project.godot' @(
    'run/max_fps=60', 'window/per_pixel_transparency/allowed=true'
)
if ($project -notmatch 'renderer/rendering_method="forward_plus"' -and
    $project -notmatch 'Forward Plus') {
    Add-Failure 'app/project.godot does not select the Forward+ renderer'
}

# 2. Scene configuration.
$scene = Read-Required 'app/scenes/main.tscn'
Require-Text $scene 'app/scenes/main.tscn' @(
    'type="SubViewport"', 'transparent_bg = true', 'size = Vector2i(1920, 1080)',
    'render_target_update_mode = 4', 'type="Node3D"',
    'transform = Transform3D(-1, 0, 0, 0, 1, 0, 0, 0, -1, 0, 0, 0)',
    'type="TextureRect"', 'type="AvatarRuntime"'
)
if ($scene -match '(?m)^\[node[^\]]*type="SubViewport"') {
    $subViewports = [regex]::Matches($scene, 'type="SubViewport"').Count
    if ($subViewports -ne 1) {
        Add-Failure "main.tscn declares $subViewports SubViewports, expected exactly 1"
    }
}

# 3. Runtime source and pacer.
$runtime = Read-Required 'native/gdextension/src/avatar_runtime.cpp'
Require-Text $runtime 'avatar_runtime.cpp' @(
    'set_max_fps(kTargetFps)', 'set_low_processor_usage_mode(false)',
    'SubViewport::UPDATE_ALWAYS', 'window_set_vsync_mode',
    'FramePacer::select_mode', 'delay_usec', 'get_texture()',
    'preview_render_count_', 'disable_app_nap', 'PROCESS_MODE_ALWAYS',
    'DebugHud', 'set_visible(false)'
)

$pacer = Read-Required 'native/core/include/avatar/core/frame_pacer.h'
Require-Text $pacer 'frame_pacer.h' @(
    'enum class PacingMode', 'select_mode', 'begin_frame', 'dropped',
    'not replayed'
)

$pacerTest = Read-Required 'tests/unit/test_frame_pacer.cpp'
Require-Text $pacerTest 'test_frame_pacer.cpp' @(
    'frame_pacer_selects_vsync_only_for_integer_refresh_multiples',
    'frame_pacer_drops_missed_slots_and_never_replays_them',
    'PacingMode::vsync', 'PacingMode::deadline'
)

$cmake = Read-Required 'CMakeLists.txt'
Require-Text $cmake 'CMakeLists.txt' @(
    'tests/unit/test_frame_pacer.cpp', 'task_T16', 'native/core/src/frame_pacer.cpp'
)

# 4. The runtime test encodes the gate.
$runtimeTest = Read-Required 'app/tests/runtime.gd'
Require-Text $runtimeTest 'app/tests/runtime.gd' @(
    'PACED_SECONDS := 10.0', 'MAX_P99_MS := 16.6', 'MAX_BURST_FRAMES := 2',
    'is_preview_a_copy', 'get_preview_render_count', 'get_viewport_render_count',
    'ViewportTexture', 'set_consumer_connected', 'set_debug_hud_visible'
)

# 5. The measured JSON from scripts/smoke.ps1.
$jsonPath = Join-Path $root '.tools/T16-runtime.json'
if (-not (Test-Path -LiteralPath $jsonPath)) {
    Add-Failure "missing runtime JSON: $jsonPath (scripts/smoke.ps1 -Runtime must run first)"
} else {
    try {
        $summary = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json
    } catch {
        Add-Failure "runtime JSON is not parseable: $($_.Exception.Message)"
        $summary = $null
    }
    if ($null -ne $summary) {
        if ($summary.schema -ne 'avatar_runtime.result.v1') {
            Add-Failure "runtime JSON schema is '$($summary.schema)', expected avatar_runtime.result.v1"
        }
        if ($summary.passed -ne $true) {
            Add-Failure 'runtime JSON reports passed=false'
        }
        if ([int]$summary.engine.max_fps -ne 60) {
            Add-Failure "runtime JSON max_fps is '$($summary.engine.max_fps)', expected 60"
        }
        if ($summary.engine.low_processor_mode -ne $false) {
            Add-Failure 'runtime JSON reports low-processor mode enabled'
        }
        if ($summary.engine.renderer -ne 'forward_plus') {
            Add-Failure "runtime JSON renderer is '$($summary.engine.renderer)', expected forward_plus"
        }
        $size = @($summary.viewport.size)
        if ($size.Count -ne 2 -or [int]$size[0] -ne 1920 -or [int]$size[1] -ne 1080) {
            Add-Failure "runtime JSON viewport size is '$($size -join 'x')', expected 1920x1080"
        }
        if ($summary.viewport.transparent -ne $true) {
            Add-Failure 'runtime JSON reports a non-transparent viewport'
        }
        if ([int]$summary.viewport.update_mode -ne 4) {
            Add-Failure "runtime JSON update mode is '$($summary.viewport.update_mode)', expected 4 (ALWAYS)"
        }
        if ([int]$summary.viewport.count -ne 1) {
            Add-Failure "runtime JSON reports $($summary.viewport.count) SubViewports, expected 1"
        }
        if ($summary.preview.is_copy -ne $true) {
            Add-Failure 'runtime JSON does not prove the preview is a copy'
        }
        if ([int]$summary.preview.preview_render_count -ne 0) {
            Add-Failure "runtime JSON preview render count is '$($summary.preview.preview_render_count)', expected 0"
        }
        if ([int]$summary.preview.viewport_render_count -le 0) {
            Add-Failure 'runtime JSON has no avatar viewport render count'
        }
        if ($summary.presentation.yaw_180 -ne $true) {
            Add-Failure 'runtime JSON does not record the 180 degree presentation transform'
        }
        if ($summary.pacing.mode -notin @('vsync', 'deadline')) {
            Add-Failure "runtime JSON pacing mode is '$($summary.pacing.mode)'"
        }
        $samples = [int]$summary.pacing.samples
        if ($samples -lt 500) {
            Add-Failure "runtime JSON has $samples pacing samples, expected at least 500"
        }
        $frameTimeP99 = [double]$summary.pacing.frame_time_p99_ms
        if ($frameTimeP99 -gt 16.6) {
            Add-Failure "frame-time p99 is $frameTimeP99 ms, expected <= 16.6 ms"
        }
        $jitterP99 = [double]$summary.pacing.interval_jitter_p99_ms
        if ($jitterP99 -gt 16.6) {
            Add-Failure "interval-jitter p99 is $jitterP99 ms, expected <= 16.6 ms"
        }
        $target = [double]$summary.pacing.target_interval_ms
        $maxInterval = [double]$summary.pacing.max_interval_ms
        if ($maxInterval -gt ($target * 3.0)) {
            Add-Failure "max interval is $maxInterval ms, longer than two missed frames"
        }
        $maxBurst = [int]$summary.pacing.max_burst_frames
        if ($maxBurst -gt 2) {
            Add-Failure "max catch-up burst is $maxBurst frames, expected <= 2"
        }
        Write-Host (("T16 gate: measured {0} samples, mode={1}, frame_time_p99={2:N3} ms, " +
            "interval_p99={3:N3} ms, interval_jitter_p99={4:N3} ms, max_interval={5:N3} ms, " +
            "max_burst={6}, dropped={7}") -f $samples, $summary.pacing.mode, $frameTimeP99,
            [double]$summary.pacing.interval_p99_ms, $jitterP99, $maxInterval, $maxBurst,
            [int]$summary.pacing.dropped)
    }
}

# 6. Design and progress documents.
$architecture = Read-Required 'docs/design/architecture.md'
Require-Text $architecture 'docs/design/architecture.md' @(
    'T16', 'deadline scheduler', 'ViewportTexture', 'App Nap'
)

$testing = Read-Required 'docs/design/testing-and-quality.md'
Require-Text $testing 'docs/design/testing-and-quality.md' @(
    'tests/gates/T16.ps1', 'T16', '16.6 ms', 'preview'
)

$map = Read-Required 'docs/design/implementation-map.md'
if ($map -ne '') {
    if ($map -notmatch 'current as of T(\d{2,3})') {
        Add-Failure 'implementation-map.md has no "current as of TNN" marker'
    } elseif ([int]$Matches[1] -lt 16) {
        Add-Failure "implementation-map.md is stale (current as of T$($Matches[1]), expected T16 or later)"
    }
    if ($map -notmatch '(?m)^\|\s*Rendering strategy: one viewport[^\r\n]*\|\s*`Implemented`[^\r\n]*\|') {
        Add-Failure 'implementation-map.md does not mark the one-viewport/preview/pacing row Implemented'
    }
    if ($map -notmatch '(?m)^\|\s*\[ADR-0026\]\(adr/0026-fixed-60hz-pacing-and-one-viewport-preview\.md\)[^\r\n]*\|\s*Accepted\s*\|') {
        Add-Failure 'implementation-map.md does not list ADR-0026 as Accepted'
    }
}

$adr = Read-Required 'docs/design/adr/0026-fixed-60hz-pacing-and-one-viewport-preview.md'
Require-Text $adr 'ADR-0026' @('# ADR-0026', '**Status:** Accepted', 'Engine.max_fps', 'deadline')

$adrIndex = Read-Required 'docs/design/adr/README.md'
Require-Text $adrIndex 'docs/design/adr/README.md' @('0026', 'Fixed 60 Hz')

$progressPath = Join-Path $root 'docs/progress/T16.md'
if (Test-Path -LiteralPath $progressPath) {
    $progress = Get-Content -LiteralPath $progressPath -Raw
    foreach ($section in @('## What landed', '## Engine knobs', '## Measured result',
                           '## Tests', '## Gotchas', '## Follow-ups', '## Hand-off')) {
        if ($progress -notmatch [regex]::Escape($section)) {
            Add-Failure "docs/progress/T16.md is missing section '$section'"
        }
    }
    foreach ($needle in @('frame_time_p99', 'interval_jitter_p99', 'max_burst', '16.6')) {
        if ($progress -notmatch [regex]::Escape($needle)) {
            Add-Failure "docs/progress/T16.md is missing '$needle'"
        }
    }
} else {
    Write-Host 'T16 gate: note - docs/progress/T16.md not present (gitignored workspace note); skipped'
}

# 7. Design links and formatting.
& node (Join-Path $root 'scripts/check-design-links.mjs')
if ($LASTEXITCODE -ne 0) {
    Add-Failure 'design-link check failed after the T16 documentation change'
}

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts/format-check.ps1')
if ($LASTEXITCODE -ne 0) {
    Add-Failure 'formatting drift after the T16 implementation'
}

if ($failures.Count -gt 0) {
    Write-Host 'T16 gate: FAIL' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

$mode = if ($env:AVATAR_VERIFY_MODE) { $env:AVATAR_VERIFY_MODE } else { 'gate-only' }
Write-Host ("T16 gate: PASS ($mode; one viewport, preview copy, 60 Hz pacing, C++ HUD, " +
    'measured run, design links and formatting)')
exit 0
