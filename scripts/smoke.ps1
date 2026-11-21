#requires -Version 5.1
<#
.SYNOPSIS
    Run the headless Godot shell smoke test (Windows PowerShell / pwsh).

.DESCRIPTION
    Finds the pinned Godot 4.7.2 binary in .tools/godot, seeds the extension
    list that the 4.7.2 headless editor fails to write reliably, runs
    app/tests/smoke.gd and fails on a non-zero exit or a fatal engine message.

    With -Required a missing engine, project or GDExtension artifact is an
    error. Without it, a machine without Godot skips the smoke (the native-only
    T01 path). verify.ps1 and scripts/ci.ps1 use this script so the local and
    CI contracts share one implementation.

.PARAMETER Required
    Fail instead of skipping when Godot or the GDExtension artifact is absent.

.PARAMETER ProjectDir
    Godot project directory. Defaults to <root>/app.

.PARAMETER Root
    Repository root. Defaults to the parent of this script.

.PARAMETER Runtime
    Also run app/tests/runtime.gd: the T16 one-viewport configuration and
    10-second paced run. The JSON summary is written to
    <root>/.tools/T16-runtime.json for tests/gates/T16.ps1.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts/smoke.ps1 -Required
#>
[CmdletBinding()]
param(
    [switch]$Required,
    [switch]$Runtime,
    [string]$ProjectDir = '',
    [string]$Root = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($Root -eq '') {
    $Root = Split-Path -Parent $PSScriptRoot
}
$Root = (Resolve-Path -LiteralPath $Root).Path
if ($ProjectDir -eq '') {
    $ProjectDir = Join-Path $Root 'app'
}

function Find-Godot([string]$RepositoryRoot) {
    $candidates = @(
        (Join-Path $RepositoryRoot '.tools/godot/Godot_v4.7.2-stable_win64_console.exe'),
        (Join-Path $RepositoryRoot '.tools/godot/Godot_v4.7.2-stable_linux.x86_64'),
        (Join-Path $RepositoryRoot '.tools/godot/Godot.app/Contents/MacOS/Godot')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    return ''
}

function Get-ExtensionArtifact([string]$GodotPath, [string]$RepositoryRoot) {
    if ($GodotPath -match 'win64') {
        return (Join-Path $RepositoryRoot 'app/bin/avatar_studio.windows.x86_64.dll')
    }
    if ($GodotPath -match 'linux') {
        return (Join-Path $RepositoryRoot 'app/bin/avatar_studio.linux.x86_64.so')
    }
    return (Join-Path $RepositoryRoot 'app/bin/avatar_studio.macos.x86_64.dylib')
}

function Ensure-ExtensionList([string]$ProjectDirectory) {
    # Godot only scans res://*.gdextension into .godot/extension_list.cfg during
    # an editor import. In 4.7.2 the headless editor exits with an access
    # violation on shutdown after writing that file, so seed the one-line list
    # directly; the headless smoke test is the authoritative load check.
    $list = Join-Path $ProjectDirectory '.godot/extension_list.cfg'
    if (Test-Path -LiteralPath $list) {
        return
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $list) | Out-Null
    Set-Content -LiteralPath $list -Value 'res://bin/avatar_studio.gdextension' -NoNewline
    Write-Host "smoke: seeded $list (headless editor import is unreliable in Godot 4.7.2)"
}

$godot = Find-Godot $Root
if ($godot -eq '') {
    if ($Required) {
        Write-Host ('smoke: FAIL - Godot 4.7.2 is missing from .tools/godot. ' +
            'Run scripts/bootstrap.ps1 -Fast.') -ForegroundColor Red
        exit 1
    }
    Write-Host 'smoke: Godot not present, skipping (native-only)'
    exit 0
}

$project = Join-Path $ProjectDir 'project.godot'
if (-not (Test-Path -LiteralPath $project)) {
    if ($Required) {
        Write-Host "smoke: FAIL - $project is missing; the Godot shell is required" -ForegroundColor Red
        exit 1
    }
    Write-Host "smoke: $project not present, skipping"
    exit 0
}

if ($Required) {
    $extension = Get-ExtensionArtifact $godot $Root
    if (-not (Test-Path -LiteralPath $extension)) {
        Write-Host ("smoke: FAIL - the GDExtension artifact is missing at $extension. " +
            'Check that .tools/godot-cpp exists and the build completed.') -ForegroundColor Red
        exit 1
    }
}

Ensure-ExtensionList $ProjectDir

$fatalPatterns = @(
    'Failed to load GDExtension',
    'GDExtension load failed',
    'Cannot open shared library',
    'SCRIPT ERROR',
    'Parse Error'
)

function Invoke-GodotScript([string]$Script, [string]$Label) {
    $arguments = @('--headless', '--path', $ProjectDir, '--script', $Script)
    Write-Host (">> {0} {1}" -f $godot, ($arguments -join ' '))
    $output = & $godot @arguments 2>&1
    $code = $LASTEXITCODE
    foreach ($line in $output) {
        Write-Host ([string]$line)
    }
    if ($code -ne 0) {
        Write-Host "smoke: FAIL - $Label exited $code" -ForegroundColor Red
        exit 1
    }

    $text = ($output | Out-String)
    foreach ($pattern in $fatalPatterns) {
        if ($text -match [regex]::Escape($pattern)) {
            Write-Host "smoke: FAIL - $Label output contains '$pattern'" -ForegroundColor Red
            exit 1
        }
    }
}

Invoke-GodotScript 'res://tests/smoke.gd' 'Godot shell smoke test'

if ($Runtime) {
    $runtimeScript = Join-Path $ProjectDir 'tests/runtime.gd'
    if (-not (Test-Path -LiteralPath $runtimeScript)) {
        if ($Required) {
            Write-Host "smoke: FAIL - $runtimeScript is missing; the T16 runtime test is required" -ForegroundColor Red
            exit 1
        }
        Write-Host "smoke: $runtimeScript not present, skipping runtime test"
    } else {
        $resultDir = Join-Path $Root '.tools'
        New-Item -ItemType Directory -Force -Path $resultDir | Out-Null
        $resultPath = Join-Path $resultDir 'T16-runtime.json'
        $previousResult = $env:AVATAR_RUNTIME_RESULT
        $env:AVATAR_RUNTIME_RESULT = $resultPath
        try {
            Invoke-GodotScript 'res://tests/runtime.gd' 'runtime pacing test'
        } finally {
            if ($null -eq $previousResult) {
                Remove-Item Env:\AVATAR_RUNTIME_RESULT -ErrorAction SilentlyContinue
            } else {
                $env:AVATAR_RUNTIME_RESULT = $previousResult
            }
        }
        Write-Host "smoke: runtime result $resultPath"
    }
}

Write-Host 'smoke: PASS'
exit 0
