<#
.SYNOPSIS
    T09 gate: the tracker C ABI, the app-side interface and the scripted
    contract harness exist and behave.

.DESCRIPTION
    Runs after the native build and the fast tests. Checks:

      1. The C ABI header, the C++ interface, the scripted backend, the shared
         latency histogram, the contract tests and ADR-0019 exist.
      2. The C ABI exposes exactly the five documented functions, an ABI
         version, an error enum and the fixed result layout.
      3. The C header is POD-only: no C++ standard-library type or header, and
         only <stdint.h>/<stddef.h> are included.
      4. The C++ interface and ScriptedTracker implement the documented
         contract, and TrackerStats carries inference, resolution, drop-reason
         and confidence instrumentation.
      5. The contract suite covers submit/latest, timestamps, duplicate input,
         invalid vs features_valid, destroy safety and exception containment.
      6. CTest registers the C++ contract suite and the C compile check under
         fast/task_T09, and both pass.
      7. The design documents and the implementation map record the implemented
         ABI and ADR-0019.
      8. docs/progress/T09.md, when present, has Follow-ups and Hand-off (it is
         gitignored, so its absence is not a failure).

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

# 1. Required implementation and design files.
$cHeader = Read-Required 'native/tracking/include/avatar/tracker/tracker_c_api.h'
$cppInterface = Read-Required 'native/tracking/include/avatar/tracking/itracking_backend.h'
$resultHeader = Read-Required 'native/tracking/include/avatar/tracking/tracking_result.h'
$scriptedHeader = Read-Required 'native/tracking/include/avatar/tracking/scripted_tracker.h'
$null = Read-Required 'native/tracking/src/tracker_c_api.cpp'
$null = Read-Required 'native/tracking/src/scripted_tracker.cpp'
$null = Read-Required 'native/core/include/avatar/core/latency_histogram.h'
$contractTests = Read-Required 'tests/unit/test_tracker_contract.cpp'
$null = Read-Required 'tests/unit/test_latency_histogram.cpp'
$null = Read-Required 'tests/unit/tracker_c_api_c_compile.c'
$null = Read-Required 'docs/design/adr/0019-tracker-c-abi-v1.md'
$trackingDesign = Read-Required 'docs/design/tracking.md'

# 2. The five-function ABI, version, error enum and result layout.
if ($cHeader -ne '') {
    foreach ($needle in @(
            'AVATAR_TRACKER_ABI_VERSION',
            'avatar_tracker_create',
            'avatar_tracker_configure',
            'avatar_tracker_push_frame',
            'avatar_tracker_poll_result',
            'avatar_tracker_destroy',
            'avatar_tracker_error',
            'avatar_tracker_result',
            'avatar_tracker_head_pose',
            'AVATAR_TRACKER_LANDMARK_CAPACITY 478u',
            'AVATAR_TRACKER_BLENDSHAPE_COUNT 52u',
            'source_timestamp_ns',
            'features_valid',
            'confidence',
            'sizeof = 5992',
            'extern "C"')) {
        if ($cHeader -notmatch [regex]::Escape($needle)) {
            Add-Failure "tracker_c_api.h is missing '$needle'"
        }
    }
    if ($cHeader -match 'std::') {
        Add-Failure 'tracker_c_api.h uses a C++ standard-library type'
    }
    foreach ($header in @('string', 'vector', 'array', 'optional', 'memory', 'mutex')) {
        if ($cHeader -match ('#\s*include\s*<' + $header + '>')) {
            Add-Failure "tracker_c_api.h includes C++ header <$header>"
        }
    }
    $includes = [regex]::Matches($cHeader, '#\s*include\s*<([^>]+)>') |
        ForEach-Object { $_.Groups[1].Value }
    foreach ($include in $includes) {
        if ($include -notin @('stddef.h', 'stdint.h')) {
            Add-Failure "tracker_c_api.h includes unexpected header <$include>"
        }
    }
}

# 3. The app-side interface and the scripted implementation.
if ($cppInterface -ne '') {
    foreach ($needle in @('class ITrackingBackend', 'initialize', 'submit', 'latest', 'stats',
                          'shutdown', 'never waits', 'never throws')) {
        if ($cppInterface -notmatch [regex]::Escape($needle)) {
            Add-Failure "itracking_backend.h is missing '$needle'"
        }
    }
}
if ($scriptedHeader -ne '') {
    foreach ($needle in @('public ITrackingBackend', 'initialize', 'shutdown', 'configure')) {
        if ($scriptedHeader -notmatch [regex]::Escape($needle)) {
            Add-Failure "scripted_tracker.h is missing '$needle'"
        }
    }
}
if ($resultHeader -ne '') {
    foreach ($needle in @('DropReason', 'ConfidenceDistribution', 'inference_ns', 'p50', 'p95',
                          'input_width', 'dropped_script_exhausted', 'dropped_backend_busy',
                          'dropped_shutdown')) {
        if ($resultHeader -notmatch [regex]::Escape($needle)) {
            Add-Failure "tracking_result.h is missing '$needle'"
        }
    }
}

# 4. Contract coverage: the cases the task names must have a test each.
if ($contractTests -ne '') {
    foreach ($needle in @(
            'tracker_c_abi_returns_only_the_newest_result',
            'tracker_c_abi_preserves_duplicate_and_non_monotonic_timestamps',
            'tracker_c_abi_distinguishes_invalid_from_features_invalid',
            'tracker_c_abi_destroy_and_null_arguments_are_safe',
            'tracker_c_abi_contains_backend_exceptions',
            'tracker_submit_never_waits_for_inference',
            'scripted_tracker_implements_the_cpp_interface')) {
        if ($contractTests -notmatch [regex]::Escape($needle)) {
            Add-Failure "test_tracker_contract.cpp is missing the '$needle' case"
        }
    }
}

# 5. The C header compiles from C and C++ with warnings as errors.
$cmake = Read-Required 'CMakeLists.txt'
if ($cmake -ne '') {
    if ($cmake -notmatch 'LANGUAGES C CXX') {
        Add-Failure 'CMakeLists.txt does not enable the C language'
    }
    if ($cmake -notmatch 'avatar_tracker_c_abi_compile') {
        Add-Failure 'CMakeLists.txt has no C ABI compile target'
    }
    if ($cmake -notmatch 'C_STANDARD 11') {
        Add-Failure 'the C ABI compile target does not pin C11'
    }
}

# 6. CTest registration and execution.
$ctestBase = @('--test-dir', (Join-Path $root $BuildDir), '-C', $Config)
$taskList = Invoke-Capture 'ctest' ($ctestBase + @('-N', '-L', 'task_T09'))
if ($taskList.Code -ne 0 -or $taskList.Output -notmatch 'avatar_tracking_unit') {
    Add-Failure 'CTest does not list avatar_tracking_unit under label task_T09 (build first)'
}
if ($taskList.Code -ne 0 -or $taskList.Output -notmatch 'avatar_tracker_c_abi_compile') {
    Add-Failure 'CTest does not list avatar_tracker_c_abi_compile under label task_T09'
}
$run = Invoke-Capture 'ctest' ($ctestBase + @('-L', 'task_T09', '--output-on-failure'))
if ($run.Code -ne 0) {
    Add-Failure "the task_T09 tests failed: $($run.Output)"
}

# 7. Design records and the implementation map.
if ($trackingDesign -ne '') {
    foreach ($needle in @('tracker_c_api.h', 'avatar_tracker_create', 'avatar_tracker_poll_result',
                          'ITrackingBackend', 'ScriptedTracker', 'AVATAR_TRACKER_ABI_VERSION')) {
        if ($trackingDesign -notmatch [regex]::Escape($needle)) {
            Add-Failure "tracking.md does not document '$needle'"
        }
    }
}
$map = Read-Required 'docs/design/implementation-map.md'
if ($map -ne '') {
    if ($map -notmatch 'current as of T(\d{2,3})') {
        Add-Failure 'implementation-map.md has no "current as of TNN" marker'
    } elseif ([int]$Matches[1] -lt 9) {
        Add-Failure "implementation-map.md is stale (current as of T$($Matches[1]), expected T09 or later)"
    }
    if ($map -notmatch '(?m)^\|\s*\[ADR-0019\]\(adr/0019-tracker-c-abi-v1\.md\)[^\r\n]*\|\s*Accepted\s*\|') {
        Add-Failure 'implementation-map.md does not list ADR-0019 as Accepted'
    }
    if ($map -notmatch '(?m)^\|[^\r\n]*C ABI[^\r\n]*\|\s*`Implemented`\s*\|') {
        Add-Failure 'implementation-map.md has no Implemented C ABI row'
    }
}
$adrIndex = Read-Required 'docs/design/adr/README.md'
if ($adrIndex -ne '' -and
    $adrIndex -notmatch '(?m)^\|\s*\[0019\]\(0019-tracker-c-abi-v1\.md\)\s*\|[^\r\n]*\|\s*Accepted\s*\|') {
    Add-Failure 'ADR-0019 is not indexed as Accepted in docs/design/adr/README.md'
}

# 8. The task note is local; check it only when the planning workspace exists.
$progressPath = Join-Path $root 'docs/progress/T09.md'
if (Test-Path -LiteralPath $progressPath) {
    $progress = Get-Content -LiteralPath $progressPath -Raw
    foreach ($needle in @('Follow-ups', 'Hand-off')) {
        if ($progress -notmatch [regex]::Escape($needle)) {
            Add-Failure "docs/progress/T09.md is missing '$needle'"
        }
    }
} else {
    Write-Host 'T09 gate: note - docs/progress/T09.md not present (gitignored workspace note); skipped'
}

if ($failures.Count -gt 0) {
    Write-Host 'T09 gate: FAIL' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

$mode = if ($env:AVATAR_VERIFY_MODE) { $env:AVATAR_VERIFY_MODE } else { 'gate-only' }
Write-Host ("T09 gate: PASS ($mode; tracker C ABI, contract suite, latency histogram, " +
    'design records)')
exit 0
