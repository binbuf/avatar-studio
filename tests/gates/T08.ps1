<#
.SYNOPSIS
    T08 gate: the foundation gap audit.

.DESCRIPTION
    Runs after the native build and the fast tests. Checks the durable parts of the
    audit that can be repeated from a clean checkout:

      1. The design-link checker passes.
      2. The implementation map is current as of T08, makes no false build claim,
         records ADR-0018, and acknowledges the tracked development model.
      3. Every foundation path the map calls Implemented exists.
      4. Every path the map calls missing is still absent.
      5. ADR-0018 exists, is indexed and is Accepted in both places.
      6. .gitignore covers the generated tree and the local planning workspace.
      7. No build output, logs, .tools, progress notes or planning files are tracked.
      8. testing-and-quality.md contains the clean-checkout audit evidence.
      9. The T06 gate no longer depends on the gitignored progress note.
     10. docs/progress/T08.md, when present, contains a gap list and follow-ups
         (it is gitignored, so its absence is not a failure).

.PARAMETER BuildDir
    Build directory used by scripts/verify.ps1. Unused directly; accepted for the
    gate convention.

.PARAMETER Config
    Build configuration used by scripts/verify.ps1. Unused directly.
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

function Add-Failure([string]$Message) {
    $script:failures.Add($Message)
}

function Read-Required([string]$RelativePath) {
    $path = Join-Path $root $RelativePath
    if (-not (Test-Path -LiteralPath $path)) {
        Add-Failure "missing file: $RelativePath"
        return ''
    }
    return (Get-Content -LiteralPath $path -Raw)
}

function Invoke-Capture([string]$File, [string[]]$Arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $File @Arguments 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    $text = (@($output) | ForEach-Object { [string]$_ }) -join "`n"
    return [pscustomobject]@{ Code = $code; Output = $text }
}

$map = Read-Required 'docs/design/implementation-map.md'
$adr = Read-Required 'docs/design/adr/0018-clean-checkout-verification.md'
$adrIndex = Read-Required 'docs/design/adr/README.md'
$quality = Read-Required 'docs/design/testing-and-quality.md'
$t06Gate = Read-Required 'tests/gates/T06.ps1'
$gitignore = Read-Required '.gitignore'

# 1. Design links, anchors, ADR indexing and task ids.
$node = Get-Command node -ErrorAction SilentlyContinue
if ($null -eq $node) {
    Add-Failure 'node is required for the design-link check'
} else {
    $links = Invoke-Capture $node.Source @(
        (Join-Path $root 'scripts/check-design-links.mjs'),
        '--quiet'
    )
    if ($links.Code -ne 0) {
        Add-Failure "check-design-links failed: $($links.Output)"
    }
}

# 2. The map is current and does not repeat the defects T08 corrected.
if ($map -ne '') {
    if ($map -notmatch 'current as of T08') {
        Add-Failure 'implementation-map.md is not marked "current as of T08"'
    }
    if ($map -match 'CMake,\s*presets') {
        Add-Failure 'implementation-map.md still claims a CMake presets feature that does not exist'
    }
    if ($map -match 'no bundled asset exists') {
        Add-Failure 'implementation-map.md still claims no bundled asset exists'
    }
    if ($map -notmatch [regex]::Escape('assets/models/avocado_half.glb')) {
        Add-Failure 'implementation-map.md does not record the tracked development model'
    }
    if ($map -notmatch [regex]::Escape('ADR-0018')) {
        Add-Failure 'implementation-map.md has no ADR-0018 row'
    }
    if ($map -notmatch '(?m)^\|\s*\[ADR-0018\]\(adr/0018-clean-checkout-verification\.md\)[^\r\n]*\|\s*Accepted\s*\|') {
        Add-Failure 'implementation-map.md does not list ADR-0018 as Accepted'
    }
}

# 3. Foundation paths the map calls Implemented must exist.
foreach ($relative in @(
        'CMakeLists.txt',
        'tests/unit',
        'scripts/verify.ps1',
        'native/core',
        'app/project.godot',
        'native/gdextension',
        'app/tests/smoke.gd',
        'native/capture',
        'native/tracking',
        'tests/tools/golden_compare',
        'tests/fixtures/replay',
        'tests/performance',
        '.github/workflows',
        'native/licenses.json',
        'docs/design',
        'scripts/check-design-links.mjs',
        'scripts/check-licenses.mjs',
        'scripts/check-diagnostics.mjs')) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $relative))) {
        Add-Failure "map claims Implemented but the path is missing: $relative"
    }
}

# 4. Paths the map calls missing must stay absent until their owning task lands.
foreach ($relative in @(
        'native/rig',
        'native/avatar',
        'native/output',
        'native/capture/windows',
        'native/capture/macos',
        'native/capture/linux',
        'native/tracking/mediapipe',
        'platform/windows/vcam',
        'platform/macos/vcam',
        'platform/linux/vcam',
        'app/shaders',
        'app/ui',
        'tests/golden',
        'tests/integration',
        'examples',
        'installer',
        'NOTICE')) {
    if (Test-Path -LiteralPath (Join-Path $root $relative)) {
        Add-Failure "map says this is missing but it exists (flip the row): $relative"
    }
}

# 5. ADR-0018 is real, indexed and consistent.
if ($adr -eq '') {
    Add-Failure 'missing ADR: docs/design/adr/0018-clean-checkout-verification.md'
} elseif ($adr -notmatch '(?m)^- \*\*Status:\*\* Accepted') {
    Add-Failure 'ADR-0018 is not Accepted in its own record'
}
if ($adrIndex -ne '' -and
    $adrIndex -notmatch '(?m)^\|\s*\[0018\]\(0018-clean-checkout-verification\.md\)\s*\|[^\r\n]*\|\s*Accepted\s*\|') {
    Add-Failure 'ADR-0018 is not indexed as Accepted in docs/design/adr/README.md'
}

# 6. Ignore coverage for generated and local-workspace trees.
if ($gitignore -ne '') {
    foreach ($pattern in @('/build/', '/build-*/', '/.tools/', '/app/.godot/', '/app/bin/*.dll',
                          '/tests/performance/results/', 'docs/progress/', 'docs/tasks/',
                          'docs/logs/', 'docs/ROADMAP.md', 'docs/PROGRESS.md', 'docs/INDEX.md')) {
        if ($gitignore -notmatch [regex]::Escape($pattern)) {
            Add-Failure ".gitignore does not cover '$pattern'"
        }
    }
}

# 7. No tracked generated output or planning workspace.
$git = Get-Command git -ErrorAction SilentlyContinue
if ($null -eq $git) {
    Write-Host 'T08 gate: note - git not found, skipping the tracked-file audit'
} else {
    $tracked = Invoke-Capture $git.Source @('-C', $root, 'ls-files')
    if ($tracked.Code -ne 0) {
        Add-Failure "git ls-files failed: $($tracked.Output)"
    } else {
        $forbidden = @(
            '^build/',
            '^\.tools/',
            '^out/',
            '^artifacts/',
            '^app/\.godot/',
            '^tests/performance/results/',
            '^tests/fixtures/generated/',
            '^docs/progress/',
            '^docs/logs/',
            '^docs/tasks/',
            '^docs/(ROADMAP|PROGRESS|INDEX)\.md$',
            '\.(dll|pdb|log|obj|ilk|exp|tmp)$'
        )
        foreach ($line in ($tracked.Output -split "`n")) {
            $file = $line.Trim()
            if ($file -eq '') { continue }
            foreach ($pattern in $forbidden) {
                if ($file -match $pattern) {
                    Add-Failure "tracked generated or workspace file: $file"
                }
            }
        }
    }
}

# 8. The durable clean-checkout evidence lives in the design set, not only in progress/.
if ($quality -ne '' -and $quality -notmatch '(?m)^## Clean-checkout audit \(T08\)') {
    Add-Failure 'testing-and-quality.md has no "## Clean-checkout audit (T08)" section'
}

# 9. The T06 gate must not require the gitignored progress note.
if ($t06Gate -ne '') {
    if ($t06Gate -match "Read-Required 'docs/progress/T06\.md'") {
        Add-Failure 'T06 gate still requires the gitignored docs/progress/T06.md'
    }
    if ($t06Gate -notmatch 'gitignored workspace note') {
        Add-Failure 'T06 gate has no clean-checkout skip for the progress note'
    }
}

# 10. The task note is local; check it only when the planning workspace exists.
$progressPath = Join-Path $root 'docs/progress/T08.md'
if (Test-Path -LiteralPath $progressPath) {
    $progress = Get-Content -LiteralPath $progressPath -Raw
    foreach ($needle in @('Gap list', 'Follow-ups', 'Hand-off')) {
        if ($progress -notmatch [regex]::Escape($needle)) {
            Add-Failure "docs/progress/T08.md is missing '$needle'"
        }
    }
} else {
    Write-Host 'T08 gate: note - docs/progress/T08.md not present (gitignored workspace note); skipped'
}

if ($failures.Count -gt 0) {
    Write-Host 'T08 gate: FAIL' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

$mode = if ($env:AVATAR_VERIFY_MODE) { $env:AVATAR_VERIFY_MODE } else { 'gate-only' }
Write-Host ("T08 gate: PASS ($mode; clean-checkout audit, implementation map, " +
    'ignore coverage, tracked-file audit)')
exit 0
