<#
.SYNOPSIS
    Build and run the Avatar Studio performance harness.

.DESCRIPTION
    Builds RelWithDebInfo, runs avatar_bench with the requested warmup and
    measurement frames, pins the process priority to Normal, and writes the
    result JSON to tests/performance/results/ (gitignored).

    The bench itself discards the warmup frames; the script does not run a
    separate process for warmup because the point is to warm the same process's
    caches and ring slots.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tests/performance/run.ps1

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tests/performance/run.ps1 `
        -Scenario rig -Frames 300 -Warmup 30
#>
[CmdletBinding()]
param(
    [ValidateSet('capture', 'ring', 'rig', 'render', 'all')]
    [string]$Scenario = 'rig',
    [int]$Frames = 300,
    [int]$Warmup = 30,
    [int]$Width = 1280,
    [int]$Height = 720,
    [ValidateSet('scripted', 'real')]
    [string]$Tracker = 'scripted',
    [int]$Fps = 60,
    [string]$BuildDir = 'build',
    [string]$Config = 'RelWithDebInfo',
    [string]$OutDir = 'tests/performance/results',
    [string]$OutFile = '',
    [switch]$NoBuild,
    [switch]$NoValidate
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

function Write-Command([string]$Text) {
    Write-Host ">> $Text" -ForegroundColor Cyan
}

function Quote-Argument([string]$Value) {
    if ($Value -match '[\s|;&<>]') {
        return '"{0}"' -f $Value
    }
    return $Value
}

Push-Location $root
try {
    if (-not $NoBuild) {
        Write-Command "cmake -S . -B $BuildDir -DAVATAR_TESTS=ON -DAVATAR_BENCHMARKS=ON"
        & cmake -S . -B $BuildDir -DAVATAR_TESTS=ON -DAVATAR_BENCHMARKS=ON
        if ($LASTEXITCODE -ne 0) {
            throw "cmake configure failed with exit code $LASTEXITCODE"
        }

        Write-Command "cmake --build $BuildDir --config $Config --target avatar_bench --parallel"
        & cmake --build $BuildDir --config $Config --target avatar_bench --parallel
        if ($LASTEXITCODE -ne 0) {
            throw "cmake build failed with exit code $LASTEXITCODE"
        }
    }

    $exeCandidates = @(
        (Join-Path $root "$BuildDir/$Config/avatar_bench.exe"),
        (Join-Path $root "$BuildDir/avatar_bench.exe"),
        (Join-Path $root "$BuildDir/avatar_bench")
    )
    $exe = $exeCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $exe) {
        throw "avatar_bench executable not found under $BuildDir (build first or drop -NoBuild)"
    }

    $results = if ([System.IO.Path]::IsPathRooted($OutDir)) {
        $OutDir
    } else {
        Join-Path $root $OutDir
    }
    New-Item -ItemType Directory -Force -Path $results | Out-Null

    $scenarios = if ($Scenario -eq 'all') {
        @('capture', 'ring', 'rig', 'render')
    } else {
        @($Scenario)
    }
    # PowerShell unwraps a single-element array from an if expression.
    $scenarios = @($scenarios)
    if ($OutFile -ne '' -and $scenarios.Count -gt 1) {
        throw "-OutFile is only valid with a single -Scenario"
    }

    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $checkScript = Join-Path $root 'tests/performance/check.mjs'
    $node = Get-Command node -ErrorAction SilentlyContinue

    foreach ($current in $scenarios) {
        if ($OutFile -ne '') {
            $outputPath = if ([System.IO.Path]::IsPathRooted($OutFile)) {
                $OutFile
            } else {
                Join-Path $root $OutFile
            }
        } else {
            $outputPath = Join-Path $results ("{0}-{1}.json" -f $current, $stamp)
        }

        $arguments = @(
            '--scenario', $current,
            '--frames', [string]$Frames,
            '--warmup', [string]$Warmup,
            '--width', [string]$Width,
            '--height', [string]$Height,
            '--tracker', $Tracker,
            '--fps', [string]$Fps,
            '--output', (Quote-Argument $outputPath)
        )

        Write-Command ("{0} {1}" -f $exe, ($arguments -join ' '))
        $startInfo = New-Object System.Diagnostics.ProcessStartInfo
        $startInfo.FileName = $exe
        $startInfo.Arguments = ($arguments -join ' ')
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $process = New-Object System.Diagnostics.Process
        $process.StartInfo = $startInfo
        $null = $process.Start()
        try {
            $process.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::Normal
        } catch {
            Write-Warning "could not pin process priority to Normal: $($_.Exception.Message)"
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $stderr = $stderrTask.Result
        $stdout = $stdoutTask.Result
        if ($stderr) {
            Write-Host $stderr.TrimEnd()
        }
        if ($stdout) {
            Write-Host $stdout.TrimEnd()
        }
        $exitCode = $process.ExitCode
        $process.Dispose()
        if ($exitCode -ne 0) {
            throw "avatar_bench failed with exit code $exitCode ($current)"
        }
        if (-not (Test-Path -LiteralPath $outputPath)) {
            throw "avatar_bench did not write $outputPath"
        }

        if (-not $NoValidate) {
            if ($node) {
                Write-Command "node $checkScript --validate $outputPath"
                & node $checkScript --validate $outputPath
                if ($LASTEXITCODE -ne 0) {
                    throw "result validation failed for $outputPath"
                }
            } else {
                Write-Warning "node not found; skipping JSON validation for $outputPath"
            }
        }

        $latest = Join-Path $results ("latest-{0}.json" -f $current)
        Copy-Item -LiteralPath $outputPath -Destination $latest -Force
        Write-Host "result: $outputPath" -ForegroundColor Green
        Write-Host "latest: $latest" -ForegroundColor Green
        Write-Host "reproduce: $exe --scenario $current --frames $Frames --warmup $Warmup --width $Width --height $Height --fps $Fps --output <file>"
    }
} finally {
    Pop-Location
}
