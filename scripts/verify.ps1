<#
.SYNOPSIS
    The verification contract for native work (Windows).

.DESCRIPTION
    -Fast : bootstrap, configure, build RelWithDebInfo, run CTest label "fast".
    -Full : -Fast plus the complete native CTest suite plus tests/gates/<Task>.ps1.

    With -Task T01 the CTest filter also matches label "task_T01", and the task
    gate script is run when it exists. Every command is printed before it runs.
    The script exits non-zero if any step fails.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify.ps1 -Fast -Task T01

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify.ps1 -Full -Task T01
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

function Invoke-Script([string]$Path) {
    Write-Command ("& `"{0}`"" -f $Path)
    & $Path
    if ($LASTEXITCODE -ne 0) {
        throw "script failed with exit code ${LASTEXITCODE}: $Path"
    }
}

$exitCode = 0
Push-Location $root
try {
    Write-Host "verify: mode=$mode task=$(if ($Task) { $Task } else { '(none)' }) build=$BuildDir config=$Config"

    Invoke-Script (Join-Path $root 'scripts/bootstrap.ps1')

    Invoke-Native 'cmake' @('-S', '.', '-B', $BuildDir, '-DAVATAR_TESTS=ON')
    Invoke-Native 'cmake' @('--build', $BuildDir, '--config', $Config, '--parallel')

    if ($Full) {
        Invoke-Native 'ctest' @('--test-dir', $BuildDir, '-C', $Config, '--output-on-failure')
    } elseif ($Task -ne '') {
        Invoke-Native 'ctest' @('--test-dir', $BuildDir, '-C', $Config, '-L', "fast|task_$Task", '--output-on-failure')
    } else {
        Invoke-Native 'ctest' @('--test-dir', $BuildDir, '-C', $Config, '-L', 'fast', '--output-on-failure')
    }

    if ($Task -ne '') {
        $gate = Join-Path $root ("tests/gates/{0}.ps1" -f $Task)
        if (Test-Path -LiteralPath $gate) {
            Invoke-Script $gate
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
