<#
.SYNOPSIS
    The verification contract (Windows).

.DESCRIPTION
    -Fast : bootstrap, configure, build RelWithDebInfo, run CTest label "fast"
            (excluding the "perf" label), then run the headless Godot shell smoke
            test when the pinned engine and app/project.godot are present.
    -Full : -Fast plus the complete native CTest suite, including the "perf"
            label, plus tests/gates/<Task>.ps1.

    The gate script for -Task TNN is tests/gates/TNN.ps1 when present. It runs
    in both modes; the AVATAR_VERIFY_MODE environment variable tells the gate
    whether it is a fast or full run (T05 uses it to skip the measurement run
    in -Fast).

    With -Task T01 the bootstrap runs with -NativeOnly so the framework-free
    core still verifies on a machine without Godot. With -Task T02 or later the
    script requires the pinned Godot 4.7.2 binary, the GDExtension artifact, and
    a passing app/tests/smoke.gd; it fails rather than silently skipping them.
    Every command is printed before it runs. The script exits non-zero if any
    step fails.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify.ps1 -Fast -Task T02

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify.ps1 -Full -Task T02
#>
[CmdletBinding()]
param(
    [switch]$Fast,
    [switch]$Full,
    [string]$Task = '',
    [string]$BuildDir = 'build',
    [string]$Config = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($Fast -and $Full) {
    Write-Error "Choose either -Fast or -Full, not both."
    exit 2
}
if (-not $Fast -and -not $Full) {
    $Fast = $true
}
if ($Task -ne '') {
    $Task = $Task.ToUpperInvariant()
    if ($Task -notmatch '^T[0-9]{2,3}$') {
        Write-Error "Invalid -Task '$Task': expected a task id such as T01."
        exit 2
    }
}

$root = Split-Path -Parent $PSScriptRoot
$mode = if ($Full) { 'full' } else { 'fast' }
$taskNumber = if ($Task -ne '') { [int]$Task.Substring(1) } else { 0 }
$requiresGodot = ($Task -ne '' -and $taskNumber -ge 2)

function Write-Command([string]$Text) {
    Write-Host ">> $Text" -ForegroundColor Cyan
}

function Invoke-Native([string]$File, [string[]]$Arguments) {
    $quoted = foreach ($argument in $Arguments) {
        if ($argument -match '[\s|;&<>]') { '"{0}"' -f $argument } else { $argument }
    }
    Write-Command ("{0} {1}" -f $File, ($quoted -join ' '))
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "command failed with exit code ${LASTEXITCODE}: $File"
    }
}

function Invoke-Script([string]$Path, [hashtable]$Parameters = @{}) {
    $argumentText = ($Parameters.GetEnumerator() | ForEach-Object { "-$($_.Key)" }) -join ' '
    Write-Command ("& `"{0}`" {1}" -f $Path, $argumentText)
    & $Path @Parameters
    if ($LASTEXITCODE -ne 0) {
        throw "script failed with exit code ${LASTEXITCODE}: $Path"
    }
}

$exitCode = 0
Push-Location $root
try {
    Write-Host "verify: mode=$mode task=$(if ($Task) { $Task } else { '(none)' }) build=$BuildDir config=$Config"

    if ($Task -eq 'T01') {
        Invoke-Script (Join-Path $root 'scripts/bootstrap.ps1') @{ NativeOnly = $true }
    } else {
        Invoke-Script (Join-Path $root 'scripts/bootstrap.ps1') @{ Fast = $true }
    }

    $configureArguments = @('-S', '.', '-B', $BuildDir, '-DAVATAR_TESTS=ON', '-DAVATAR_BENCHMARKS=ON')
    if ($Task -eq 'T11') {
        Invoke-Script (Join-Path $root 'scripts/build-tracker.ps1')
        $configureArguments += '-DAVATAR_REAL_TRACKER=ON'
    }
    Invoke-Native 'cmake' $configureArguments
    Invoke-Native 'cmake' @('--build', $BuildDir, '--config', $Config, '--parallel')

    if ($Full) {
        # Full runs the non-perf suite, then the perf label explicitly. Keeping
        # them separate makes the hardware-dependent run visible in the log and
        # lets a future task add a perf test without touching this script.
        Invoke-Native 'ctest' @('--test-dir', $BuildDir, '-C', $Config, '-LE', 'perf', '--output-on-failure')
        $perfList = (& ctest --test-dir $BuildDir -C $Config -N -L perf 2>&1) -join "`n"
        if ($LASTEXITCODE -eq 0 -and $perfList -match 'Test #') {
            Invoke-Native 'ctest' @('--test-dir', $BuildDir, '-C', $Config, '-L', 'perf', '--output-on-failure')
        } else {
            Write-Host "verify: no perf-labelled tests registered, skipped"
        }
    } elseif ($Task -ne '') {
        # Perf tests are opt-in (-Full). -LE perf keeps -Fast hardware-independent.
        Invoke-Native 'ctest' @('--test-dir', $BuildDir, '-C', $Config, '-L', "fast|task_$Task", '-LE', 'perf', '--output-on-failure')
    } else {
        Invoke-Native 'ctest' @('--test-dir', $BuildDir, '-C', $Config, '-L', 'fast', '-LE', 'perf', '--output-on-failure')
    }

    $smokeParameters = @{}
    if ($requiresGodot) {
        $smokeParameters.Required = $true
    }
    if ($taskNumber -ge 16) {
        # T16 adds the one-viewport configuration and 10-second paced run.
        $smokeParameters.Runtime = $true
    }
    if ($taskNumber -ge 17) {
        # T17 adds the runtime GLB import, fixtures and app-scene test.
        $smokeParameters.Model = $true
    }
    Invoke-Script (Join-Path $root 'scripts/smoke.ps1') $smokeParameters

    # The planning workspace is gitignored, so this is a local-only check. It
    # keeps the [camera+person] roadmap tag, the task's requires: front matter,
    # and its "## Preflight" section in sync. Clean checkouts skip it.
    if (Test-Path -LiteralPath (Join-Path $root 'docs/ROADMAP.md')) {
        Invoke-Native 'node' @('scripts/check-task-preflight.mjs')
    }

    if ($Task -ne '') {
        $gate = Join-Path $root ("tests/gates/{0}.ps1" -f $Task)
        if (Test-Path -LiteralPath $gate) {
            # Gates read this to decide whether a hardware-dependent measurement
            # is expected (T05). Older gates ignore it.
            $env:AVATAR_VERIFY_MODE = $mode
            try {
                Invoke-Script $gate
            } finally {
                Remove-Item Env:\AVATAR_VERIFY_MODE -ErrorAction SilentlyContinue
            }
        } else {
            Write-Host "verify: no gate script for $Task (tests/gates/$Task.ps1 not present), skipped"
        }
    } elseif ($Full) {
        Write-Host "verify: -Full without -Task runs no task gate (pass -Task TNN to run one)"
    }

    Write-Host "verify: PASS ($mode)" -ForegroundColor Green
} catch {
    Write-Host "verify: FAIL - $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
} finally {
    Pop-Location
}

exit $exitCode
