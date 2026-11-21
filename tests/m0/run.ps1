<#
.SYNOPSIS
    Run the Avatar Studio headless M0 harness.

.DESCRIPTION
    Builds (unless -NoBuild) and runs avatar_m0 at 192x192 and 256x256 with a
    real or scripted tracker, writes one JSON per size plus a combined summary
    under tests/performance/results/ (gitignored), and writes a human summary.

    Modes:
      short  the 2-minute CI run (60 s per size, 5 s warmup, 5 s RSS samples).
      full   the 30-minute soak (15 min per size, 10 s warmup, 60 s RSS samples).
             Leak or monotonic handle growth fails the run.

    The real backend is used only when libavatartracker.dll and the T11 replay
    clip are present. Otherwise the harness falls back to ScriptedTracker and
    labels the result synthetic; that is harness evidence, not tracker evidence.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tests/m0/run.ps1 -Mode short

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tests/m0/run.ps1 -Mode full
#>
[CmdletBinding()]
param(
    [ValidateSet('short', 'full')]
    [string]$Mode = 'short',
    [double]$DurationMinutes = 0,
    [double]$WarmupSeconds = -1,
    [int]$Fps = 30,
    [ValidateSet('auto', 'real', 'scripted')]
    [string]$Tracker = 'auto',
    [ValidateSet('auto', 'replay', 'pattern')]
    [string]$Source = 'auto',
    [string]$Replay = '.tools/t11/live.avclip',
    [string]$BuildDir = 'build',
    [string]$Config = 'RelWithDebInfo',
    [string]$OutDir = 'tests/performance/results',
    [string]$SummaryPath = '',
    [switch]$NoBuild,
    [switch]$NoSummary,
    [switch]$NoStrict
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

function Write-Command([string]$Text) {
    Write-Host ">> $Text" -ForegroundColor Cyan
}

function Resolve-RootedPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return (Join-Path $root $Path)
}

function Format-Number([object]$Value, [string]$Format = '0.00') {
    if ($null -eq $Value) {
        return 'n/a'
    }
    return ([double]$Value).ToString($Format)
}

function Quote-Argument([string]$Value) {
    if ($Value -match '[\s|;&<>]') {
        return '"{0}"' -f $Value
    }
    return $Value
}

$onWindows = ([System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT)

# Mode defaults: the two modes are the two documented budgets, not arbitrary
# multipliers. A caller can still override the total duration.
$totalMinutes = if ($DurationMinutes -gt 0) { $DurationMinutes } else { if ($Mode -eq 'full') { 30.0 } else { 2.0 } }
$perSizeSeconds = ($totalMinutes * 60.0) / 2.0
$warmup = if ($WarmupSeconds -ge 0) { $WarmupSeconds } else { if ($Mode -eq 'full') { 60.0 } else { 5.0 } }
$rssInterval = if ($Mode -eq 'full') { 60.0 } else { 5.0 }
$strict = ($Mode -eq 'full') -and (-not $NoStrict)
$summaryDefault = if ($Mode -eq 'full') { 'docs/progress/T13-m0-results.md' } else { 'docs/progress/T13-m0-short.md' }
if ($SummaryPath -eq '') {
    $SummaryPath = $summaryDefault
}

$buildRoot = Resolve-RootedPath $BuildDir
$results = Resolve-RootedPath $OutDir
New-Item -ItemType Directory -Force -Path $results | Out-Null
$summaryFile = Resolve-RootedPath $SummaryPath
$summaryDir = Split-Path -Parent $summaryFile
if ($summaryDir -and -not (Test-Path -LiteralPath $summaryDir)) {
    New-Item -ItemType Directory -Force -Path $summaryDir | Out-Null
}

if (-not $NoBuild) {
    Write-Command "cmake -S . -B $BuildDir -DAVATAR_TESTS=ON -DAVATAR_BENCHMARKS=ON"
    & cmake -S . -B $BuildDir -DAVATAR_TESTS=ON -DAVATAR_BENCHMARKS=ON
    if ($LASTEXITCODE -ne 0) {
        throw "cmake configure failed with exit code $LASTEXITCODE"
    }
    Write-Command "cmake --build $BuildDir --config $Config --target avatar_m0 --parallel"
    & cmake --build $BuildDir --config $Config --target avatar_m0 --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "cmake build failed with exit code $LASTEXITCODE"
    }
}

$exeCandidates = @(
    (Join-Path $buildRoot "$Config/avatar_m0.exe"),
    (Join-Path $buildRoot 'avatar_m0.exe'),
    (Join-Path $buildRoot "$Config/avatar_m0"),
    (Join-Path $buildRoot 'avatar_m0')
)
$exe = $exeCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $exe) {
    throw "avatar_m0 executable not found under $buildRoot (build first or drop -NoBuild)"
}

$replayPath = Resolve-RootedPath $Replay
$dllCandidates = @(
    (Join-Path $buildRoot "$Config/libavatartracker.dll"),
    (Join-Path $buildRoot 'libavatartracker.dll'),
    (Resolve-RootedPath '.tools/tracker-sdk/libavatartracker.dll')
)
$trackerDll = $dllCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1

$trackerKind = $Tracker
if ($Tracker -eq 'auto') {
    if ($onWindows -and $trackerDll -and (Test-Path -LiteralPath $replayPath)) {
        $trackerKind = 'real'
    } else {
        $trackerKind = 'scripted'
    }
}
if ($trackerKind -eq 'real') {
    if (-not $onWindows) {
        throw 'the real tracker backend is Windows-only today'
    }
    if (-not $trackerDll) {
        throw 'libavatartracker.dll not found; run scripts/build-tracker.ps1 or use -Tracker scripted'
    }
}

$sourceKind = $Source
if ($Source -eq 'auto') {
    if ($trackerKind -eq 'real' -and (Test-Path -LiteralPath $replayPath)) {
        $sourceKind = 'replay'
    } else {
        $sourceKind = 'pattern'
    }
}
if ($sourceKind -eq 'replay' -and -not (Test-Path -LiteralPath $replayPath)) {
    throw "replay clip not found: $replayPath"
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$sizes = @(192, 256)
$runs = New-Object System.Collections.Generic.List[object]
$runFailures = New-Object System.Collections.Generic.List[string]
$leakFailures = New-Object System.Collections.Generic.List[string]

foreach ($size in $sizes) {
    $base = "m0-$Mode-$size-$stamp"
    $jsonPath = Join-Path $results "$base.json"
    $tracePath = Join-Path $results "$base-pose.csv"
    $clipPath = Join-Path $results "$base-clip"
    $arguments = @(
        '--source', $sourceKind,
        '--tracker', $trackerKind,
        '--input-size', [string]$size,
        '--duration-seconds', $perSizeSeconds.ToString('0.###', [System.Globalization.CultureInfo]::InvariantCulture),
        '--warmup-seconds', $warmup.ToString('0.###', [System.Globalization.CultureInfo]::InvariantCulture),
        '--rss-interval-seconds', $rssInterval.ToString('0.###', [System.Globalization.CultureInfo]::InvariantCulture),
        '--fps', [string]$Fps,
        '--clip-frames', '48',
        '--output', $jsonPath,
        '--pose-trace', $tracePath,
        '--clip-dir', $clipPath,
        '--mode', $Mode
    )
    if ($sourceKind -eq 'replay') {
        $arguments += @('--replay', $replayPath)
    }
    if ($trackerKind -eq 'real') {
        $arguments += @('--tracker-dll', $trackerDll)
    }
    if ($strict) {
        $arguments += '--strict'
    }

    $argumentLine = ($arguments | ForEach-Object { Quote-Argument ([string]$_) }) -join ' '
    Write-Command ("{0} {1}" -f $exe, $argumentLine)
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $exe
    $startInfo.Arguments = $argumentLine
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    $null = $process.Start()
    try {
        $process.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::Normal
    } catch {
        Write-Warning "could not pin process priority to Normal: $($_.Exception.Message)"
    }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $stdout = $stdoutTask.Result
    $stderr = $stderrTask.Result
    $exitCode = $process.ExitCode
    $process.Dispose()
    if ($stdout) {
        Write-Host $stdout.TrimEnd()
    }
    if ($stderr) {
        Write-Host $stderr.TrimEnd()
    }

    if (-not (Test-Path -LiteralPath $jsonPath)) {
        $runFailures.Add("size $size did not write $jsonPath (exit $exitCode)")
        continue
    }
    $run = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json
    $runs.Add($run)

    $latest = Join-Path $results ("latest-m0-$Mode-$size.json")
    Copy-Item -LiteralPath $jsonPath -Destination $latest -Force
    Write-Host "result: $jsonPath" -ForegroundColor Green

    if ($run.metrics.leak.passed -ne $true) {
        $leakFailures.Add("size $size leak check failed (RSS $($run.metrics.leak.rss_growth_percent)%, handles $($run.metrics.leak.handle_baseline) -> $($run.metrics.leak.handle_final), monotonic $($run.metrics.leak.handle_monotonic_growth))")
    }
    if ($exitCode -ne 0) {
        if ($exitCode -eq 4 -and $Mode -eq 'full') {
            # The JSON exists; the leak failure is recorded above and turns into
            # a non-zero script result after the summary is written.
            Write-Warning "avatar_m0 reported a strict leak failure at size $size (exit 4)"
        } else {
            $runFailures.Add("avatar_m0 failed at size $size with exit code $exitCode")
        }
    }
}

$totalDuration = 0.0
foreach ($run in $runs) {
    $totalDuration += [double]$run.config.duration_s
}

$bothSizes = ($runs.Count -eq 2)
$sizesPresent = @($runs | ForEach-Object { [int]$_.config.input_size }) | Sort-Object
if ($bothSizes -and ($sizesPresent -join ',') -ne '192,256') {
    $bothSizes = $false
}
$allLeakPassed = ($runs.Count -gt 0)
$allDropsWithinBudget = ($runs.Count -gt 0)
$maxInferenceP95 = 0.0
$maxFrameAgeP95 = 0.0
$maxRssGrowth = 0.0
foreach ($run in $runs) {
    if ($run.metrics.leak.passed -ne $true) {
        $allLeakPassed = $false
    }
    if ([double]$run.metrics.dropped.ratio -gt 0.02) {
        $allDropsWithinBudget = $false
    }
    $maxInferenceP95 = [Math]::Max($maxInferenceP95, [double]$run.metrics.inference_ms.p95)
    $maxFrameAgeP95 = [Math]::Max($maxFrameAgeP95, [double]$run.metrics.frame_age_ms.p95)
    $maxRssGrowth = [Math]::Max($maxRssGrowth, [double]$run.metrics.rss.growth_percent)
}

$summary = [ordered]@{
    schema = 'avatar_m0.summary.v1'
    generated_utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    mode = $Mode
    tracker = $trackerKind
    source = $sourceKind
    synthetic = ($trackerKind -ne 'real' -or $sourceKind -ne 'replay')
    total_duration_s = $totalDuration
    per_size_duration_s = $perSizeSeconds
    runs = $runs.ToArray()
    checks = [ordered]@{
        both_sizes = $bothSizes
        all_leak_passed = $allLeakPassed
        dropped_within_budget = $allDropsWithinBudget
        max_inference_p95_ms = [Math]::Round($maxInferenceP95, 4)
        max_frame_age_p95_ms = [Math]::Round($maxFrameAgeP95, 4)
        max_rss_growth_percent = [Math]::Round($maxRssGrowth, 4)
        strict = $strict
    }
}

$summaryJsonPath = Join-Path $results "latest-m0-$Mode.json"
$summaryStampPath = Join-Path $results "m0-$Mode-$stamp.json"
$summaryText = $summary | ConvertTo-Json -Depth 24
Set-Content -LiteralPath $summaryJsonPath -Value $summaryText -Encoding UTF8
Set-Content -LiteralPath $summaryStampPath -Value $summaryText -Encoding UTF8
Write-Host "summary: $summaryJsonPath" -ForegroundColor Green

if (-not $NoSummary) {
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('# T13 - M0 headless harness results')
    $lines.Add('')
    $synthetic = $summary.synthetic
    $resultLabel = if ($runFailures.Count -eq 0 -and ($leakFailures.Count -eq 0 -or -not $strict)) { 'PASS' } else { 'FAIL' }
    $lines.Add("**Mode:** $Mode | **Generated:** $($summary.generated_utc) | **Harness result:** $resultLabel")
    $lines.Add('')
    $lines.Add("**Tracker:** $trackerKind | **Source:** $sourceKind | **Synthetic:** $([bool]$synthetic)")
    if ($runs.Count -gt 0) {
        $hw = $runs[0].hardware
        $lines.Add("**Hardware:** $($hw.os) | $($hw.cpu) | $($hw.logical_cores) logical cores | $([math]::Round([double]$hw.memory_bytes / 1GB, 1)) GiB | tier '$($hw.tier)'")
    }
    $lines.Add('')
    if ($synthetic) {
        $lines.Add('> **Synthetic evidence.** The real tracker DLL or the replay clip was not available. This run exercises the harness; it is not tracker evidence and must not be used for the M0 pass/pivot decision.')
        $lines.Add('')
    }
    $lines.Add('| Input | Duration | Frames | Valid | Inference p50/p95/p99 (ms) | Frame age p50/p95/p99 (ms) | Dropped | RSS growth | Handles | Leak |')
    $lines.Add('|---|---:|---:|---:|---|---|---:|---:|---:|---|')
    foreach ($run in $runs) {
        $m = $run.metrics
        $lines.Add(("| {0}x{0} | {1} s | {2} | {3} | {4} / {5} / {6} | {7} / {8} / {9} | {10} ({11}%) | {12}% | {13} -> {14} | {15} |" -f `
            [int]$run.config.input_size, `
            (Format-Number $run.config.duration_s '0'), `
            [int]$m.frames.resolved, `
            [int]$m.frames.valid, `
            (Format-Number $m.inference_ms.p50 '0.000'), `
            (Format-Number $m.inference_ms.p95 '0.000'), `
            (Format-Number $m.inference_ms.p99 '0.000'), `
            (Format-Number $m.frame_age_ms.p50 '0.000'), `
            (Format-Number $m.frame_age_ms.p95 '0.000'), `
            (Format-Number $m.frame_age_ms.p99 '0.000'), `
            [int]$m.dropped.total, `
            (Format-Number ($m.dropped.ratio * 100.0) '0.000'), `
            (Format-Number $m.rss.growth_percent '0.00'), `
            [int]$m.leak.handle_baseline, `
            [int]$m.leak.handle_final, `
            $(if ($m.leak.passed) { 'pass' } else { 'FAIL' })))
    }
    $lines.Add('')
    $lines.Add('## M0 thresholds (applied by tests/gates/T14.ps1)')
    $lines.Add('')
    $lines.Add('| Check | Threshold | Measured | Verdict |')
    $lines.Add('|---|---|---|---|')
    $tier = if ($runs.Count -gt 0) { [string]$runs[0].hardware.tier } else { 'unknown' }
    $p95Limit = if ($tier -eq 'reference') { 15.0 } else { 30.0 }
    $ageLimit = if ($tier -eq 'reference') { 50.0 } else { 80.0 }
    $lines.Add("| Inference p95 | <= $p95Limit ms ($tier tier) | $(Format-Number $maxInferenceP95 '0.000') ms | $(if ($maxInferenceP95 -le $p95Limit) { 'pass' } else { 'FAIL' }) |")
    $lines.Add("| Frame age p95 | <= $ageLimit ms ($tier tier) | $(Format-Number $maxFrameAgeP95 '0.000') ms | $(if ($maxFrameAgeP95 -le $ageLimit) { 'pass' } else { 'FAIL' }) |")
    $lines.Add("| Dropped frames | <= 2% | $(if ($allDropsWithinBudget) { 'within budget' } else { 'over budget' }) | $(if ($allDropsWithinBudget) { 'pass' } else { 'FAIL' }) |")
    $lines.Add("| RSS growth after warmup | <= 5% | $(Format-Number $maxRssGrowth '0.00')% | $(if ($allLeakPassed) { 'pass' } else { 'FAIL' }) |")
    $lines.Add("| Handle count | not monotonically growing | $(if ($allLeakPassed) { 'not growing' } else { 'grew monotonically' }) | $(if ($allLeakPassed) { 'pass' } else { 'FAIL' }) |")
    $lines.Add("| Soak duration | >= 1800 s (full only) | $(Format-Number $totalDuration '0') s | $(if ($Mode -eq 'full' -and $totalDuration -ge 1800) { 'pass' } elseif ($Mode -eq 'full') { 'FAIL' } else { 'n/a (short)' }) |")
    $lines.Add('')
    $lines.Add('## Artifacts')
    $lines.Add('')
    $lines.Add("- Combined JSON: ``$summaryJsonPath``")
    foreach ($run in $runs) {
        $lines.Add("- $([int]$run.config.input_size) JSON: ``$($run.artifacts.pose_trace)`` trace, ``$($run.artifacts.pose_svg)`` chart, ``$($run.artifacts.clip_dir)`` clip")
    }
    $lines.Add('')
    $lines.Add('## Human review at T14')
    $lines.Add('')
    $lines.Add('Watch the BMP clip at normal speed and slowed down. Confirm: no visible jitter at rest, no swimming under motion, no left/right swap. The pose chart shows yaw (red), pitch (green) and roll (blue) over time; the CSV has every measured pose.')
    if ($sourceKind -eq 'replay') {
        $lines.Add('')
        $lines.Add('The replay clip loops. Ignore the pose discontinuity at each loop boundary; it is source content, not tracker jitter.')
    }
    if ($runFailures.Count -gt 0) {
        $lines.Add('')
        $lines.Add('## Failures')
        $lines.Add('')
        foreach ($failure in $runFailures) {
            $lines.Add("- $failure")
        }
    }
    if ($leakFailures.Count -gt 0) {
        $lines.Add('')
        $lines.Add('## Leak failures')
        $lines.Add('')
        foreach ($failure in $leakFailures) {
            $lines.Add("- $failure")
        }
    }
    Set-Content -LiteralPath $summaryFile -Value ($lines -join "`n") -Encoding UTF8
    Write-Host "human summary: $summaryFile" -ForegroundColor Green
}

if ($runFailures.Count -gt 0) {
    foreach ($failure in $runFailures) {
        Write-Host "FAIL: $failure" -ForegroundColor Red
    }
    exit 1
}
if ($strict -and $leakFailures.Count -gt 0) {
    foreach ($failure in $leakFailures) {
        Write-Host "FAIL: $failure" -ForegroundColor Red
    }
    exit 1
}
if ($leakFailures.Count -gt 0) {
    Write-Warning "short mode records the leak verdict but does not fail on it; the full soak is the gate"
}

Write-Host ("M0 harness: PASS ($Mode; tracker=$trackerKind source=$sourceKind synthetic=$($summary.synthetic))") -ForegroundColor Green
exit 0
