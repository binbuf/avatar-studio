#requires -Version 5.1
<#
.SYNOPSIS
    Fetch the pinned native and Godot dependencies into .tools/ (Windows).

.DESCRIPTION
    Idempotent: an already verified dependency is left alone. Every download is
    SHA256 checked before it is installed. CMake consumes .tools/ directly and
    never downloads anything itself.

    Pinned versions and exact download URLs (also recorded in docs/progress/T02.md):

      Godot 4.7.2 stable (released 2026-08-18)
        https://github.com/godotengine/godot/releases/download/4.7.2-stable/Godot_v4.7.2-stable_win64.exe.zip
        SHA256 731980F9608D61333E5BAF54A2EF17210ACC7A538446C0CB9969F002ACA1E953
        Installed at .tools/godot/Godot_v4.7.2-stable_win64_console.exe

      godot-cpp 10.0.0-stable (released 2026-09-15), built with GODOTCPP_API_VERSION=4.7
        https://github.com/godotengine/godot-cpp/archive/refs/tags/10.0.0-stable.zip
        SHA256 C763A08B8EC18DAE300CD54BB8848FAF1A5FD11DA629449D16649A72A1884727
        Installed at .tools/godot-cpp/

      nlohmann/json 3.11.3
        https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
        SHA256 9BEA4C8066EF4A1C206B2BE5A36302F8926F7FDC6087AF5D20B417D0CF103EA6
        Installed at .tools/nlohmann_json/include/nlohmann/json.hpp

    godot-cpp 10.x is versioned independently of Godot and targets Godot 4.3+
    through the GODOTCPP_API_VERSION option. 4.7 is the pinned target for this
    project; see ADR-0012. Do not silently fall back to Godot 4.8 beta or an
    older engine: a missing or mismatched download is an error.

.PARAMETER Force
    Re-download and re-install even when the local copy already verifies.

.PARAMETER Fast
    Fetch the pinned set required by the fast verification path. This is the
    default set today (native core plus Godot and godot-cpp); the switch exists
    so the verification contract is explicit and future optional dependencies
    can be kept out of the fast path.

.PARAMETER NativeOnly
    Skip Godot and godot-cpp. Used by the T01 verification path so native-only
    work keeps working on a machine without the engine download.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts/bootstrap.ps1 -Fast

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts/bootstrap.ps1 -NativeOnly
#>
[CmdletBinding()]
param(
    [switch]$Force,
    [switch]$Fast,
    [switch]$NativeOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($Fast -and $NativeOnly) {
    throw "choose either -Fast or -NativeOnly, not both"
}

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

$archiveDeps = @(
    [pscustomobject]@{
        Name      = 'godot'
        Version   = '4.7.2'
        Url       = 'https://github.com/godotengine/godot/releases/download/4.7.2-stable/Godot_v4.7.2-stable_win64.exe.zip'
        Sha256    = '731980F9608D61333E5BAF54A2EF17210ACC7A538446C0CB9969F002ACA1E953'
        File      = 'Godot_v4.7.2-stable_win64.exe.zip'
        Sentinel  = 'Godot_v4.7.2-stable_win64_console.exe'
        StripRoot = $false
    },
    [pscustomobject]@{
        Name      = 'godot-cpp'
        Version   = '10.0.0-stable'
        Url       = 'https://github.com/godotengine/godot-cpp/archive/refs/tags/10.0.0-stable.zip'
        Sha256    = 'C763A08B8EC18DAE300CD54BB8848FAF1A5FD11DA629449D16649A72A1884727'
        File      = 'godot-cpp-10.0.0-stable.zip'
        Sentinel  = 'CMakeLists.txt'
        StripRoot = $true
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

# Download with the same rule: .NET first, Invoke-WebRequest as the fallback. The final
# error names the exact URL and the manual fallback so a blocked download is actionable.
function Save-Download([string]$Url, [string]$Destination) {
    $reasons = @()

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
            $reasons += $_.Exception.Message
            if (Test-Path -LiteralPath $Destination) {
                Remove-Item -LiteralPath $Destination -Force -ErrorAction SilentlyContinue
            }
            Write-Host "  note     .NET download failed ($($_.Exception.Message)); retrying with Invoke-WebRequest"
        } finally {
            $client.Dispose()
        }
    }

    try {
        Invoke-WebRequest -Uri $Url -OutFile $Destination -UseBasicParsing
        return
    } catch {
        $reason = $_.Exception.Message
        if ($null -ne $_.Exception.Response -and $null -ne $_.Exception.Response.StatusCode) {
            $reason = "HTTP $([int]$_.Exception.Response.StatusCode) $reason"
        }
        $reasons += $reason
    }

    throw ("download failed: {0}`n  reason: {1}`n  fallback: download the file manually, place it in {2}, and re-run this script" -f $Url, ($reasons -join '; '), $Destination)
}

function Expand-Zip([string]$ZipPath, [string]$Destination) {
    if (Test-Path -LiteralPath $Destination) {
        Remove-Item -LiteralPath $Destination -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction Stop
        [System.IO.Compression.ZipFile]::ExtractToDirectory($ZipPath, $Destination)
    } catch {
        if (Test-Path -LiteralPath $Destination) {
            Remove-Item -LiteralPath $Destination -Recurse -Force
        }
        New-Item -ItemType Directory -Force -Path $Destination | Out-Null
        Expand-Archive -LiteralPath $ZipPath -DestinationPath $Destination -Force
    }
}

function Get-CachedArchive([pscustomobject]$Dep) {
    $cachePath = Join-Path $downloadsDir $Dep.File
    $cacheOk = (Test-Path -LiteralPath $cachePath) -and ((Get-Sha256 $cachePath) -eq $Dep.Sha256)
    if (-not $Force -and $cacheOk) {
        Write-Host "  cache    $($Dep.Name) $($Dep.Version) (verified download cache)"
        return $cachePath
    }

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
    return $cachePath
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

function Install-Archive([pscustomobject]$Dep) {
    $destDir = Join-Path $toolsDir $Dep.Name
    $markerPath = Join-Path $destDir '.pinned'
    $markerText = '{0} {1} sha256={2}' -f $Dep.Name, $Dep.Version, $Dep.Sha256
    $sentinelPath = Join-Path $destDir $Dep.Sentinel

    if (-not $Force -and (Test-Path -LiteralPath $sentinelPath) -and (Test-Path -LiteralPath $markerPath)) {
        $markerOk = ((Get-Content -LiteralPath $markerPath -Raw).Trim() -eq $markerText)
        if ($markerOk) {
            Write-Host "  ok       $($Dep.Name) $($Dep.Version) (verified, skipped)"
            return
        }
    }

    $cachePath = Get-CachedArchive -Dep $Dep
    $extractRoot = Join-Path $toolsDir ("{0}.extract" -f $Dep.Name)
    Write-Host "  extract  $($Dep.Name) $($Dep.Version)"
    Expand-Zip -ZipPath $cachePath -Destination $extractRoot

    $source = $extractRoot
    if ($Dep.StripRoot) {
        $children = @(Get-ChildItem -LiteralPath $extractRoot)
        if ($children.Count -eq 1 -and $children[0].PSIsContainer) {
            $source = $children[0].FullName
        }
    }

    if (Test-Path -LiteralPath $destDir) {
        Remove-Item -LiteralPath $destDir -Recurse -Force
    }
    Move-Item -LiteralPath $source -Destination $destDir
    if (Test-Path -LiteralPath $extractRoot) {
        Remove-Item -LiteralPath $extractRoot -Recurse -Force
    }

    if (-not (Test-Path -LiteralPath $sentinelPath)) {
        throw "archive $($Dep.Name) $($Dep.Version) did not contain $($Dep.Sentinel)"
    }
    Set-Content -LiteralPath $markerPath -Value $markerText -NoNewline
    Write-Host "  install  $($Dep.Name) $($Dep.Version) -> $destDir"
}

Write-Host "bootstrap: pinned dependencies in $toolsDir"
if ($Fast) {
    Write-Host "bootstrap: fast profile (all pinned dependencies)"
}
foreach ($dep in $deps) {
    Install-Dependency -Dep $dep
}

if ($NativeOnly) {
    Write-Host "bootstrap: Godot and godot-cpp skipped (-NativeOnly)"
    Write-Host "bootstrap: done (native only). Configure with: cmake -S . -B build"
    exit 0
}

foreach ($dep in $archiveDeps) {
    Install-Archive -Dep $dep
}

$godotExe = Join-Path $toolsDir 'godot/Godot_v4.7.2-stable_win64_console.exe'
$godotCppDir = Join-Path $toolsDir 'godot-cpp'
Write-Host "bootstrap: godot      $godotExe"
Write-Host "bootstrap: godot-cpp  $godotCppDir"
Write-Host "bootstrap: done. Configure with: cmake -S . -B build"
exit 0
