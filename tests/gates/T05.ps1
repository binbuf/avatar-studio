<#
.SYNOPSIS
    T05 gate: the performance harness, hardware fingerprint and regression
    comparator exist and behave.

.DESCRIPTION
    Runs after the native build and the fast tests. Checks:

      1. The harness, runners, comparator, baseline and design record exist.
      2. avatar_bench exposes the required switches and emits the documented
         metric names.
      3. The checked-in baseline is a local baseline (not a release gate) with a
         hardware fingerprint and explicit metric/operator/target checks.
      4. CTest registers the self-test under fast/task_T05 and the measurement
         under perf; the fast label never includes the perf test.
      5. avatar_bench --self-test passes (all scenarios, well-formed JSON).
      6. check.mjs passes the baseline and fails a deliberately broken result,
         naming the failing metric.
      7. In -Full mode only, a fresh measurement is compared to the baseline.

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

# 1. Required implementation files.
$benchSource = Read-Required 'tests/performance/avatar_bench.cpp'
$checkScript = Read-Required 'tests/performance/check.mjs'
$null = Read-Required 'tests/performance/make_baseline.mjs'
$null = Read-Required 'tests/performance/run.ps1'
$null = Read-Required 'tests/performance/run.sh'
$null = Read-Required 'tests/performance/README.md'
$null = Read-Required 'docs/design/adr/0015-performance-harness-and-local-baseline.md'
$baselinePath = Join-Path $root 'tests/performance/baselines/foundation-local.json'
if (-not (Test-Path -LiteralPath $baselinePath)) {
    Add-Failure 'missing baseline: tests/performance/baselines/foundation-local.json'
}

# 2. Harness contract: switches, scenarios and metric names.
if ($benchSource -ne '') {
    foreach ($needle in @('--frames', '--warmup', '--width', '--height', '--tracker',
                          '--scenario', '--output', '--self-test',
                          'capture', 'ring', 'rig', 'render',
                          'frame_time_ms', 'app_latency_ms', 'end_to_end_latency_ms',
                          'dropped_frames', 'dropped_tracking_frames', 'rss_bytes',
                          'cpu_percent_one_core', 'gpu_percent', 'fingerprint',
                          'git_commit', 'p50', 'p95', 'p99')) {
        if ($benchSource -notmatch [regex]::Escape($needle)) {
            Add-Failure "avatar_bench.cpp is missing '$needle'"
        }
    }
}
if ($checkScript -ne '') {
    foreach ($needle in @('metrics.', 'operator', 'target', 'allowance', 'FAIL')) {
        if ($checkScript -notmatch [regex]::Escape($needle)) {
            Add-Failure "check.mjs is missing '$needle'"
        }
    }
}

# 3. Baseline shape and local-baseline marker.
$baseline = $null
if (Test-Path -LiteralPath $baselinePath) {
    try {
        $baseline = Get-Content -LiteralPath $baselinePath -Raw | ConvertFrom-Json
    } catch {
        Add-Failure "baseline is not valid JSON: $($_.Exception.Message)"
    }
}
if ($null -ne $baseline) {
    if ($baseline.schema -ne 'avatar_bench.baseline.v1') {
        Add-Failure "baseline schema is not avatar_bench.baseline.v1"
    }
    if ($baseline.kind -ne 'local') {
        Add-Failure "baseline kind is not 'local'"
    }
    if ($baseline.release_gate -ne $false) {
        Add-Failure "baseline is marked as a release gate; it must be local only"
    }
    if (-not $baseline.hardware -or -not $baseline.hardware.fingerprint) {
        Add-Failure "baseline has no hardware fingerprint"
    }
    if (-not $baseline.result -or -not $baseline.result.metrics) {
        Add-Failure "baseline has no embedded result"
    }
    if (-not $baseline.checks -or $baseline.checks.Count -lt 3) {
        Add-Failure "baseline has fewer than three checks"
    } else {
        foreach ($check in $baseline.checks) {
            if (-not $check.metric -or -not $check.operator -or $null -eq $check.target) {
                Add-Failure "baseline check is missing metric/operator/target: $($check | ConvertTo-Json -Compress)"
            }
        }
    }
}

# 4. CTest labels: perf is opt-in and excluded from fast.
$ctestBase = @('--test-dir', (Join-Path $root $BuildDir), '-C', $Config, '-N')
$fastList = (& ctest @ctestBase -L fast 2>&1) -join "`n"
if ($LASTEXITCODE -ne 0) {
    Add-Failure 'ctest -L fast failed (build first)'
}
if ($fastList -notmatch 'avatar_bench_selftest') {
    Add-Failure 'CTest does not list avatar_bench_selftest under label fast'
}
if ($fastList -match 'avatar_bench_perf') {
    Add-Failure 'CTest lists avatar_bench_perf under label fast; perf must be opt-in'
}
$perfList = (& ctest @ctestBase -L perf 2>&1) -join "`n"
if ($LASTEXITCODE -ne 0 -or $perfList -notmatch 'avatar_bench_perf') {
    Add-Failure 'CTest does not list avatar_bench_perf under label perf'
}
$taskList = (& ctest @ctestBase -L task_T05 2>&1) -join "`n"
if ($LASTEXITCODE -ne 0 -or $taskList -notmatch 'avatar_bench_selftest') {
    Add-Failure 'CTest does not list avatar_bench_selftest under label task_T05'
}

$verifyScript = Read-Required 'scripts/verify.ps1'
if ($verifyScript -ne '') {
    if ($verifyScript -notmatch [regex]::Escape('-LE')) {
        Add-Failure 'scripts/verify.ps1 does not exclude the perf label from -Fast'
    }
    if ($verifyScript -notmatch 'AVATAR_VERIFY_MODE') {
        Add-Failure 'scripts/verify.ps1 does not expose the fast/full mode to gates'
    }
}

# 5. Executable and JSON self-test.
$exe = Join-Path $root "$BuildDir/$Config/avatar_bench.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    $exe = Join-Path $root "$BuildDir/avatar_bench.exe"
}
if (-not (Test-Path -LiteralPath $exe)) {
    $exe = Join-Path $root "$BuildDir/avatar_bench"
}
if (-not (Test-Path -LiteralPath $exe)) {
    Add-Failure 'avatar_bench executable is missing (build first)'
} else {
    & $exe --self-test | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Add-Failure 'avatar_bench --self-test failed'
    }
}

# 6. Comparator pass and deliberate-failure behaviour.
$node = Get-Command node -ErrorAction SilentlyContinue
$gateDir = Join-Path $root "$BuildDir/t05-gate"
New-Item -ItemType Directory -Force -Path $gateDir | Out-Null
$resultPath = Join-Path $gateDir 'baseline-result.json'
$missPath = Join-Path $gateDir 'deliberate-miss.json'

function Write-JsonFile([string]$Path, [object]$Value) {
    $text = $Value | ConvertTo-Json -Depth 100
    [System.IO.File]::WriteAllText($Path, $text, (New-Object System.Text.UTF8Encoding($false)))
}

if ($null -ne $baseline -and $null -ne $baseline.result) {
    Write-JsonFile $resultPath $baseline.result
}

if (-not $node) {
    Add-Failure 'node is required for tests/performance/check.mjs'
} elseif ($null -ne $baseline -and (Test-Path -LiteralPath $resultPath)) {
    # Native stderr becomes a terminating error under ErrorActionPreference=Stop
    # in Windows PowerShell 5.1; relax it only while capturing check.mjs output.
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & node (Join-Path $root 'tests/performance/check.mjs') `
            --baseline $baselinePath --result $resultPath --quiet | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Add-Failure 'check.mjs did not pass the checked-in baseline'
        }

        $miss = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
        if ($null -eq $miss.metrics.dropped_frames) {
            Add-Failure 'baseline result has no metrics.dropped_frames to mutate'
        } else {
            $miss.metrics.dropped_frames = 3
            Write-JsonFile $missPath $miss
            $missOutput = (& node (Join-Path $root 'tests/performance/check.mjs') `
                --baseline $baselinePath --result $missPath 2>&1) -join "`n"
            $missCode = $LASTEXITCODE
            if ($missCode -eq 0) {
                Add-Failure 'check.mjs did not fail a result that deliberately misses a target'
            }
            if ($missOutput -notmatch [regex]::Escape('metrics.dropped_frames')) {
                Add-Failure 'check.mjs failure output does not name the failing metric'
            }
        }
    } finally {
        $ErrorActionPreference = $previousPreference
    }
}

# 7. Full verification measures this machine and compares to the baseline.
$mode = $env:AVATAR_VERIFY_MODE
if ($mode -eq 'full' -and (Test-Path -LiteralPath $exe) -and $null -ne $baseline -and $node) {
    $frames = [int]$baseline.result.config.frames
    $warmup = [int]$baseline.result.config.warmup_frames
    $width = [int]$baseline.result.config.width
    $height = [int]$baseline.result.config.height
    $fps = [int]$baseline.result.config.fps
    $fresh = Join-Path $gateDir 'fresh-rig.json'
    & $exe --scenario rig --frames $frames --warmup $warmup `
        --width $width --height $height --fps $fps --quiet --output $fresh
    if ($LASTEXITCODE -ne 0) {
        Add-Failure 'fresh measurement for the full gate failed'
    } else {
        & node (Join-Path $root 'tests/performance/check.mjs') `
            --baseline $baselinePath --result $fresh --quiet | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Add-Failure 'fresh measurement failed the checked-in local baseline'
        }
    }
}

# 8. Design record reflects the implemented harness.
$quality = Read-Required 'docs/design/testing-and-quality.md'
if ($quality -ne '') {
    foreach ($needle in @('avatar_bench', 'check.mjs', 'perf', 'tests/performance/results/',
                          'app_latency_ms', 'hardware fingerprint', 'local baseline')) {
        if ($quality -notmatch [regex]::Escape($needle)) {
            Add-Failure "docs/design/testing-and-quality.md is missing '$needle'"
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host 'T05 gate: FAIL' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

$modeText = if ($mode) { $mode } else { 'gate-only' }
Write-Host "T05 gate: PASS ($modeText; harness, fingerprint, local baseline, perf label, comparator)"
exit 0
