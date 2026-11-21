<#
.SYNOPSIS
    T06 gate: CI, formatting, static analysis and the licence manifest.

.DESCRIPTION
    Runs after the native build and the fast tests. Checks:

      1. The workflows, pinned tool configs, scripts and design records exist.
      2. ci.yml names the Windows/macOS/Ubuntu matrix and runs bootstrap,
         configure, build RelWithDebInfo, ctest -L fast and failure artifacts.
      3. nightly.yml owns the full suite, performance, lavapipe goldens,
         sanitizers and clang-tidy, and marks release-blocking jobs.
      4. Both workflow files parse as YAML (PyYAML when available).
      5. The licence manifest is well formed and the real tree passes.
      6. A deliberately unlisted dependency fails the licence check.
      7. A deny-listed licence fails the licence check.
      8. The diagnostics scan is advisory on the real tree and rejects an
         unknown AREA-NNN and a log call without a code in strict mode.
      9. CTest registers the licence and diagnostics checks under fast/task_T06.
     10. The format check passes when clang-format is available (CI requires it).

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

function New-TempDirectory([string]$Name) {
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ("avatar-t06-{0}-{1}" -f $Name, [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $path | Out-Null
    return $path
}

function Write-Fixture([string]$Base, [string]$Relative, [string]$Content) {
    $path = Join-Path $Base $Relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    [System.IO.File]::WriteAllText($path, $Content, (New-Object System.Text.UTF8Encoding($false)))
    return $path
}

# 1. Required implementation files.
$ci = Read-Required '.github/workflows/ci.yml'
$nightly = Read-Required '.github/workflows/nightly.yml'
$clangFormat = Read-Required '.clang-format'
$clangTidy = Read-Required '.clang-tidy'
$licenceScript = Read-Required 'scripts/check-licenses.mjs'
$diagnosticsScript = Read-Required 'scripts/check-diagnostics.mjs'
$formatPs1 = Read-Required 'scripts/format-check.ps1'
$formatSh = Read-Required 'scripts/format-check.sh'
$ciPs1 = Read-Required 'scripts/ci.ps1'
$ciSh = Read-Required 'scripts/ci.sh'
$smokePs1 = Read-Required 'scripts/smoke.ps1'
$smokeSh = Read-Required 'scripts/smoke.sh'
$adr = Read-Required 'docs/design/adr/0016-ci-quality-gates.md'
# The progress note is a gitignored local workspace file, so a clean checkout
# does not have it. Read it when present; never fail the gate because the
# planning workspace is absent (T08 clean-checkout audit).
$progressPath = Join-Path $root 'docs/progress/T06.md'
$progress = ''
if (Test-Path -LiteralPath $progressPath) {
    $progress = Get-Content -LiteralPath $progressPath -Raw
}

# 2. Fast CI matrix.
if ($ci -ne '') {
    foreach ($needle in @('windows-latest', 'macos-latest', 'ubuntu-24.04',
                          'scripts/bootstrap', 'cmake -S . -B build',
                          '--config RelWithDebInfo', '-L fast',
                          'upload-artifact', 'if: failure()',
                          'check-licenses.mjs', 'check-design-links.mjs', 'format-check')) {
        if ($ci -notmatch [regex]::Escape($needle)) {
            Add-Failure "ci.yml is missing '$needle'"
        }
    }
    if ($ci -notmatch 'permissions:\s*\r?\n\s+contents:\s*read') {
        Add-Failure 'ci.yml does not restrict permissions to contents: read'
    }
}

# 3. Nightly ownership and release-blocking markers.
if ($nightly -ne '') {
    foreach ($needle in @('schedule:', 'cron:', 'lavapipe', 'mesa-vulkan-drivers',
                          '-fsanitize=address', '-fsanitize=thread', 'clang-tidy',
                          'performance', 'release-blocking', 'ctest --test-dir build',
                          '--self-test')) {
        if ($nightly -notmatch [regex]::Escape($needle)) {
            Add-Failure "nightly.yml is missing '$needle'"
        }
    }
}

# 4. YAML validity. PyYAML is present on this machine; a runner without it gets
#    a warning rather than a false failure, because the structural checks above
#    still cover the contract.
$python = Get-Command python -ErrorAction SilentlyContinue
if ($null -eq $python) {
    Write-Host 'T06 gate: note - python not found, skipping the YAML parse check'
} else {
    $yamlScript = New-TempDirectory 'yaml'
    $yamlPath = Write-Fixture $yamlScript 'validate.py' @'
import sys
try:
    import yaml
except Exception:
    sys.exit(3)
ok = True
for path in sys.argv[1:]:
    try:
        with open(path, encoding="utf-8") as handle:
            yaml.safe_load(handle)
    except Exception as error:
        print(f"{path}: {error}")
        ok = False
sys.exit(0 if ok else 1)
'@
    $yamlResult = Invoke-Capture $python.Source @(
        $yamlPath,
        (Join-Path $root '.github/workflows/ci.yml'),
        (Join-Path $root '.github/workflows/nightly.yml')
    )
    if ($yamlResult.Code -eq 1) {
        Add-Failure "workflow YAML is invalid: $($yamlResult.Output)"
    } elseif ($yamlResult.Code -eq 3) {
        Write-Host 'T06 gate: note - PyYAML not installed, skipping the YAML parse check'
    } elseif ($yamlResult.Code -ne 0) {
        Add-Failure "YAML validation could not run (exit $($yamlResult.Code)): $($yamlResult.Output)"
    }
    Remove-Item -LiteralPath $yamlScript -Recurse -Force -ErrorAction SilentlyContinue
}

# 5. Pinned tool configuration.
if ($clangFormat -ne '') {
    foreach ($needle in @('BasedOnStyle: LLVM', 'IndentWidth: 4', 'ColumnLimit: 100',
                          'Standard: c++20')) {
        if ($clangFormat -notmatch [regex]::Escape($needle)) {
            Add-Failure ".clang-format is missing '$needle'"
        }
    }
}
if ($clangTidy -ne '') {
    foreach ($needle in @('Checks:', 'WarningsAsErrors:', 'HeaderFilterRegex:')) {
        if ($clangTidy -notmatch [regex]::Escape($needle)) {
            Add-Failure ".clang-tidy is missing '$needle'"
        }
    }
}

# 6. Licence manifest shape.
$manifestPath = Join-Path $root 'native/licenses.json'
$manifest = $null
if (-not (Test-Path -LiteralPath $manifestPath)) {
    Add-Failure 'missing licence manifest: native/licenses.json'
} else {
    try {
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    } catch {
        Add-Failure "native/licenses.json is not valid JSON: $($_.Exception.Message)"
    }
}
if ($null -ne $manifest) {
    if ($manifest.schema -ne 'avatar.licenses.v1') {
        Add-Failure 'licences manifest schema is not avatar.licenses.v1'
    }
    if (-not $manifest.policy -or -not $manifest.policy.deny_spdx) {
        Add-Failure 'licences manifest has no deny list'
    }
    $ids = @($manifest.components | ForEach-Object { $_.id })
    foreach ($required in @('nlohmann_json', 'godot', 'godot-cpp', 'eigen', 'mediapipe',
                            'onnxruntime', 'spout2')) {
        if ($ids -notcontains $required) {
            Add-Failure "licences manifest is missing component '$required'"
        }
    }
    foreach ($component in $manifest.components) {
        if (-not $component.license -or -not $component.version -or -not $component.url) {
            Add-Failure "licences manifest component '$($component.id)' is incomplete"
        }
        if ($null -eq $component.modified) {
            Add-Failure "licences manifest component '$($component.id)' has no modified flag"
        }
    }
}

# 7. The real tree passes the licence gate.
$node = Get-Command node -ErrorAction SilentlyContinue
if ($null -eq $node) {
    Add-Failure 'node is required for the licence and diagnostics gates'
} else {
    $real = Invoke-Capture $node.Source @(
        (Join-Path $root 'scripts/check-licenses.mjs'),
        '--root', $root
    )
    if ($real.Code -ne 0) {
        Add-Failure "licence check failed on the real tree: $($real.Output)"
    }
    $selfTest = Invoke-Capture $node.Source @(
        (Join-Path $root 'scripts/check-licenses.mjs'),
        '--root', $root,
        '--self-test'
    )
    if ($selfTest.Code -ne 0) {
        Add-Failure "licence check self-test failed: $($selfTest.Output)"
    }

    # 8. A deliberately unlisted test dependency fails.
    $unlisted = New-TempDirectory 'unlisted'
    $null = Write-Fixture $unlisted 'tests/unit/bad_dependency.cpp' @'
#include <mysterylib/secret.h>
int main() { return 0; }
'@
    $unlistedResult = Invoke-Capture $node.Source @(
        (Join-Path $root 'scripts/check-licenses.mjs'),
        '--manifest', $manifestPath,
        '--scan', (Join-Path $unlisted 'tests')
    )
    if ($unlistedResult.Code -eq 0) {
        Add-Failure 'licence check did not fail a deliberately unlisted dependency'
    }
    if ($unlistedResult.Output -notmatch 'mysterylib') {
        Add-Failure 'licence check failure does not name the unlisted include'
    }
    Remove-Item -LiteralPath $unlisted -Recurse -Force -ErrorAction SilentlyContinue

    # 9. A deny-listed licence fails.
    if ($null -ne $manifest) {
        $denied = New-TempDirectory 'denied'
        $empty = New-TempDirectory 'empty'
        $deniedManifest = Join-Path $denied 'licenses.json'
        $badComponent = [pscustomobject]@{
            id = 'gpl-test-component'
            name = 'GPL Test Component'
            version = '1.0.0'
            license = 'GPL-3.0-only'
            url = 'https://example.invalid/gpl'
            modified = $false
            shipped = $false
            status = 'planned'
            include_prefixes = @('gpl-test/')
        }
        $deniedObject = [pscustomobject]@{
            schema = $manifest.schema
            updated = $manifest.updated
            policy = $manifest.policy
            components = @($manifest.components) + @($badComponent)
        }
        $deniedObject | ConvertTo-Json -Depth 20 |
            Set-Content -LiteralPath $deniedManifest -Encoding UTF8
        $deniedResult = Invoke-Capture $node.Source @(
            (Join-Path $root 'scripts/check-licenses.mjs'),
            '--manifest', $deniedManifest,
            '--scan', $empty
        )
        if ($deniedResult.Code -eq 0) {
            Add-Failure 'licence check did not fail a deny-listed GPL component'
        }
        if ($deniedResult.Output -notmatch 'GPL-3\.0-only') {
            Add-Failure 'licence check failure does not name the denied licence'
        }
        Remove-Item -LiteralPath $denied -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $empty -Recurse -Force -ErrorAction SilentlyContinue
    }

    # 10. Diagnostics: advisory on the real tree, strict on a bad fixture.
    $diagnostics = Invoke-Capture $node.Source @(
        (Join-Path $root 'scripts/check-diagnostics.mjs'),
        '--root', $root
    )
    if ($diagnostics.Code -ne 0) {
        Add-Failure "diagnostics scan failed on the real tree: $($diagnostics.Output)"
    }
    $diagnosticsSelfTest = Invoke-Capture $node.Source @(
        (Join-Path $root 'scripts/check-diagnostics.mjs'),
        '--root', $root,
        '--self-test'
    )
    if ($diagnosticsSelfTest.Code -ne 0) {
        Add-Failure "diagnostics scan self-test failed: $($diagnosticsSelfTest.Output)"
    }
    $badDiagnostics = New-TempDirectory 'diagnostics'
    $null = Write-Fixture $badDiagnostics 'native/src/bad.cpp' @'
#include <cstdio>
void f() { std::printf("TRK-999 and no code\n"); }
'@
    $badResult = Invoke-Capture $node.Source @(
        (Join-Path $root 'scripts/check-diagnostics.mjs'),
        '--root', $badDiagnostics,
        '--header', (Join-Path $root 'native/core/include/avatar/core/diagnostics.h'),
        '--strict'
    )
    if ($badResult.Code -eq 0) {
        Add-Failure 'diagnostics scan did not fail an unknown code in strict mode'
    }
    if ($badResult.Output -notmatch 'TRK-999') {
        Add-Failure 'diagnostics scan failure does not name the unknown code'
    }
    Remove-Item -LiteralPath $badDiagnostics -Recurse -Force -ErrorAction SilentlyContinue
}

# 11. CTest registration and CI script parity.
$ctestBase = @('--test-dir', (Join-Path $root $BuildDir), '-C', $Config, '-N')
$taskList = Invoke-Capture 'ctest' ($ctestBase + @('-L', 'task_T06'))
if ($taskList.Code -ne 0) {
    Add-Failure 'ctest -L task_T06 failed (build first)'
} else {
    foreach ($name in @('license_manifest_check', 'diagnostics_source_scan')) {
        if ($taskList.Output -notmatch [regex]::Escape($name)) {
            Add-Failure "CTest does not list $name under label task_T06"
        }
    }
}
$fastList = Invoke-Capture 'ctest' ($ctestBase + @('-L', 'fast'))
if ($fastList.Code -ne 0 -or $fastList.Output -notmatch 'license_manifest_check') {
    Add-Failure 'CTest does not list license_manifest_check under label fast'
}

foreach ($script in @(@{ Path = 'scripts/ci.ps1'; Text = $ciPs1 },
                      @{ Path = 'scripts/ci.sh'; Text = $ciSh })) {
    if ($script.Text -eq '') { continue }
    foreach ($needle in @('check-licenses.mjs', 'check-diagnostics.mjs', 'check-design-links.mjs',
                          'format-check', 'ctest', 'bootstrap')) {
        if ($script.Text -notmatch [regex]::Escape($needle)) {
            Add-Failure "$($script.Path) is missing '$needle'"
        }
    }
}

# 12. Design and progress records.
if ($adr -eq '') {
    Add-Failure 'missing ADR: docs/design/adr/0016-ci-quality-gates.md'
}
if ($progress -ne '') {
    foreach ($needle in @('branch protection', 'release-blocking', 'lavapipe',
                          'clang-format', 'licenses.json')) {
        if ($progress -notmatch [regex]::Escape($needle)) {
            Add-Failure "docs/progress/T06.md is missing '$needle'"
        }
    }
} else {
    Write-Host 'T06 gate: note - docs/progress/T06.md not present (gitignored workspace note); skipped'
}
$quality = Read-Required 'docs/design/testing-and-quality.md'
if ($quality -ne '') {
    foreach ($needle in @('ci.yml', 'nightly.yml', 'check-licenses.mjs',
                          'format-check', 'release-blocking')) {
        if ($quality -notmatch [regex]::Escape($needle)) {
            Add-Failure "docs/design/testing-and-quality.md is missing '$needle'"
        }
    }
}
$security = Read-Required 'docs/design/security-privacy.md'
if ($security -ne '') {
    foreach ($needle in @('native/licenses.json', 'check-licenses.mjs', 'deny list')) {
        if ($security -notmatch [regex]::Escape($needle)) {
            Add-Failure "docs/design/security-privacy.md is missing '$needle'"
        }
    }
}
$diagnosticsDoc = Read-Required 'docs/design/diagnostics.md'
if ($diagnosticsDoc -ne '') {
    foreach ($needle in @('check-diagnostics.mjs', 'advisory', 'T41')) {
        if ($diagnosticsDoc -notmatch [regex]::Escape($needle)) {
            Add-Failure "docs/design/diagnostics.md is missing '$needle'"
        }
    }
}

# 13. Formatting is enforced when the tool is available. CI always installs it.
$clangFormatExe = $env:CLANG_FORMAT
if (-not $clangFormatExe) {
    $command = Get-Command clang-format -ErrorAction SilentlyContinue
    if ($null -ne $command) { $clangFormatExe = $command.Source }
}
if (-not $clangFormatExe) {
    Write-Host 'T06 gate: note - clang-format not found, skipping the local drift check'
} else {
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $formatOutput = & (Join-Path $root 'scripts/format-check.ps1') -Require 2>&1
        $formatCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($formatCode -ne 0) {
        Add-Failure ("format-check failed: " +
            ((@($formatOutput) | ForEach-Object { [string]$_ }) -join "`n"))
    }
}

if ($failures.Count -gt 0) {
    Write-Host 'T06 gate: FAIL' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

$mode = if ($env:AVATAR_VERIFY_MODE) { $env:AVATAR_VERIFY_MODE } else { 'gate-only' }
Write-Host ("T06 gate: PASS ($mode; CI matrix, format, licence manifest, diagnostics scan, " +
    'local ci parity)')
exit 0
