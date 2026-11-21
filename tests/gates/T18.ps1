#requires -Version 5.1
<# T18: numerical FaceRig regression suite, local CPU budget, and dependency/docs checks.
   This gate never requires gitignored progress/task files. #>
[CmdletBinding()]
param([string]$BuildDir = 'build', [string]$Config = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $root
try {
    & ctest --test-dir $BuildDir -C $Config -R '^avatar_rig_(unit|perf)$' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'FaceRig numerical or CPU-budget test failed' }
    $tests = (& ctest --test-dir $BuildDir -C $Config -N -R '^avatar_rig_(unit|perf)$') -join "`n"
    if ($LASTEXITCODE -ne 0 -or $tests -notmatch 'Total Tests: 2') {
        throw 'both FaceRig numerical and benchmark tests must be registered'
    }
    $header = Get-Content native/rig/include/avatar/rig/face_rig.h -Raw
    if ($header -match '#include\s*[<"](?:Eigen|godot_cpp|windows)') {
        throw 'FaceRig public API leaked a vendor or platform type'
    }
    foreach ($path in @('docs/design/adr/0028-facerig-estimation-and-temporal-contract.md', 'NOTICE')) {
        if (-not (Test-Path -LiteralPath $path)) { throw "missing $path" }
    }
    & node scripts/check-licenses.mjs --self-test
    if ($LASTEXITCODE -ne 0) { throw 'licence check failed' }
    & node scripts/check-design-links.mjs
    if ($LASTEXITCODE -ne 0) { throw 'design-link check failed' }
    & powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format-check.ps1 -Require
    if ($LASTEXITCODE -ne 0) { throw 'format check failed' }
    Write-Host 'T18 gate: PASS (numerics, CPU <0.1 ms, API, licences, design, format)'
} finally {
    Pop-Location
}
