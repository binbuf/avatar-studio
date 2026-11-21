<#
.SYNOPSIS
    T14 numeric gate: apply M0 feasibility thresholds to the latest M0 soak JSON.

.DESCRIPTION
    Reads tests/performance/results/latest-m0-full.json (written by
    tests/m0/run.ps1 -Mode full) and applies the M0 exit criteria from
    docs/design/roadmap.md plus the harness-specific bounds recorded in
    ADR-0023:

      Inference p95  <= 15 ms (reference tier) / 30 ms (minimum tier)
      Inference p99  <= 25 ms / 45 ms
      Frame age p95  <= 50 ms / 80 ms
      Frame age p99  <= 100 ms / 150 ms
      Dropped frames <= 2%
      RSS growth     <= 5% after warmup
      Handles        not monotonically growing
      Duration       >= 1800 s total, >= 600 s per input size
      Evidence       real tracker, real replay source, both 192 and 256

    A synthetic or short run fails the gate: it is not M0 evidence. T13 owns the
    harness smoke; T14 owns the pass/pivot decision.
    This checks the numeric soak subset only. CI builds, hardware equivalence,
    long-term pose interpretation and subjective pose approval belong in the
    T14 decision record; this script does not certify them.

.PARAMETER ResultPath
    Combined M0 JSON. Defaults to the latest full soak.

.PARAMETER AllowShort
    Permit a short-mode summary. This is for harness debugging only; the M0
    gate requires the full soak.
#>
[CmdletBinding()]
param(
    [string]$ResultPath = 'tests/performance/results/latest-m0-full.json',
    [switch]$AllowShort
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$failures = New-Object System.Collections.Generic.List[string]

function Add-Failure([string]$Message) {
    $script:failures.Add($Message)
}

$inputPath = if ([System.IO.Path]::IsPathRooted($ResultPath)) { $ResultPath } else { Join-Path $root $ResultPath }
if (-not (Test-Path -LiteralPath $inputPath)) {
    Write-Host "T14 gate: FAIL - missing M0 JSON: $inputPath" -ForegroundColor Red
    Write-Host 'Run: powershell -NoProfile -ExecutionPolicy Bypass -File tests/m0/run.ps1 -Mode full'
    exit 1
}

try {
    $data = Get-Content -LiteralPath $inputPath -Raw | ConvertFrom-Json
} catch {
    Write-Host "T14 gate: FAIL - cannot parse $inputPath : $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

$runs = @()
$mode = ''
$synthetic = $false
$totalDuration = 0.0
if ($data.schema -eq 'avatar_m0.summary.v1') {
    $runs = @($data.runs)
    $mode = [string]$data.mode
    $synthetic = [bool]$data.synthetic
    $totalDuration = [double]$data.total_duration_s
} elseif ($data.schema -eq 'avatar_m0.result.v1') {
    $runs = @($data)
    $mode = [string]$data.mode
    $synthetic = [bool]$data.tracker.synthetic -or [bool]$data.source.synthetic
    $totalDuration = [double]$data.config.duration_s
} else {
    Add-Failure "unknown JSON schema '$($data.schema)'"
}

if ($mode -ne 'full' -and -not $AllowShort) {
    Add-Failure "mode is '$mode'; the M0 gate requires the full soak"
}
if ($synthetic) {
    Add-Failure 'the run is synthetic (scripted tracker or pattern source); it is not M0 tracker evidence'
}
if ($runs.Count -ne 2) {
    Add-Failure "found $($runs.Count) runs, expected 2 (192 and 256)"
} else {
    $sizes = @($runs | ForEach-Object { [int]$_.config.input_size }) | Sort-Object
    if (($sizes -join ',') -ne '192,256') {
        Add-Failure "input sizes are '$($sizes -join ',')', expected 192,256"
    }
}
if ($totalDuration -lt 1800.0 -and -not $AllowShort) {
    Add-Failure "total measured duration is $totalDuration s, expected >= 1800 s"
}

$table = New-Object System.Collections.Generic.List[string]
$table.Add('| Input | Tier | Inference p50/p95/p99 | Frame age p50/p95/p99 | Dropped | RSS growth | Handles | Leak |')
$table.Add('|---|---|---|---|---:|---:|---|---|')

foreach ($run in $runs) {
    $label = "input $($run.config.input_size)"
    $tier = [string]$run.hardware.tier
    if ($tier -eq 'reference') {
        $inferenceP95Limit = 15.0
        $inferenceP99Limit = 25.0
        $ageP95Limit = 50.0
        $ageP99Limit = 100.0
    } elseif ($tier -eq 'minimum') {
        $inferenceP95Limit = 30.0
        $inferenceP99Limit = 45.0
        $ageP95Limit = 80.0
        $ageP99Limit = 150.0
    } else {
        Add-Failure "$label has an unknown hardware tier '$tier'"
        $inferenceP95Limit = 15.0
        $inferenceP99Limit = 25.0
        $ageP95Limit = 50.0
        $ageP99Limit = 100.0
    }

    if ($run.tracker.kind -ne 'real') {
        Add-Failure "$label tracker is '$($run.tracker.kind)', expected real"
    }
    if ($run.tracker.synthetic -eq $true) {
        Add-Failure "$label tracker is labelled synthetic"
    }
    if ($run.source.synthetic -eq $true) {
        Add-Failure "$label source is labelled synthetic"
    }
    if ([double]$run.config.duration_s -lt 600.0 -and -not $AllowShort) {
        Add-Failure "$label measured $($run.config.duration_s) s, expected >= 600 s"
    }

    $inferenceP50 = [double]$run.metrics.inference_ms.p50
    $inferenceP95 = [double]$run.metrics.inference_ms.p95
    $inferenceP99 = [double]$run.metrics.inference_ms.p99
    $ageP50 = [double]$run.metrics.frame_age_ms.p50
    $ageP95 = [double]$run.metrics.frame_age_ms.p95
    $ageP99 = [double]$run.metrics.frame_age_ms.p99
    if ($inferenceP95 -gt $inferenceP95Limit) {
        Add-Failure ("$label inference p95 {0:N3} ms exceeds {1:N1} ms" -f $inferenceP95, $inferenceP95Limit)
    }
    if ($inferenceP99 -gt $inferenceP99Limit) {
        Add-Failure ("$label inference p99 {0:N3} ms exceeds {1:N1} ms" -f $inferenceP99, $inferenceP99Limit)
    }
    if ($ageP95 -gt $ageP95Limit) {
        Add-Failure ("$label frame age p95 {0:N3} ms exceeds {1:N1} ms" -f $ageP95, $ageP95Limit)
    }
    if ($ageP99 -gt $ageP99Limit) {
        Add-Failure ("$label frame age p99 {0:N3} ms exceeds {1:N1} ms" -f $ageP99, $ageP99Limit)
    }
    if ([double]$run.metrics.dropped.ratio -gt 0.02) {
        Add-Failure ("$label dropped {0:P3} of frames, expected <= 2%" -f [double]$run.metrics.dropped.ratio)
    }
    if ($run.metrics.leak.passed -ne $true) {
        Add-Failure "$label leak check failed"
    }
    if ([double]$run.metrics.leak.rss_growth_percent -gt 5.0) {
        Add-Failure ("$label RSS grew {0:N2}% after warmup, expected <= 5%" -f [double]$run.metrics.leak.rss_growth_percent)
    }
    if ($run.metrics.leak.handle_monotonic_growth -eq $true) {
        Add-Failure "$label handle count grew monotonically"
    }
    $tracePath = [string]$run.artifacts.pose_trace
    if ($tracePath -eq '' -or -not (Test-Path -LiteralPath $tracePath)) {
        Add-Failure "$label pose trace is missing: $tracePath"
    }
    $clipDir = [string]$run.artifacts.clip_dir
    if ($clipDir -eq '' -or -not (Test-Path -LiteralPath $clipDir)) {
        Add-Failure "$label clip directory is missing: $clipDir"
    } elseif (@(Get-ChildItem -LiteralPath $clipDir -Filter '*.bmp').Count -lt 1) {
        Add-Failure "$label clip directory has no BMP frames"
    }

    $table.Add(("| {0}x{0} | {1} | {2:N3} / {3:N3} / {4:N3} ms | {5:N3} / {6:N3} / {7:N3} ms | {8:P3} | {9:N2}% | {10} -> {11} | {12} |" -f `
        [int]$run.config.input_size, $tier,
        $inferenceP50, $inferenceP95, $inferenceP99,
        $ageP50, $ageP95, $ageP99,
        [double]$run.metrics.dropped.ratio,
        [double]$run.metrics.rss.growth_percent,
        [int]$run.metrics.leak.handle_baseline,
        [int]$run.metrics.leak.handle_final,
        $(if ($run.metrics.leak.passed -eq $true) { 'pass' } else { 'FAIL' })))
}

Write-Host 'T14 gate: numeric M0 thresholds (platform builds and human pose approval are separate)'
Write-Host ("  input: $inputPath")
Write-Host ("  mode: $mode | synthetic: $synthetic | total: $totalDuration s")
foreach ($line in $table) {
    Write-Host "  $line"
}

if ($failures.Count -gt 0) {
    Write-Host 'T14 gate: FAIL' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

if ($AllowShort) {
    Write-Host 'T14 gate: PASS (short harness evidence; this is not the M0 gate)' -ForegroundColor Yellow
} else {
    Write-Host 'T14 gate: PASS (numeric M0 thresholds met on real soak evidence; full M0 decision requires platform and human review evidence)' -ForegroundColor Green
}
exit 0
