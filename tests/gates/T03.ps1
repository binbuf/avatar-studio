<#
.SYNOPSIS
    T03 gate: the data-plane primitives and the diagnostics registry exist and
    the core still reads no wall clock.

.DESCRIPTION
    Runs after the native build and the fast unit tests. Checks invariants that
    a unit test cannot see from inside the library:

      1. The ring, latest-slot, diagnostics header and diagnostics source exist.
      2. native/core never reads a wall clock and only clock.cpp includes
         <chrono>; the FrameClock/TimeSource contract is the only time path.
      3. The ring primitive keeps drop-oldest in the policy wrapper.
      4. The latest-value slot exposes publish/read and a sequence.
      5. The diagnostics registry covers every area and exposes the Logger.
      6. Public core headers contain no explicit throw.
      7. The TSan configuration for T06 is documented.
      8. CTest registers the unit suite and the benchmark under task_T03.

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

function Read-Required([string]$RelativePath) {
    $path = Join-Path $root $RelativePath
    if (-not (Test-Path -LiteralPath $path)) {
        $failures.Add("missing file: $RelativePath")
        return ''
    }
    return (Get-Content -LiteralPath $path -Raw)
}

# 1. Required implementation files.
$ringPath = 'native/core/include/avatar/core/spsc_ring.h'
$slotPath = 'native/core/include/avatar/core/latest_slot.h'
$diagnosticsPath = 'native/core/include/avatar/core/diagnostics.h'
$diagnosticsSourcePath = 'native/core/src/diagnostics.cpp'
$ring = Read-Required $ringPath
$slot = Read-Required $slotPath
$diagnostics = Read-Required $diagnosticsPath
$null = Read-Required $diagnosticsSourcePath

# 2. Wall-clock and framework isolation.
$coreFiles = Get-ChildItem -Path (Join-Path $root 'native/core') -Recurse -File -Include '*.h', '*.cpp'
foreach ($file in $coreFiles) {
    $text = Get-Content -LiteralPath $file.FullName -Raw
    if ($text -match '(?i)#\s*include\s*[<"]godot') {
        $failures.Add("core includes a Godot header: $($file.FullName)")
    }
    $wallClockPattern = '\b(system_clock|high_resolution_clock)\b|\bstd::time\s*\(|\bGetSystemTime\b|\bQueryPerformanceCounter\b|\bclock_gettime\b|\blocaltime\b|\bgmtime\b'
    if ($text -match $wallClockPattern) {
        $failures.Add("core reads a wall clock (use FrameClock/SteadyTimeSource): $($file.FullName)")
    }
    if ($text -match '#\s*include\s*<chrono>' -and $file.Name -ne 'clock.cpp') {
        $failures.Add("<chrono> is only allowed in clock.cpp: $($file.FullName)")
    }
}

# 3. Ring primitive and drop-oldest policy wrapper.
if ($ring -ne '') {
    foreach ($needle in @('try_push', 'try_pop', 'dropped', 'alignas(kCacheLineSize)', 'DropOldestRing', 'try_discard_oldest')) {
        if ($ring -notmatch [regex]::Escape($needle)) {
            $failures.Add("$ringPath is missing '$needle'")
        }
    }
}

# 4. Latest-value slot.
if ($slot -ne '') {
    foreach ($needle in @('publish', 'read', 'sequence', 'LatestSlot')) {
        if ($slot -notmatch [regex]::Escape($needle)) {
            $failures.Add("$slotPath is missing '$needle'")
        }
    }
}

# 5. Registry covers every area and exposes the structured logger.
if ($diagnostics -ne '') {
    foreach ($needle in @(
            'DiagnosticCode',
            'DiagnosticInfo',
            'kDiagnosticRegistry',
            'cap_001',
            'trk_002',
            'rig_004',
            'avt_101',
            'out_007',
            'vcam_007',
            'gpu_004',
            'cfg_006',
            'sec_004',
            'Logger',
            'is_forbidden_log_key'
        )) {
        if ($diagnostics -notmatch [regex]::Escape($needle)) {
            $failures.Add("$diagnosticsPath is missing '$needle'")
        }
    }
}

# 6. No explicit throw in the public core headers.
$publicHeaders = Get-ChildItem -Path (Join-Path $root 'native/core/include') -Recurse -File -Include '*.h'
foreach ($header in $publicHeaders) {
    $text = Get-Content -LiteralPath $header.FullName -Raw
    if ($text -match '\bthrow\b') {
        $failures.Add("public core header throws an exception: $($header.FullName)")
    }
}

# 7. TSan configuration is documented for T06.
$quality = Read-Required 'docs/design/testing-and-quality.md'
if ($quality -ne '' -and $quality -notmatch [regex]::Escape('fsanitize=thread')) {
    $failures.Add('docs/design/testing-and-quality.md does not document the TSan job (fsanitize=thread)')
}

# 8. CTest registration.
$ctestOutput = & ctest --test-dir (Join-Path $root $BuildDir) -C $Config -N -L task_T03 2>&1
$ctestText = $ctestOutput -join "`n"
if ($LASTEXITCODE -ne 0 -or $ctestText -notmatch 'avatar_core_unit') {
    $failures.Add('CTest does not list avatar_core_unit under label task_T03 (build first)')
}
if ($LASTEXITCODE -ne 0 -or $ctestText -notmatch 'avatar_core_bench') {
    $failures.Add('CTest does not list avatar_core_bench under label task_T03 (build first)')
}

if ($failures.Count -gt 0) {
    Write-Host 'T03 gate: FAIL' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host 'T03 gate: PASS (SPSC ring + drop-oldest policy, latest-value slot, diagnostics registry/logger, no wall clock, TSan documented, labelled tests)'
exit 0
