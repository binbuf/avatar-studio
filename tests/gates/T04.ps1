<#
.SYNOPSIS
    T04 gate: deterministic sources, scripted tracker, replay fixtures and the
    golden comparator exist and keep the no-wall-clock / no-unseeded-randomness
    contract.

.DESCRIPTION
    Runs after the native build and the fast tests. Checks:

      1. The capture/tracking headers and sources exist.
      2. ICameraSource documents producer-owned slots and pin/release ownership.
      3. native/capture and native/tracking never read a wall clock and never
         use unseeded randomness.
      4. The golden comparator implements SSIM, a max-delta guard and PPM
         artifacts.
      5. The replay fixtures exist and stay small (large clips are generated).
      6. CTest registers the component suite and the comparator CLI under
         task_T04, and the comparator self-test passes.

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
$captureHeader = Read-Required 'native/capture/include/avatar/capture/icamera_source.h'
$null = Read-Required 'native/capture/include/avatar/capture/video_frame.h'
$null = Read-Required 'native/capture/include/avatar/capture/frame_ring.h'
$null = Read-Required 'native/capture/include/avatar/capture/test_pattern_source.h'
$null = Read-Required 'native/capture/include/avatar/capture/synthetic_face_source.h'
$null = Read-Required 'native/capture/include/avatar/capture/replay_source.h'
$null = Read-Required 'native/capture/include/avatar/capture/y4m_reader.h'
$null = Read-Required 'native/capture/include/avatar/capture/face_ground_truth.h'
$null = Read-Required 'native/tracking/include/avatar/tracking/tracking_result.h'
$null = Read-Required 'native/tracking/include/avatar/tracking/scripted_tracker.h'
$comparator = Read-Required 'tests/tools/golden_compare/golden_compare.cpp'
$null = Read-Required 'tests/tools/golden_compare/golden_compare.h'
$null = Read-Required 'tests/tools/golden_compare/main.cpp'
$null = Read-Required 'scripts/gen_fixtures.ps1'

# 2. Ownership contract is stated where the interface lives.
if ($captureHeader -ne '') {
    foreach ($needle in @('producer', 'pin', 'release', 'immutable', 'No hidden copies')) {
        if ($captureHeader -notmatch [regex]::Escape($needle)) {
            $failures.Add("icamera_source.h does not document '$needle'")
        }
    }
}

# 3. Determinism: no wall clock, no unseeded randomness, no <chrono> in the
#    capture and tracking trees.
$wallClockPattern = '\b(system_clock|high_resolution_clock)\b|\bstd::time\s*\(|\bGetSystemTime\b|\bQueryPerformanceCounter\b|\bclock_gettime\b|\blocaltime\b|\bgmtime\b'
$randomPattern = '\bstd::random_device\b|\bsrand\s*\(|\brand\s*\(|\brandom\s*\('
foreach ($tree in @('native/capture', 'native/tracking')) {
    $files = Get-ChildItem -Path (Join-Path $root $tree) -Recurse -File -Include '*.h', '*.cpp'
    foreach ($file in $files) {
        $text = Get-Content -LiteralPath $file.FullName -Raw
        if ($text -match $wallClockPattern) {
            $failures.Add("wall-clock read in $($file.FullName)")
        }
        if ($text -match $randomPattern) {
            $failures.Add("unseeded randomness in $($file.FullName)")
        }
        if ($text -match '#\s*include\s*<chrono>') {
            $failures.Add("<chrono> in the capture/tracking tree: $($file.FullName)")
        }
    }
}

# 4. Comparator contract.
if ($comparator -ne '') {
    foreach ($needle in @('ssim', 'max_delta', 'write_artifacts', 'P6')) {
        if ($comparator -notmatch [regex]::Escape($needle)) {
            $failures.Add("golden_compare.cpp is missing '$needle'")
        }
    }
}

# 5. Small fixtures only.
foreach ($fixture in @('tests/fixtures/replay/tiny.y4m', 'tests/fixtures/replay/tiny.json')) {
    $path = Join-Path $root $fixture
    if (-not (Test-Path -LiteralPath $path)) {
        $failures.Add("missing fixture: $fixture")
        continue
    }
    $size = (Get-Item -LiteralPath $path).Length
    if ($size -gt 100KB) {
        $failures.Add("fixture is too large for git (generate it instead): $fixture ($size bytes)")
    }
}

# 6. CTest registration and the comparator CLI contract.
$ctestOutput = & ctest --test-dir (Join-Path $root $BuildDir) -C $Config -N -L task_T04 2>&1
$ctestText = $ctestOutput -join "`n"
if ($LASTEXITCODE -ne 0 -or $ctestText -notmatch 'avatar_capture_unit') {
    $failures.Add('CTest does not list avatar_capture_unit under label task_T04 (build first)')
}
if ($LASTEXITCODE -ne 0 -or $ctestText -notmatch 'golden_compare_cli') {
    $failures.Add('CTest does not list golden_compare_cli under label task_T04 (build first)')
}

$comparatorExe = Join-Path $root "$BuildDir/$Config/golden_compare.exe"
if (-not (Test-Path -LiteralPath $comparatorExe)) {
    $comparatorExe = Join-Path $root "$BuildDir/golden_compare"
}
if (-not (Test-Path -LiteralPath $comparatorExe)) {
    $failures.Add('golden_compare executable is missing (build first)')
} else {
    $selfTestDir = Join-Path $root "$BuildDir/golden_compare_gate"
    & $comparatorExe --self-test $selfTestDir | Out-Null
    if ($LASTEXITCODE -ne 0) {
        $failures.Add('golden_compare --self-test failed')
    }
}

# 7. The design document reflects the implemented source set and fixture policy.
$quality = Read-Required 'docs/design/testing-and-quality.md'
if ($quality -ne '') {
    foreach ($needle in @('TestPatternSource', 'SyntheticFaceSource', 'ReplaySource', 'ScriptedTracker', 'golden_compare', 'PPM', 'fixtures')) {
        if ($quality -notmatch [regex]::Escape($needle)) {
            $failures.Add("docs/design/testing-and-quality.md is missing '$needle'")
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host 'T04 gate: FAIL' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host 'T04 gate: PASS (sources, scripted tracker, fixtures, comparator, determinism, labelled tests)'
exit 0
