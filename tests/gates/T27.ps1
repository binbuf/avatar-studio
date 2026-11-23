#requires -Version 5.1
<#
T27: the M1 threshold reader.

Reads the M1 golden and soak JSON (written by app/tests/m1_golden.gd and
app/tests/m1_soak.gd) and applies the documented thresholds itself. It does not
trust the harness's own `passed` flag: every metric is re-checked here, so a
future T27 blind review and this task share one definition.

  -SoakPath    default .tools/T26-soak.json
  -GoldenPath  default .tools/T26-golden.json
  -MinSeconds  minimum measured soak duration (default 600)
  -AllowShort  accept a shorter soak (fast verification only)

Thresholds (docs/design/testing-and-quality.md):
  frame-time p99 <= 16.6 ms, interval-jitter p99 <= 16.6 ms,
  app latency p50 <= 70 ms / p95 <= 100 ms, RSS growth <= 5%,
  0 dropped output frames, pacing drift <= 5%.
  Golden SSIM >= 0.995 and decal-bbox max delta <= 8 for every case.
#>
[CmdletBinding()]
param(
    [string]$SoakPath = '.tools/T26-soak.json',
    [string]$GoldenPath = '.tools/T26-golden.json',
    [double]$MinSeconds = 600,
    [switch]$AllowShort
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $root
try {
    function Get-Prop($Object, [string]$Name) {
        if ($null -eq $Object) { return $null }
        $property = $Object.PSObject.Properties[$Name]
        if ($null -eq $property) { return $null }
        return $property.Value
    }

    function Assert-AtMost([string]$Label, $Value, [double]$Limit) {
        if ($null -eq $Value) { throw "$Label is missing" }
        $number = [double]$Value
        if ($number -gt $Limit) {
            throw ("{0} {1} exceeds {2}" -f $Label, $number, $Limit)
        }
        Write-Host ("  ok  {0} = {1} <= {2}" -f $Label, $number, $Limit)
    }

    if (-not (Test-Path -LiteralPath $SoakPath)) { throw "Missing soak JSON: $SoakPath" }
    if (-not (Test-Path -LiteralPath $GoldenPath)) { throw "Missing golden JSON: $GoldenPath" }

    $soak = Get-Content -LiteralPath $SoakPath -Raw | ConvertFrom-Json
    if ([string](Get-Prop $soak 'schema') -ne 'avatar_bench.result.v1') {
        throw "Soak JSON schema is not avatar_bench.result.v1"
    }
    $config = Get-Prop $soak 'config'
    $measured = [double](Get-Prop $config 'seconds')
    if ($measured -lt $MinSeconds -and -not $AllowShort) {
        throw "Soak measured $measured s, below the $MinSeconds s minimum"
    }
    if ([string](Get-Prop $soak 'scenario') -ne 'live') { throw 'Soak scenario is not live' }
    if ($null -eq (Get-Prop (Get-Prop $soak 'hardware') 'fingerprint')) {
        throw 'Soak hardware fingerprint is missing'
    }
    $source = Get-Prop $config 'source'
    if ([string](Get-Prop $source 'tracker_available') -ne 'True') {
        throw 'Soak did not have a tracker available'
    }

    $metrics = Get-Prop $soak 'metrics'
    Assert-AtMost 'frame-time p99 ms' (Get-Prop (Get-Prop $metrics 'frame_time_ms') 'p99') 16.6
    Assert-AtMost 'interval-jitter p99 ms' (Get-Prop (Get-Prop $metrics 'interval_jitter_ms') 'p99') 16.6
    Assert-AtMost 'app latency p50 ms' (Get-Prop (Get-Prop $metrics 'app_latency_ms') 'p50') 70.0
    Assert-AtMost 'app latency p95 ms' (Get-Prop (Get-Prop $metrics 'app_latency_ms') 'p95') 100.0
    Assert-AtMost 'RSS growth percent' (Get-Prop $metrics 'rss_growth_percent') 5.0
    Assert-AtMost 'dropped output frames' (Get-Prop (Get-Prop $metrics 'output') 'dropped') 0
    $pacing = Get-Prop $soak 'pacing'
    Assert-AtMost 'pacing drift percent' ([math]::Abs([double](Get-Prop $pacing 'drift_percent'))) 5.0
    $modes = Get-Prop $pacing 'display_modes'
    foreach ($mode in @('60', '120', '144')) {
        if ($null -eq (Get-Prop $modes $mode)) { throw "Pacing evidence for $mode Hz is missing" }
    }
    $appSamples = [int](Get-Prop (Get-Prop $metrics 'app_latency_ms') 'samples')
    if ($appSamples -lt 10) { throw "App latency has only $appSamples samples" }

    $golden = Get-Content -LiteralPath $GoldenPath -Raw | ConvertFrom-Json
    if ([string](Get-Prop $golden 'schema') -ne 'avatar_m1_golden.result.v1') {
        throw 'Golden JSON schema is not avatar_m1_golden.result.v1'
    }
    if (-not [bool](Get-Prop $golden 'passed')) { throw 'Golden suite did not pass' }
    if (-not [bool](Get-Prop $golden 'rendered')) { throw 'Golden suite was not rendered' }
    if ([string](Get-Prop $golden 'renderer') -ne 'forward_plus') {
        throw 'Golden suite did not run on Forward+'
    }
    $goldens = [int](Get-Prop $golden 'goldens')
    if ($goldens -lt 28) { throw "Golden suite has $goldens cases, expected at least 28" }
    $cases = @(Get-Prop $golden 'cases')
    $imageCases = @($cases | Where-Object { $null -ne (Get-Prop $_ 'ssim') })
    if ($imageCases.Count -lt 28) {
        throw "Golden JSON lists $($imageCases.Count) compared cases, expected 28"
    }
    foreach ($case in $imageCases) {
        $name = [string](Get-Prop $case 'name')
        $ssim = [double](Get-Prop $case 'ssim')
        $delta = [int](Get-Prop $case 'decal_max_delta')
        if ($ssim -lt 0.995) { throw "Golden $name SSIM $ssim is below 0.995" }
        if ($delta -gt 8) { throw "Golden $name decal max delta $delta exceeds 8" }
    }
    if (Test-Path -LiteralPath '.tools/T26-golden-artifacts') {
        $artifacts = @(Get-ChildItem -LiteralPath '.tools/T26-golden-artifacts' -File -ErrorAction SilentlyContinue)
        if ($artifacts.Count -gt 0) {
            throw "Golden artifacts were attached for a passing run ($($artifacts.Count) files)"
        }
    }
    Write-Host ("T27 thresholds: PASS ({0} goldens, {1} s soak, app p50 {2} ms / p95 {3} ms)" -f `
            $goldens, $measured, (Get-Prop (Get-Prop $metrics 'app_latency_ms') 'p50'), `
            (Get-Prop (Get-Prop $metrics 'app_latency_ms') 'p95'))
} finally { Pop-Location }
