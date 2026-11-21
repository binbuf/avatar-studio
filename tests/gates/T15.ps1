<#
.SYNOPSIS
    T15 gate: the M0 audit's contract and documentation claims are true.

.DESCRIPTION
    Runs after the native build and the fast tests. T15 is the M0 gap audit, so
    this gate is a regression check on the audit itself rather than a new
    measurement:

      1. The renderer-facing contract fields exist in the headers: frame
         timestamps, format/colour-space, ring ownership, tracking source
         timestamp, features_valid, stats and the failure-code registry.
      2. The fast suite contains the contract tests that exercise those fields.
      3. The implementation map is current as of T15 (or later), records the
         ADR-0025 Windows-scoped closure, and no longer claims M0 is open on the
         numeric evidence alone.
      4. The realised-behaviour documents no longer carry the pre-ADR-0025
         statements and describe the actual VideoFrame timestamp split.
      5. The M0 short harness produced a valid JSON report for both sizes.
      6. docs/progress/T15.md, when present, carries the audit, gap list,
         readiness statement and hand-off sections.
      7. The design-link and formatting checks pass.

    The full soak and the hosted three-platform CI state are T14/T52/T55
    evidence, not something this fast gate can re-measure. A synthetic short run
    is called out explicitly and is not treated as tracker evidence.

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
    # Normalise whitespace so a phrase that wraps across a markdown line still
    # matches; the check is about the claim, not its line breaks.
    $normalised = ($Text -replace '\s+', ' ')
    foreach ($needle in $Needles) {
        $wanted = ($needle -replace '\s+', ' ')
        if (-not $normalised.Contains($wanted)) {
            Add-Failure "$Path is missing '$needle'"
        }
    }
}

# 1. Header contract.
$video = Read-Required 'native/capture/include/avatar/capture/video_frame.h'
Require-Text $video 'video_frame.h' @(
    'TimestampNs timestamp_ns', 'TimestampNs arrival_ns', 'PixelFormat format',
    'ColorSpace color_space', 'bool mirrored', 'FrameStorage storage',
    'strides', 'planes', 'kMaxFramePlanes')

$ring = Read-Required 'native/capture/include/avatar/capture/frame_ring.h'
Require-Text $ring 'frame_ring.h' @(
    'class FrameLease', 'class FrameWrite', 'begin_write', 'publish',
    'try_acquire_latest', 'dropped()', 'capacity')

$camera = Read-Required 'native/capture/include/avatar/capture/camera_source.h'
Require-Text $camera 'camera_source.h' @(
    'enum class CameraState', 'enum class CameraEventKind', 'take_events',
    'color_space()', 'publish_latency')

$tracking = Read-Required 'native/tracking/include/avatar/tracking/tracking_result.h'
Require-Text $tracking 'tracking_result.h' @(
    'source_timestamp_ns', 'features_valid', 'struct TrackerStats',
    'enum class DropReason', 'ConfidenceDistribution', 'inference_ns')

$abi = Read-Required 'native/tracking/include/avatar/tracker/tracker_c_api.h'
Require-Text $abi 'tracker_c_api.h' @(
    '#define AVATAR_TRACKER_ABI_VERSION 1u', 'source_timestamp_ns',
    'features_valid', 'AVATAR_TRACKER_ERROR_INTERNAL',
    'sizeof(avatar_tracker_result) == 5992')

$diagnostics = Read-Required 'native/core/include/avatar/core/diagnostics.h'
Require-Text $diagnostics 'diagnostics.h' @(
    'cap_001', 'cap_008', 'trk_001', 'trk_009', 'kDiagnosticRegistry')

# 2. Contract tests.
$cameraTest = Read-Required 'tests/unit/test_camera_source.cpp'
Require-Text $cameraTest 'test_camera_source.cpp' @(
    'frame().color_space', 'frame().arrival_ns', 'frame().timestamp_ns',
    'ColorSpace::bt601_limited')

$ringTest = Read-Required 'tests/unit/test_frame_ring.cpp'
Require-Text $ringTest 'test_frame_ring.cpp' @(
    'frame_ring_defaults_colour_space_from_format', 'ColorSpace::bt709_limited',
    'does_not_overwrite_a_pinned_slot', 'arrival_ns')

$trackerTest = Read-Required 'tests/unit/test_tracker_contract.cpp'
Require-Text $trackerTest 'test_tracker_contract.cpp' @(
    'source_timestamp_ns', 'features_valid')

$realTrackerTest = Read-Required 'tests/unit/test_real_tracker.cpp'
Require-Text $realTrackerTest 'test_real_tracker.cpp' @('features_valid')

# 3. Implementation map.
$map = Read-Required 'docs/design/implementation-map.md'
if ($map -ne '') {
    if ($map -notmatch 'current as of T(\d{2,3})') {
        Add-Failure 'implementation-map.md has no "current as of TNN" marker'
    } elseif ([int]$Matches[1] -lt 15) {
        Add-Failure "implementation-map.md is stale (current as of T$($Matches[1]), expected T15 or later)"
    }
    if ($map -notmatch '(?m)^\|\s*M0 Windows exit decision[^\r\n]*\|\s*`Implemented`[^\r\n]*\|') {
        Add-Failure 'implementation-map.md does not mark the M0 Windows exit decision as Implemented'
    }
    if ($map -notmatch '(?m)^\|\s*M0 macOS/Linux real-backend[^\r\n]*\|\s*`Planned`[^\r\n]*\|') {
        Add-Failure 'implementation-map.md does not keep the macOS/Linux real-backend evidence Planned'
    }
    if ($map -notmatch '(?m)^\|\s*M0 Reference/Minimum hardware tier[^\r\n]*\|\s*`Planned`[^\r\n]*\|') {
        Add-Failure 'implementation-map.md does not keep the Reference/Minimum tier evidence Planned'
    }
    if ($map -notmatch '(?m)^\|\s*\[ADR-0025\]\(adr/0025-m0-windows-scope-and-deferred-platform-evidence\.md\)[^\r\n]*\|\s*Accepted\s*\|') {
        Add-Failure 'implementation-map.md does not list ADR-0025 as Accepted'
    }
    if ($map -match 'local numeric evidence alone does not close M0') {
        Add-Failure 'implementation-map.md still claims the numeric evidence alone does not close M0'
    }
}

# 4. Realised-behaviour documents.
$designReadme = Read-Required 'docs/design/README.md'
Require-Text $designReadme 'docs/design/README.md' @('ADR-0025', 'T15')

$trackingDoc = Read-Required 'docs/design/tracking.md'
Require-Text $trackingDoc 'docs/design/tracking.md' @(
    'T15 renderer-facing contract audit', 'source_timestamp_ns', 'features_valid',
    'ABI v1 exposes no statistics query')
if ($trackingDoc -match 'Rendering remains gated on the outstanding M0 evidence') {
    Add-Failure 'tracking.md still says rendering is gated on outstanding M0 evidence'
}

$captureDoc = Read-Required 'docs/design/camera-capture.md'
Require-Text $captureDoc 'docs/design/camera-capture.md' @(
    'does not reorder or reject timestamps', 'CAP-005')
if ($captureDoc -match 'device survey \(T13\)') {
    Add-Failure 'camera-capture.md still assigns the device survey to T13'
}

$architecture = Read-Required 'docs/design/architecture.md'
Require-Text $architecture 'docs/design/architecture.md' @(
    'arrival_ns', 'source timestamp mapped onto FrameClock',
    'app-facing real-tracker statistics')

$testing = Read-Required 'docs/design/testing-and-quality.md'
Require-Text $testing 'docs/design/testing-and-quality.md' @(
    'tests/gates/T15.ps1', 'Windows-scoped M0 contract audit')

$designRoadmap = Read-Required 'docs/design/roadmap.md'
Require-Text $designRoadmap 'docs/design/roadmap.md' @('T15 audited', 'ADR-0025')

# 5. The M0 short harness report.
$jsonPath = Join-Path $root 'tests/performance/results/latest-m0-short.json'
if (-not (Test-Path -LiteralPath $jsonPath)) {
    Add-Failure "missing M0 short JSON: $jsonPath (the avatar_m0_short CTest must run first)"
} else {
    try {
        $summary = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json
    } catch {
        Add-Failure "M0 short JSON is not parseable: $($_.Exception.Message)"
        $summary = $null
    }
    if ($null -ne $summary) {
        if ($summary.schema -ne 'avatar_m0.summary.v1') {
            Add-Failure "M0 short JSON schema is '$($summary.schema)', expected avatar_m0.summary.v1"
        }
        if ($summary.mode -ne 'short') {
            Add-Failure "M0 short JSON mode is '$($summary.mode)', expected short"
        }
        $runs = @($summary.runs)
        if ($runs.Count -ne 2) {
            Add-Failure "M0 short JSON has $($runs.Count) runs, expected 2"
        } else {
            $sizes = @($runs | ForEach-Object { [int]$_.config.input_size }) | Sort-Object
            if (($sizes -join ',') -ne '192,256') {
                Add-Failure "M0 short JSON sizes are '$($sizes -join ',')', expected 192,256"
            }
        }
        foreach ($run in $runs) {
            $label = "run $($run.config.input_size)"
            if ($run.metrics.leak.passed -ne $true) {
                Add-Failure "$label leak check failed"
            }
            if ([double]$run.metrics.dropped.ratio -gt 0.02) {
                Add-Failure "$label dropped frames exceed 2%"
            }
            foreach ($distribution in @('inference_ms', 'frame_age_ms')) {
                if ($null -eq $run.metrics.$distribution -or
                    [int]$run.metrics.$distribution.samples -le 0) {
                    Add-Failure "$label has no $distribution samples"
                } else {
                    $p50 = [double]$run.metrics.$distribution.p50
                    $p95 = [double]$run.metrics.$distribution.p95
                    $p99 = [double]$run.metrics.$distribution.p99
                    if (-not ($p50 -le $p95 -and $p95 -le $p99)) {
                        Add-Failure "$label $distribution percentiles are not ordered"
                    }
                }
            }
            if ($null -eq $run.metrics.frames.features_valid) {
                Add-Failure "$label does not report a features_valid count"
            }
        }
        if ($summary.synthetic -eq $true) {
            Write-Host 'T15 gate: note - the short harness is SYNTHETIC; it proves the harness, not tracker feasibility' -ForegroundColor Yellow
        } else {
            Write-Host 'T15 gate: note - the short harness used the real tracker and replay source' -ForegroundColor Green
        }
    }
}

# 6. The local progress note, when present.
$progressPath = Join-Path $root 'docs/progress/T15.md'
if (Test-Path -LiteralPath $progressPath) {
    $progress = Get-Content -LiteralPath $progressPath -Raw
    foreach ($section in @('## M0 audit', '## Contract audit', '## Gap list', '## M1 readiness',
                           '## Follow-ups', '## Hand-off')) {
        if ($progress -notmatch [regex]::Escape($section)) {
            Add-Failure "docs/progress/T15.md is missing section '$section'"
        }
    }
    foreach ($needle in @('ADR-0025', 'T16', 'severity', 'owner')) {
        if ($progress -notmatch [regex]::Escape($needle)) {
            Add-Failure "docs/progress/T15.md is missing '$needle'"
        }
    }
    $gapCount = [regex]::Matches($progress, '(?m)^\d+\.\s+\*\*G\d+').Count
    if ($gapCount -lt 5) {
        Add-Failure "docs/progress/T15.md lists $gapCount numbered gaps, expected at least 5"
    }
} else {
    Write-Host 'T15 gate: note - docs/progress/T15.md not present (gitignored workspace note); skipped'
}

# 7. Design links and formatting.
& node (Join-Path $root 'scripts/check-design-links.mjs')
if ($LASTEXITCODE -ne 0) {
    Add-Failure 'design-link check failed after the T15 documentation change'
}

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts/format-check.ps1')
if ($LASTEXITCODE -ne 0) {
    Add-Failure 'formatting drift remains after the T15 audit'
}

if ($failures.Count -gt 0) {
    Write-Host 'T15 gate: FAIL' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

$mode = if ($env:AVATAR_VERIFY_MODE) { $env:AVATAR_VERIFY_MODE } else { 'gate-only' }
Write-Host ("T15 gate: PASS ($mode; renderer-facing contracts, M0 map/docs, short harness report, " +
    'design links and formatting)')
exit 0
