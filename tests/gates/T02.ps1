<#
.SYNOPSIS
    T02 gate: the pinned Godot shell and GDExtension are actually in place.

.DESCRIPTION
    Runs after the native build and the headless smoke test. Checks invariants
    that the smoke test cannot see from inside Godot:

      1. Bootstrap pins Godot 4.7.2 and godot-cpp 10.0.0-stable by exact URL
         and SHA256, with no 4.8/older fallback.
      2. The pinned engine and godot-cpp checkout are installed in .tools/.
      3. project.godot selects Forward+, 60 fps, and the main scene.
      4. The main scene owns a fixed 1920x1080 transparent SubViewport with
         render_target_update_mode ALWAYS.
      5. The .gdextension descriptor names the entry symbol and both debug and
         release library entries.
      6. The built GDExtension artifact exists in app/bin/.

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

function Read-Required([string]$RelativePath) {
    $path = Join-Path $root $RelativePath
    if (-not (Test-Path -LiteralPath $path)) {
        $failures.Add("missing file: $RelativePath")
        return ''
    }
    return (Get-Content -LiteralPath $path -Raw)
}

# 1. Pins live in both bootstrap scripts.
foreach ($script in @('scripts/bootstrap.ps1', 'scripts/bootstrap.sh')) {
    $text = Read-Required $script
    if ($text -eq '') { continue }
    foreach ($needle in @(
            'Godot_v4.7.2-stable',
            '731980F9608D61333E5BAF54A2EF17210ACC7A538446C0CB9969F002ACA1E953',
            'godot-cpp-10.0.0-stable',
            'C763A08B8EC18DAE300CD54BB8848FAF1A5FD11DA629449D16649A72A1884727'
        )) {
        if ($text -notmatch [regex]::Escape($needle)) {
            $failures.Add("$script does not pin '$needle'")
        }
    }
    if ($text -match 'Godot_v4\.8|4\.8-stable|Godot_v4\.6|4\.6-stable') {
        $failures.Add("$script mentions a non-pinned Godot version")
    }
}

# 2. Pinned engine and godot-cpp are installed.
$godotExe = Join-Path $root '.tools/godot/Godot_v4.7.2-stable_win64_console.exe'
$godotMarker = Join-Path $root '.tools/godot/.pinned'
if (-not (Test-Path -LiteralPath $godotExe)) {
    $failures.Add("pinned Godot is missing: $godotExe (run scripts/bootstrap.ps1 -Fast)")
} elseif (-not (Test-Path -LiteralPath $godotMarker)) {
    $failures.Add("pinned Godot marker is missing: $godotMarker")
} else {
    $marker = Get-Content -LiteralPath $godotMarker -Raw
    if ($marker -notmatch 'godot 4\.7\.2 sha256=731980F9608D61333E5BAF54A2EF17210ACC7A538446C0CB9969F002ACA1E953') {
        $failures.Add("pinned Godot marker is not the expected 4.7.2 record: $marker")
    }
}

$godotCppCmake = Join-Path $root '.tools/godot-cpp/CMakeLists.txt'
$godotCppMarker = Join-Path $root '.tools/godot-cpp/.pinned'
if (-not (Test-Path -LiteralPath $godotCppCmake)) {
    $failures.Add("pinned godot-cpp is missing: $godotCppCmake (run scripts/bootstrap.ps1 -Fast)")
} elseif (-not (Test-Path -LiteralPath $godotCppMarker)) {
    $failures.Add("pinned godot-cpp marker is missing: $godotCppMarker")
} else {
    $marker = Get-Content -LiteralPath $godotCppMarker -Raw
    if ($marker -notmatch 'godot-cpp 10\.0\.0-stable sha256=C763A08B8EC18DAE300CD54BB8848FAF1A5FD11DA629449D16649A72A1884727') {
        $failures.Add("pinned godot-cpp marker is not the expected 10.0.0-stable record: $marker")
    }
}

# 3. Project settings.
$project = Read-Required 'app/project.godot'
if ($project -ne '') {
    foreach ($needle in @(
            'rendering_method="forward_plus"',
            'run/max_fps=60',
            'run/main_scene="res://scenes/main.tscn"',
            'config/features=PackedStringArray("4.7", "Forward Plus")'
        )) {
        if ($project -notmatch [regex]::Escape($needle)) {
            $failures.Add("app/project.godot is missing '$needle'")
        }
    }
}

# 4. Main scene viewport contract.
$scene = Read-Required 'app/scenes/main.tscn'
if ($scene -ne '') {
    foreach ($needle in @(
            'type="SubViewport"',
            'size = Vector2i(1920, 1080)',
            'transparent_bg = true',
            'render_target_update_mode = 4'
        )) {
        if ($scene -notmatch [regex]::Escape($needle)) {
            $failures.Add("app/scenes/main.tscn is missing '$needle'")
        }
    }
}

# 5. GDExtension descriptor.
$descriptor = Read-Required 'app/bin/avatar_studio.gdextension'
if ($descriptor -ne '') {
    foreach ($needle in @(
            'entry_symbol = "avatar_studio_library_init"',
            'windows.debug.x86_64',
            'windows.release.x86_64'
        )) {
        if ($descriptor -notmatch [regex]::Escape($needle)) {
            $failures.Add("app/bin/avatar_studio.gdextension is missing '$needle'")
        }
    }
}

# 6. Built adapter.
$extension = Join-Path $root 'app/bin/avatar_studio.windows.x86_64.dll'
if (-not (Test-Path -LiteralPath $extension)) {
    $failures.Add("GDExtension artifact is missing: $extension (build must run with .tools/godot-cpp present)")
}

# 7. Smoke test exists and is runnable by verify.
$smoke = Read-Required 'app/tests/smoke.gd'
if ($smoke -ne '') {
    foreach ($needle in @('AvatarRuntime', 'SubViewport', 'UPDATE_ALWAYS', 'transparent_bg')) {
        if ($smoke -notmatch [regex]::Escape($needle)) {
            $failures.Add("app/tests/smoke.gd does not check '$needle'")
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host 'T02 gate: FAIL' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

Write-Host 'T02 gate: PASS (Godot 4.7.2 pinned, shell viewport, GDExtension descriptor and artifact present)'
exit 0
