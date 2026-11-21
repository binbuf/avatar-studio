<#
.SYNOPSIS
    T01 gate: the native core stays framework-free and dependencies stay pinned.

.DESCRIPTION
    Runs after the native build. Checks invariants that a unit test cannot see:

      1. native/core does not include Godot headers and reads no wall clock.
      2. The pinned nlohmann/json copy exists and matches the bootstrapped version.
      3. CMake never downloads dependencies (no FetchContent/ExternalProject).
      4. CTest registers the native suite under the task_T01 label.

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

# 1. Framework-free core.
$coreFiles = Get-ChildItem -Path (Join-Path $root 'native/core') -Recurse -File -Include '*.h', '*.cpp'
foreach ($file in $coreFiles) {
    $text = Get-Content -LiteralPath $file.FullName -Raw
    if ($text -match '(?i)#\s*include\s*[<"]godot') {
        $failures.Add("core includes a Godot header: $($file.FullName)")
    }
    if ($text -match '\b(system_clock|high_resolution_clock)\b') {
        $failures.Add("core reads a wall clock (only steady_clock via SteadyTimeSource is allowed): $($file.FullName)")
    }
}

# 2. Pinned dependency is present and recorded.
$jsonHeader = Join-Path $root '.tools/nlohmann_json/include/nlohmann/json.hpp'
$jsonMarker = Join-Path $root '.tools/nlohmann_json/.pinned'
if (-not (Test-Path -LiteralPath $jsonHeader)) {
    $failures.Add("pinned nlohmann/json is missing: $jsonHeader (run scripts/bootstrap.ps1)")
} elseif (-not (Test-Path -LiteralPath $jsonMarker)) {
    $failures.Add("pinned nlohmann/json marker is missing: $jsonMarker")
} else {
    $marker = Get-Content -LiteralPath $jsonMarker -Raw
    if ($marker -notmatch 'nlohmann_json 3\.11\.3 sha256=[0-9A-Fa-f]{64}') {
        $failures.Add("pinned nlohmann/json marker is not the expected 3.11.3 record: $marker")
    }
}

# 3. CMake must not fetch anything.
$cmakeText = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmakeText -match '\bFetchContent\b|ExternalProject_Add|file\s*\(\s*DOWNLOAD') {
    $failures.Add('CMakeLists.txt downloads dependencies; bootstrap must be the only fetch path')
}

# 4. The native suite is registered under the task label.
$ctestOutput = & ctest --test-dir (Join-Path $root $BuildDir) -C $Config -N -L task_T01 2>&1
$ctestText = $ctestOutput -join "`n"
if ($LASTEXITCODE -ne 0 -or $ctestText -notmatch 'avatar_core_unit') {
    $failures.Add("CTest does not list avatar_core_unit under label task_T01 (build first)")
}

if ($failures.Count -gt 0) {
    Write-Host 'T01 gate: FAIL' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host 'T01 gate: PASS (framework-free core, pinned dependencies, no CMake fetch, labelled tests)'
exit 0
