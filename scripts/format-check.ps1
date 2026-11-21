#requires -Version 5.1
<#
.SYNOPSIS
    Check (or apply) the pinned clang-format style.

.DESCRIPTION
    Runs clang-format 18.1.8 with the repository .clang-format over every C++
    source in native/, tests/ and app/. CI installs clang-format and runs this
    with -Require, so drift fails the build.

    A developer machine without clang-format prints a skip and exits 0 unless
    -Require (or AVATAR_REQUIRE_FORMAT=1) is set. That keeps the fast local
    path usable without a full LLVM install while the CI gate stays strict.

.PARAMETER Root
    Repository root. Defaults to the parent of this script.

.PARAMETER ClangFormat
    Path to the clang-format executable. Defaults to $env:CLANG_FORMAT, then
    clang-format on PATH, then .tools/clang-format/clang-format.exe.

.PARAMETER Require
    Fail when clang-format is not available.

.PARAMETER Fix
    Rewrite files in place instead of reporting drift.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format-check.ps1

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format-check.ps1 -Require -Fix
#>
[CmdletBinding()]
param(
    [string]$Root = '',
    [string]$ClangFormat = '',
    [switch]$Require,
    [switch]$Fix
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($Root -eq '') {
    $Root = Split-Path -Parent $PSScriptRoot
}
$Root = (Resolve-Path -LiteralPath $Root).Path

$stylePath = Join-Path $Root '.clang-format'
if (-not (Test-Path -LiteralPath $stylePath)) {
    Write-Host "format-check: FAIL - .clang-format is missing from $Root" -ForegroundColor Red
    exit 1
}

function Test-ClangFormat([string]$Path) {
    if ($Path -eq '' -or -not (Test-Path -LiteralPath $Path)) { return $false }
    try {
        $null = & $Path --version 2>&1
        return ($LASTEXITCODE -eq 0)
    } catch {
        return $false
    }
}

function Find-ClangFormat([string]$Requested) {
    if ($Requested -ne '') {
        if (Test-Path -LiteralPath $Requested) {
            $resolved = (Resolve-Path -LiteralPath $Requested).Path
            if (Test-ClangFormat $resolved) { return $resolved }
            return ''
        }
        $command = Get-Command $Requested -ErrorAction SilentlyContinue
        if ($null -ne $command -and (Test-ClangFormat $command.Source)) { return $command.Source }
        return ''
    }
    if ($env:CLANG_FORMAT -and $env:CLANG_FORMAT -ne '') {
        if (Test-Path -LiteralPath $env:CLANG_FORMAT) {
            $resolved = (Resolve-Path -LiteralPath $env:CLANG_FORMAT).Path
            if (Test-ClangFormat $resolved) { return $resolved }
        } else {
            $command = Get-Command $env:CLANG_FORMAT -ErrorAction SilentlyContinue
            if ($null -ne $command -and (Test-ClangFormat $command.Source)) { return $command.Source }
        }
    }
    $command = Get-Command clang-format -ErrorAction SilentlyContinue
    if ($null -ne $command -and (Test-ClangFormat $command.Source)) { return $command.Source }
    # The pip package ships a launcher shim in bin/ that needs its module on
    # sys.path; prefer the real binary in its data directory so a repository-local
    # install works without activating the venv.
    foreach ($candidate in @(
            (Join-Path $Root '.tools/clang-format/clang_format/data/bin/clang-format.exe'),
            (Join-Path $Root '.tools/clang-format/clang-format.exe'),
            (Join-Path $Root '.tools/clang-format/bin/clang-format.exe'),
            (Join-Path $Root '.tools/llvm/bin/clang-format.exe'))) {
        if (Test-ClangFormat $candidate) { return $candidate }
    }
    return ''
}

$executable = Find-ClangFormat $ClangFormat
if ($executable -eq '') {
    $required = $Require -or ($env:AVATAR_REQUIRE_FORMAT -eq '1')
    if ($required) {
        Write-Host ("format-check: FAIL - clang-format is required but was not found. " +
            "Install clang-format 18.1.8 or set CLANG_FORMAT.") -ForegroundColor Red
        exit 1
    }
    Write-Host ("format-check: SKIP - clang-format not found. " +
        "Install clang-format 18.1.8 (CI pins it) or set CLANG_FORMAT. " +
        "Set AVATAR_REQUIRE_FORMAT=1 to make this fatal.")
    exit 0
}

$excluded = '\\(build|build-[^\\]*|\.tools|\.git|out|artifacts|node_modules|results|\.godot)\\'
$extensions = @('.h', '.hh', '.hpp', '.hxx', '.ipp', '.inl', '.c', '.cc', '.cpp', '.cxx')
$files = @()
foreach ($tree in @('native', 'tests', 'app')) {
    $directory = Join-Path $Root $tree
    if (-not (Test-Path -LiteralPath $directory)) { continue }
    $files += @(
        Get-ChildItem -LiteralPath $directory -Recurse -File |
            Where-Object { $extensions -contains $_.Extension } |
            Where-Object { $_.FullName -notmatch $excluded } |
            ForEach-Object { $_.FullName }
    )
}
if ($files.Count -eq 0) {
    Write-Host 'format-check: PASS (no C++ sources found)'
    exit 0
}

$version = (& $executable --version 2>&1 | Out-String).Trim()
Write-Host "format-check: $version on $($files.Count) files"

$previousPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    if ($Fix) {
        & $executable -i --style=file @files 2>&1 | ForEach-Object { Write-Host ([string]$_) }
        $code = $LASTEXITCODE
        if ($code -ne 0) {
            Write-Host "format-check: FAIL - clang-format -i exited $code" -ForegroundColor Red
            exit 1
        }
        Write-Host 'format-check: PASS (files rewritten)'
        exit 0
    }

    $output = & $executable --dry-run --Werror --style=file @files 2>&1
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        $lines = @($output | ForEach-Object { [string]$_ })
        $lines | Select-Object -First 60 | ForEach-Object { Write-Host $_ }
        if ($lines.Count -gt 60) {
            Write-Host ("format-check: ... {0} more line(s)" -f ($lines.Count - 60))
        }
        Write-Host ('format-check: FAIL - formatting drift. Run ' +
            'scripts/format-check.ps1 -Fix, review, and commit.') -ForegroundColor Red
        exit 1
    }
} finally {
    $ErrorActionPreference = $previousPreference
}

Write-Host 'format-check: PASS'
exit 0
