<#
.SYNOPSIS
    T07 gate: implementation map, MVP sequencing ADR, and design status.

.DESCRIPTION
    Runs after the native build and the fast tests. Checks:

      1. The design map, the MVP ADR and the link checker exist.
      2. node scripts/check-design-links.mjs passes: relative links and anchors
         resolve, ADRs are indexed with a matching status, no design document
         names a missing task id, and the map covers every design document.
      3. The implementation map names every top-level design document.
      4. The map's "does not exist yet" table lists the subsystems that have no
         directory at this point (capture backends, tracker backend, rig,
         output, editor, virtual cameras).
      5. ADR-0017 is indexed as Proposed and says so itself; it records the
         Windows-first MVP, the 1.0 parity requirement and a re-entry condition.
      6. docs/design/README.md points at the implementation map instead of
         claiming the whole design is Planned.
      7. docs/design/roadmap.md has the MVP subsection, references ADR-0017 and
         the task phases, and keeps the M0-M5 exit criteria intact.

.PARAMETER BuildDir
    Build directory used by scripts/verify.ps1. Unused; kept for gate parity.

.PARAMETER Config
    Build configuration used by scripts/verify.ps1. Unused; kept for gate parity.
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

# 1. Required files.
$checker = Read-Required 'scripts/check-design-links.mjs'
$map = Read-Required 'docs/design/implementation-map.md'
$adr = Read-Required 'docs/design/adr/0017-windows-first-mvp-sequencing.md'
$adrIndex = Read-Required 'docs/design/adr/README.md'
$designReadme = Read-Required 'docs/design/README.md'
$designRoadmap = Read-Required 'docs/design/roadmap.md'

# 2. The link checker passes on the real tree.
$node = Get-Command node -ErrorAction SilentlyContinue
if ($null -eq $node) {
    Add-Failure 'node is not on PATH; the design-link check cannot run'
} elseif ($checker -ne '') {
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $checkOutput = & $node.Source (Join-Path $root 'scripts/check-design-links.mjs') 2>&1
        $checkCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($checkCode -ne 0) {
        Add-Failure ("check-design-links failed: " +
            ((@($checkOutput) | ForEach-Object { [string]$_ }) -join "`n"))
    } else {
        Write-Host ("T07 gate: " + ((@($checkOutput) | ForEach-Object { [string]$_ }) -join ' '))
    }
}

# 3. Every top-level design document is named by the map.
$designDocs = @(
    'README.md',
    'product-overview.md',
    'architecture.md',
    'face-compositing.md',
    'tracking.md',
    'camera-capture.md',
    'output-and-virtual-camera.md',
    'avatar-package-format.md',
    'user-data.md',
    'editor-ux.md',
    'security-privacy.md',
    'testing-and-quality.md',
    'diagnostics.md',
    'roadmap.md'
)
foreach ($doc in $designDocs) {
    if ($map -notmatch [regex]::Escape("]($doc)")) {
        Add-Failure "implementation-map.md does not link $doc"
    }
}
foreach ($doc in $designDocs) {
    $path = Join-Path $root "docs/design/$doc"
    if (-not (Test-Path -LiteralPath $path)) {
        Add-Failure "missing design document: docs/design/$doc"
    }
}

# 4. The map is explicit about what does not exist yet.
$missingNeedles = @(
    'native/tracking/mediapipe/',
    'native/rig/',
    'native/output/',
    'platform/windows/vcam/',
    'app/ui/'
)
$missingSection = $map
if ($map -match '(?s)## What does not exist yet(.*?)## README') {
    $missingSection = $Matches[1]
} else {
    Add-Failure 'implementation-map.md has no "## What does not exist yet" section'
}
foreach ($needle in $missingNeedles) {
    if ($missingSection -notmatch [regex]::Escape($needle)) {
        Add-Failure "the missing-component table does not name $needle"
    }
}

# 5. ADR-0017 is Proposed and records the decision, the 1.0 requirement and the re-entry condition.
if ($adr -notmatch '(?m)^-\s*\*\*Status:\*\*\s*Proposed\s*$') {
    Add-Failure 'ADR-0017 does not declare "- **Status:** Proposed"'
}
if ($adrIndex -notmatch '(?m)^\|\s*\[0017\]\(0017-windows-first-mvp-sequencing\.md\)\s*\|\s*[^|]+\|\s*Proposed\s*\|') {
    Add-Failure 'ADR-0017 is not indexed as Proposed in docs/design/adr/README.md'
}
foreach ($needle in @('Windows 11', 'OBS', 'Spout2', 'window capture', 'Re-entry condition', '1.0 requirements')) {
    if ($adr -notmatch [regex]::Escape($needle)) {
        Add-Failure "ADR-0017 does not mention '$needle'"
    }
}
foreach ($adrNumber in @('0001', '0005')) {
    if ($adr -notmatch "ADR-$adrNumber") {
        Add-Failure "ADR-0017 does not state that ADR-$adrNumber stays in force"
    }
}

# 6. The design README points at the map.
if ($designReadme -notmatch 'implementation-map\.md') {
    Add-Failure 'docs/design/README.md does not point at implementation-map.md'
}
if ($designReadme -match 'avatar pipeline, real capture, real tracking, the editor and output subsystems are still') {
    Add-Failure 'docs/design/README.md still carries the pre-map current-reality sentence'
}

# 7. The roadmap has the MVP subsection and keeps the milestone criteria.
if ($designRoadmap -notmatch '(?m)^## MVP definition and sequencing\s*$') {
    Add-Failure 'docs/design/roadmap.md has no "## MVP definition and sequencing" subsection'
}
if ($designRoadmap -notmatch 'adr/0017-windows-first-mvp-sequencing\.md') {
    Add-Failure 'the roadmap MVP subsection does not reference ADR-0017'
}
if ($designRoadmap -notmatch 'T46') {
    Add-Failure 'the roadmap MVP subsection does not reference the task phases'
}
foreach ($milestone in @('## M0', '## M1', '## M2', '## M3', '## M4', '## M5')) {
    if ($designRoadmap -notmatch [regex]::Escape($milestone)) {
        Add-Failure "docs/design/roadmap.md lost '$milestone'"
    }
}
$exitCriteria = ([regex]::Matches($designRoadmap, '(?m)^\*\*Exit criteria\*\*\s*$')).Count
if ($exitCriteria -ne 6) {
    Add-Failure "expected 6 milestone exit-criteria blocks in roadmap.md, found $exitCriteria"
}

if ($failures.Count -gt 0) {
    Write-Host 'T07 gate: FAIL' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

$mode = if ($env:AVATAR_VERIFY_MODE) { $env:AVATAR_VERIFY_MODE } else { 'gate-only' }
Write-Host ("T07 gate: PASS ($mode; implementation map, design links, ADR-0017, " +
    'README status, roadmap MVP sequencing)')
exit 0
