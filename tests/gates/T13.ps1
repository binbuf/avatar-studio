<#
.SYNOPSIS
    T13 gate: the headless M0 harness, its short CI run and its soak wiring exist and
    produce a valid, complete JSON report.

.DESCRIPTION
    Runs after the native build and the fast tests. In -Fast mode the avatar_m0_short
    CTest has just written latest-m0-short.json; in -Full mode avatar_m0_soak has
    written latest-m0-full.json. The gate checks the schema, both input sizes, the
    measurement fields the task requires, the leak verdict and the human artifacts.

    The M0 pass/pivot thresholds are owned by tests/gates/T14.ps1. This gate only
    proves the harness is real and complete, so a synthetic run is allowed but is
    called out explicitly.

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

function Invoke-Capture([string]$File, [string[]]$Arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $File @Arguments 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    $text = (@($output) | ForEach-Object { [string]$_ }) -join "`n"
    return [pscustomobject]@{ Code = $code; Output = $text }
}

# 1. Implementation, harness, gate and design files.
$m0 = Read-Required 'tests/m0/m0.cpp'
$runner = Read-Required 'tests/m0/run.ps1'
$t14 = Read-Required 'tests/gates/T14.ps1'
$adr = Read-Required 'docs/design/adr/0023-m0-headless-harness-and-thresholds.md'
$testing = Read-Required 'docs/design/testing-and-quality.md'
$tracking = Read-Required 'docs/design/tracking.md'
$map = Read-Required 'docs/design/implementation-map.md'
$adrIndex = Read-Required 'docs/design/adr/README.md'
$designReadme = Read-Required 'docs/design/README.md'

# 2. The harness source contains the required capabilities. These are structural
#    checks; the JSON checks below prove they actually ran.
if ($m0 -ne '') {
    foreach ($needle in @(
            'AVCLIP1', 'ScriptedBackend', 'CabiBackend', 'avatar_tracker_create',
            'avatar_tracker_push_frame', 'avatar_tracker_poll_result',
            'inference_ms', 'frame_age_ms', 'dropped', 'superseded', 'stalled',
            'confidence', 'rss_bytes', 'handle_count', 'thread_cpu',
            'pose_stability', 'write_pose_svg', 'write_bmp', '--input-size',
            '--strict', 'avatar_m0.result.v1')) {
        if ($m0 -notmatch [regex]::Escape($needle)) {
            Add-Failure "tests/m0/m0.cpp is missing '$needle'"
        }
    }
}
if ($runner -ne '') {
    foreach ($needle in @('short', 'full', 'latest-m0-', 'T13-m0-results.md',
                          '--rss-interval-seconds', '--strict', 'live.avclip',
                          'libavatartracker.dll')) {
        if ($runner -notmatch [regex]::Escape($needle)) {
            Add-Failure "tests/m0/run.ps1 is missing '$needle'"
        }
    }
}
if ($t14 -ne '') {
    foreach ($needle in @('latest-m0-full.json', 'inference_ms', 'frame_age_ms',
                          'handle_monotonic_growth', '15.0', '30.0', '1800')) {
        if ($t14 -notmatch [regex]::Escape($needle)) {
            Add-Failure "tests/gates/T14.ps1 is missing '$needle'"
        }
    }
}

# 3. CMake builds the harness and registers the short and soak runs.
$cmake = Read-Required 'CMakeLists.txt'
if ($cmake -ne '') {
    foreach ($needle in @('tests/m0/m0.cpp', 'avatar_m0_selftest', 'avatar_m0_short',
                          'avatar_m0_soak', 'task_T13')) {
        if ($cmake -notmatch [regex]::Escape($needle)) {
            Add-Failure "CMakeLists.txt is missing '$needle'"
        }
    }
}

# 4. The JSON report for this verification mode.
$verifyMode = if ($env:AVATAR_VERIFY_MODE) { $env:AVATAR_VERIFY_MODE } else { '' }
if ($verifyMode -ne 'fast' -and $verifyMode -ne 'full') {
    $soakPath = Join-Path $root 'tests/performance/results/latest-m0-full.json'
    $verifyMode = if (Test-Path -LiteralPath $soakPath) { 'full' } else { 'fast' }
}
$harnessMode = if ($verifyMode -eq 'full') { 'full' } else { 'short' }
$jsonPath = Join-Path $root ("tests/performance/results/latest-m0-{0}.json" -f $harnessMode)
if (-not (Test-Path -LiteralPath $jsonPath)) {
    Add-Failure "missing M0 JSON: $jsonPath (the avatar_m0_$harnessMode CTest must run first)"
} else {
    try {
        $summary = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json
    } catch {
        Add-Failure "M0 JSON is not parseable: $($_.Exception.Message)"
        $summary = $null
    }
    if ($null -ne $summary) {
        if ($summary.schema -ne 'avatar_m0.summary.v1') {
            Add-Failure "M0 JSON schema is '$($summary.schema)', expected avatar_m0.summary.v1"
        }
        if ($summary.mode -ne $harnessMode) {
            Add-Failure "M0 JSON mode is '$($summary.mode)', expected '$harnessMode'"
        }
        $runs = @($summary.runs)
        if ($runs.Count -ne 2) {
            Add-Failure "M0 JSON has $($runs.Count) runs, expected 2 (192 and 256)"
        } else {
            $sizes = @($runs | ForEach-Object { [int]$_.config.input_size }) | Sort-Object
            if (($sizes -join ',') -ne '192,256') {
                Add-Failure "M0 JSON sizes are '$($sizes -join ',')', expected 192,256"
            }
        }
        foreach ($run in $runs) {
            $label = "run $($run.config.input_size)"
            if ($run.schema -ne 'avatar_m0.result.v1') {
                Add-Failure "$label has schema '$($run.schema)'"
            }
            if ($run.tracker.kind -notin @('real', 'scripted')) {
                Add-Failure "$label has an unknown tracker kind '$($run.tracker.kind)'"
            }
            if ($run.source.kind -notin @('replay', 'pattern')) {
                Add-Failure "$label has an unknown source kind '$($run.source.kind)'"
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
            if ([int]$run.metrics.frames.resolved -le 0) {
                Add-Failure "$label resolved no frames"
            }
            foreach ($reason in @('superseded', 'stalled', 'submit_error', 'source_missed')) {
                if ($null -eq $run.metrics.dropped.reasons.$reason) {
                    Add-Failure "$label is missing dropped reason '$reason'"
                }
            }
            if (@($run.metrics.confidence.bins).Count -ne 10) {
                Add-Failure "$label confidence distribution does not have 10 bins"
            }
            if (@($run.metrics.rss.samples).Count -lt 1) {
                Add-Failure "$label has no RSS samples"
            }
            if ($null -eq $run.metrics.leak -or
                $null -eq $run.metrics.leak.passed) {
                Add-Failure "$label has no leak verdict"
            }
            if ($null -eq $run.metrics.pose_stability -or
                $null -eq $run.metrics.pose_stability.frame_delta_deg) {
                Add-Failure "$label has no pose-stability distribution"
            }
            if (@($run.metrics.cpu.thread_cpu).Count -lt 1) {
                Add-Failure "$label has no thread CPU samples"
            }
            $tracePath = [string]$run.artifacts.pose_trace
            if ($tracePath -eq '' -or -not (Test-Path -LiteralPath $tracePath)) {
                Add-Failure "$label pose trace is missing: $tracePath"
            }
            $clipDir = [string]$run.artifacts.clip_dir
            if ($clipDir -eq '' -or -not (Test-Path -LiteralPath $clipDir)) {
                Add-Failure "$label clip directory is missing: $clipDir"
            } elseif (@(Get-ChildItem -LiteralPath $clipDir -Filter '*.bmp').Count -lt 1) {
                Add-Failure "$label clip directory has no BMP frames: $clipDir"
            }
        }
        if ($harnessMode -eq 'full') {
            if ([double]$summary.total_duration_s -lt 1800.0) {
                Add-Failure "the soak total duration is $($summary.total_duration_s) s, expected >= 1800 s"
            }
            foreach ($run in $runs) {
                if ([double]$run.config.duration_s -lt 600.0) {
                    Add-Failure "run $($run.config.input_size) measured $($run.config.duration_s) s, expected >= 600 s"
                }
                if ($run.metrics.leak.passed -ne $true) {
                    Add-Failure "run $($run.config.input_size) leak check failed"
                }
            }
            if ($summary.synthetic -eq $true) {
                Write-Host 'T13 gate: note - the full soak is SYNTHETIC (scripted tracker or pattern source); the harness is proven, not tracker feasibility' -ForegroundColor Yellow
            }
        }
    }
}

# 5. Design records and the implementation map.
if ($adr -ne '') {
    foreach ($needle in @('**Status:** Accepted', 'avatar_m0', 'avatar_m0.result.v1',
                          '5%', '1800', '15 ms')) {
        if ($adr -notmatch [regex]::Escape($needle)) {
            Add-Failure "ADR-0023 is missing '$needle'"
        }
    }
}
if ($testing -ne '') {
    foreach ($needle in @('T13', 'avatar_m0', 'avatar_m0.result.v1', '30-minute soak',
                          'tests/gates/T14.ps1')) {
        if ($testing -notmatch [regex]::Escape($needle)) {
            Add-Failure "testing-and-quality.md does not document '$needle'"
        }
    }
}
if ($tracking -ne '') {
    foreach ($needle in @('T13', 'avatar_m0', 'submit-to-poll')) {
        if ($tracking -notmatch [regex]::Escape($needle)) {
            Add-Failure "tracking.md does not document '$needle'"
        }
    }
}
if ($map -ne '') {
    if ($map -notmatch 'current as of T13') {
        Add-Failure 'implementation-map.md is not current as of T13'
    }
    if ($map -notmatch '(?m)^\|[^\r\n]*M0 headless harness[^\r\n]*\|\s*`Implemented`\s*\|') {
        Add-Failure 'implementation-map.md has no Implemented M0 harness row'
    }
    if ($map -notmatch '(?m)^\|\s*\[ADR-0023\]\(adr/0023-m0-headless-harness-and-thresholds\.md\)[^\r\n]*\|\s*Accepted\s*\|') {
        Add-Failure 'implementation-map.md does not list ADR-0023 as Accepted'
    }
}
if ($adrIndex -ne '' -and
    $adrIndex -notmatch '(?m)^\|\s*\[0023\]\(0023-m0-headless-harness-and-thresholds\.md\)\s*\|[^\r\n]*\|\s*Accepted\s*\|') {
    Add-Failure 'ADR-0023 is not indexed as Accepted in docs/design/adr/README.md'
}
if ($designReadme -ne '' -and $designReadme -notmatch 'T13') {
    Add-Failure 'docs/design/README.md does not record T13'
}

& node (Join-Path $root 'scripts/check-design-links.mjs')
if ($LASTEXITCODE -ne 0) {
    Add-Failure 'design-link check failed after the T13 documentation change'
}

# 6. The local progress workspace is gitignored; check it when present.
$progressPath = Join-Path $root 'docs/progress/T13.md'
if (Test-Path -LiteralPath $progressPath) {
    $progress = Get-Content -LiteralPath $progressPath -Raw
    foreach ($needle in @('Follow-ups', 'Hand-off')) {
        if ($progress -notmatch [regex]::Escape($needle)) {
            Add-Failure "docs/progress/T13.md is missing '$needle'"
        }
    }
} else {
    Write-Host 'T13 gate: note - docs/progress/T13.md not present (gitignored workspace note); skipped'
}

if ($failures.Count -gt 0) {
    Write-Host 'T13 gate: FAIL' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host ("T13 gate: PASS ($verifyMode/$harnessMode; headless M0 harness, short/soak wiring, valid JSON, " +
    'leak evidence, pose trace and clip)')
exit 0
