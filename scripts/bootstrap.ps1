#requires -Version 5.1
<#
.SYNOPSIS
    Fetch the pinned native dependencies into .tools/ (Windows).

.DESCRIPTION
    Idempotent: an already verified dependency is left alone. Every download is
    SHA256 checked before it is installed. CMake consumes .tools/ directly and
    never downloads anything itself.

.PARAMETER Force
    Re-download and re-install even when the local copy already verifies.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts/bootstrap.ps1
#>
[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Windows PowerShell 5.1 defaults to old TLS on some machines.
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
} catch {
    # Not fatal on PowerShell 7, where this is ignored.
}

$root = Split-Path -Parent $PSScriptRoot
$toolsDir = Join-Path $root '.tools'
$downloadsDir = Join-Path $toolsDir 'downloads'

$deps = @(
    [pscustomobject]@{
        Name    = 'nlohmann_json'
        Version = '3.11.3'
        Url     = 'https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp'
        Sha256  = '9BEA4C8066EF4A1C206B2BE5A36302F8926F7FDC6087AF5D20B417D0CF103EA6'
        File    = 'json.hpp'
        Dest    = 'nlohmann_json/include/nlohmann/json.hpp'
    }
)

# SHA256 without assuming any particular cmdlet is present. Locked-down hosts (restricted
# language mode, trimmed module sets, some CI agents) do not expose Get-FileHash even though
# Write-Host works, so the .NET implementation is the primary path and the cmdlet is a fallback.
function Get-Sha256([string]$Path) {
    $algorithm = $null
    try {
        $algorithm = [System.Security.Cryptography.SHA256]::Create()
    } catch {
        $algorithm = $null
    }
    if ($null -ne $algorithm) {
        $stream = $null
        try {
            $stream = [System.IO.File]::Open(
                $Path,
                [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::Read,
                [System.IO.FileShare]::Read)
            $bytes = $algorithm.ComputeHash($stream)
            return ([System.BitConverter]::ToString($bytes) -replace '-', '')
        } catch {
            # Fall through to the cmdlet / OS tool paths below.
        } finally {
            if ($null -ne $stream) { $stream.Dispose() }
            $algorithm.Dispose()
        }
    }

    $fileHash = Get-Command Get-FileHash -ErrorAction SilentlyContinue
    if ($null -ne $fileHash) {
        return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    }

    $certUtil = Get-Command certutil.exe -ErrorAction SilentlyContinue
    if ($null -ne $certUtil) {
        $output = & $certUtil.Source -hashfile $Path SHA256 2>&1
        if ($LASTEXITCODE -eq 0) {
            foreach ($line in $output) {
                $candidate = ([string]$line) -replace '\s', ''
                if ($candidate -match '^[0-9A-Fa-f]{64}$') {
                    return $candidate.ToUpperInvariant()
                }
            }
        }
    }

    throw "cannot compute SHA256 for $Path (tried .NET, Get-FileHash and certutil)"
}

# Download with the same rule: .NET first, Invoke-WebRequest as the fallback.
function Save-Download([string]$Url, [string]$Destination) {
    $client = $null
    try {
        $client = New-Object System.Net.WebClient
    } catch {
        $client = $null
    }
    if ($null -ne $client) {
        try {
            $client.Headers.Add('User-Agent', 'avatar-studio-bootstrap')
            $client.DownloadFile($Url, $Destination)
            return
        } catch {
            if (Test-Path -LiteralPath $Destination) {
                Remove-Item -LiteralPath $Destination -Force -ErrorAction SilentlyContinue
            }
            Write-Host "  note     .NET download failed ($($_.Exception.Message)); retrying with Invoke-WebRequest"
        } finally {
            $client.Dispose()
        }
    }
    Invoke-WebRequest -Uri $Url -OutFile $Destination -UseBasicParsing
}

function Install-Dependency([pscustomobject]$Dep) {
    $destPath = Join-Path $toolsDir $Dep.Dest
    $markerPath = Join-Path (Join-Path $toolsDir $Dep.Name) '.pinned'
    $markerText = '{0} {1} sha256={2}' -f $Dep.Name, $Dep.Version, $Dep.Sha256

    if (-not $Force -and (Test-Path -LiteralPath $destPath) -and (Test-Path -LiteralPath $markerPath)) {
        $markerOk = ((Get-Content -LiteralPath $markerPath -Raw).Trim() -eq $markerText)
        $hashOk = ((Get-Sha256 $destPath) -eq $Dep.Sha256)
        if ($markerOk -and $hashOk) {
            Write-Host "  ok       $($Dep.Name) $($Dep.Version) (verified, skipped)"
            return
        }
    }

    $cachePath = Join-Path $downloadsDir $Dep.File
    $cacheOk = (Test-Path -LiteralPath $cachePath) -and ((Get-Sha256 $cachePath) -eq $Dep.Sha256)
    if (-not $Force -and $cacheOk) {
        Write-Host "  cache    $($Dep.Name) $($Dep.Version) (verified download cache)"
    } else {
        New-Item -ItemType Directory -Force -Path $downloadsDir | Out-Null
        $partialPath = "$cachePath.part"
        if (Test-Path -LiteralPath $partialPath) {
            Remove-Item -LiteralPath $partialPath -Force
        }
        Write-Host "  download $($Dep.Name) $($Dep.Version)"
        Write-Host "           $($Dep.Url)"
        Save-Download -Url $Dep.Url -Destination $partialPath
        $actual = Get-Sha256 $partialPath
        if ($actual -ne $Dep.Sha256) {
            Remove-Item -LiteralPath $partialPath -Force -ErrorAction SilentlyContinue
            throw "SHA256 mismatch for $($Dep.Name) $($Dep.Version): expected $($Dep.Sha256), got $actual"
        }
        Move-Item -LiteralPath $partialPath -Destination $cachePath -Force
    }

    $destDir = Split-Path -Parent $destPath
    New-Item -ItemType Directory -Force -Path $destDir | Out-Null
    Copy-Item -LiteralPath $cachePath -Destination $destPath -Force

    $installedHash = Get-Sha256 $destPath
    if ($installedHash -ne $Dep.Sha256) {
        throw "Installed copy of $($Dep.Name) failed SHA256 verification: $installedHash"
    }

    Set-Content -LiteralPath $markerPath -Value $markerText -NoNewline
    Write-Host "  install  $($Dep.Name) $($Dep.Version) -> $destPath"
}

Write-Host "bootstrap: pinned dependencies in $toolsDir"
foreach ($dep in $deps) {
    Install-Dependency -Dep $dep
}
Write-Host "bootstrap: done. Configure with: cmake -S . -B build"
exit 0
