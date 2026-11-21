#requires -Version 5.1
<#
.SYNOPSIS
    Run the same local quality gates as the CI workflow (Windows).

.DESCRIPTION
    One entry point for the checks that must pass before a push:

      1. clang-format drift (skipped when clang-format is not installed; CI
         installs it and passes AVATAR_REQUIRE_FORMAT=1),
      2. the third-party licence manifest (scripts/check-licenses.mjs),
      3. the diagnostics source scan (advisory until T41),
      4. the design-link checker (scripts/check-design-links.mjs),
      5. bootstrap of the pinned dependencies,
      6. CMake configure and a RelWithDebInfo build,
      7. CTest label "fast" (the "perf" label is opt-in with -Full),
      8. the headless Godot shell smoke test, runtime pacing and GLB import
         tests (unless -NativeOnly).

    The GitHub Actions matrix runs the same sequence on Windows, macOS and
    Linux. This script is not a replacement for scripts/verify.ps1: verify also
    runs the task gate and is the objective contract.

.PARAMETER BuildDir
    Build directory. Defaults to "build".

.PARAMETER Config
    Build configuration. Defaults to RelWithDebInfo.

.PARAMETER SkipBootstrap
    Use the dependencies already present in .tools/.

.PARAMETER NativeOnly
    Bootstrap without Godot/godot-cpp and skip the shell smoke. For work that
    does not need the engine.

.PARAMETER Full
    Run the complete CTest suite, including the "perf" label.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts/ci.ps1

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts/ci.ps1 -NativeOnly -SkipBootstrap
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build',
    [string]$Config = 'RelWithDebInfo',
    [switch]$SkipBootstrap,
    [switch]$NativeOnly,
    [switch]$Full
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$script:step = 0

function Write-Step([string]$Text) {
    $script:step += 1
    Write-Host ''
    Write-Host ("ci: [{0}] {1}" -f $script:step, $Text) -ForegroundColor Cyan
}

function Invoke-Native([string]$File, [string[]]$Arguments) {
    $quoted = foreach ($argument in $Arguments) {
        if ($argument -match '[\s|;&<>]') { '"{0}"' -f $argument } else { $argument }
    }
    Write-Host (">> {0} {1}" -f $File, ($quoted -join ' '))
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "command failed with exit code ${LASTEXITCODE}: $File"
    }
}

function Invoke-PsScript([string]$Path, [hashtable]$Parameters = @{}) {
    $argumentText = ($Parameters.GetEnumerator() | ForEach-Object { "-$($_.Key)" }) -join ' '
    Write-Host ("& `"{0}`" {1}" -f $Path, $argumentText)
    & $Path @Parameters
    if ($LASTEXITCODE -ne 0) {
        throw "script failed with exit code ${LASTEXITCODE}: $Path"
    }
}

$exitCode = 0
Push-Location $root
try {
    $node = Get-Command node -ErrorAction SilentlyContinue
    if ($null -eq $node) {
        throw 'node is required for the licence, diagnostics and design-link checks'
    }

    Write-Step 'clang-format drift'
    Invoke-PsScript (Join-Path $root 'scripts/format-check.ps1')

    Write-Step 'third-party licence manifest'
    Invoke-Native 'node' @('scripts/check-licenses.mjs')

    Write-Step 'diagnostics source scan (advisory until T41)'
    Invoke-Native 'node' @('scripts/check-diagnostics.mjs')

    Write-Step 'design links'
    Invoke-Native 'node' @('scripts/check-design-links.mjs')

    if ($SkipBootstrap) {
        Write-Host 'ci: bootstrap skipped (-SkipBootstrap)'
    } else {
        Write-Step 'bootstrap pinned dependencies'
        $bootstrap = @{}
        if ($NativeOnly) { $bootstrap.NativeOnly = $true } else { $bootstrap.Fast = $true }
        Invoke-PsScript (Join-Path $root 'scripts/bootstrap.ps1') $bootstrap
    }

    Write-Step 'CMake configure'
    Invoke-Native 'cmake' @('-S', '.', '-B', $BuildDir, '-DAVATAR_TESTS=ON',
        '-DAVATAR_BENCHMARKS=ON')

    Write-Step "build $Config"
    Invoke-Native 'cmake' @('--build', $BuildDir, '--config', $Config, '--parallel')

    Write-Step 'CTest fast suite'
    if ($Full) {
        Invoke-Native 'ctest' @('--test-dir', $BuildDir, '-C', $Config, '--output-on-failure')
    } else {
        Invoke-Native 'ctest' @('--test-dir', $BuildDir, '-C', $Config, '-L', 'fast',
            '-LE', 'perf', '--output-on-failure')
    }

    if (-not $NativeOnly) {
        Write-Step 'headless Godot shell smoke, runtime pacing and GLB import tests'
        Invoke-PsScript (Join-Path $root 'scripts/smoke.ps1') @{ Required = $true; Runtime = $true; Model = $true }
    }

    Write-Host ''
    Write-Host 'ci: PASS' -ForegroundColor Green
} catch {
    Write-Host "ci: FAIL - $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
} finally {
    Pop-Location
}

exit $exitCode
