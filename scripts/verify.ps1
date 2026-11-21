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

function Get-GodotExecutable([string]$Root) {
    $candidates = @(
        (Join-Path $Root '.tools/godot/Godot_v4.7.2-stable_win64_console.exe'),
        (Join-Path $Root '.tools/godot/Godot_v4.7.2-stable_linux.x86_64'),
        (Join-Path $Root '.tools/godot/Godot.app/Contents/MacOS/Godot')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    return ''
}

function Ensure-GodotExtensionList([string]$ProjectDir) {
    # Godot only scans res://*.gdextension into .godot/extension_list.cfg during
    # an editor import. In 4.7.2 the headless editor exits with an access
    # violation on shutdown after writing that file, so seed the one-line list
    # directly; the headless smoke test is the authoritative load check.
    $list = Join-Path $ProjectDir '.godot/extension_list.cfg'
    if (Test-Path -LiteralPath $list) {
        return
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $list) | Out-Null
    Set-Content -LiteralPath $list -Value 'res://bin/avatar_studio.gdextension' -NoNewline
    Write-Host "verify: seeded $list (headless editor import is unreliable in Godot 4.7.2)"
}

function Invoke-GodotSmoke([string]$Godot, [string]$ProjectDir) {
    Ensure-GodotExtensionList $ProjectDir
    $arguments = @('--headless', '--path', $ProjectDir, '--script', 'res://tests/smoke.gd')
    Write-Command ("{0} {1}" -f $Godot, ($arguments -join ' '))
    $output = & $Godot @arguments 2>&1
    $code = $LASTEXITCODE
    foreach ($line in $output) {
        Write-Host ([string]$line)
    }
    if ($code -ne 0) {
        throw "Godot shell smoke test failed with exit code $code"
    }

    $text = ($output | Out-String)
    $fatalPatterns = @(
        'Failed to load GDExtension',
        'GDExtension load failed',
        'Cannot open shared library',
        'SCRIPT ERROR',
        'Parse Error'
    )
    foreach ($pattern in $fatalPatterns) {
        if ($text -match [regex]::Escape($pattern)) {
            throw "Godot shell smoke output contains '$pattern'"
        }
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

    Invoke-Native 'cmake' @('-S', '.', '-B', $BuildDir, '-DAVATAR_TESTS=ON', '-DAVATAR_BENCHMARKS=ON')
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

    $godot = Get-GodotExecutable $root
    $project = Join-Path $root 'app/project.godot'
    if ($godot -eq '') {
        if ($requiresGodot) {
            throw ("Godot 4.7.2 is missing from .tools/godot. Run scripts/bootstrap.ps1 -Fast. " +
                "A native-only run is only allowed for T01.")
        }
        Write-Host "verify: Godot not present, skipping the shell smoke (native-only)"
    } elseif (-not (Test-Path -LiteralPath $project)) {
        if ($requiresGodot) {
            throw "app/project.godot is missing; the Godot shell is required for $Task"
        }
        Write-Host "verify: app/project.godot not present, skipping the shell smoke"
    } else {
        if ($requiresGodot) {
            $extension = Join-Path $root 'app/bin/avatar_studio.windows.x86_64.dll'
            if (-not (Test-Path -LiteralPath $extension)) {
                throw ("The GDExtension artifact is missing at $extension. " +
                    "Check that .tools/godot-cpp exists and the build completed.")
            }
        }
        Invoke-GodotSmoke $godot (Join-Path $root 'app')
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
