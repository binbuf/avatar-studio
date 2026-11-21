<# T11: real Windows tracker, isolated exports, checked model and executable contracts. #>
[CmdletBinding()]
param([string]$BuildDir = 'build', [string]$Config = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $root
try {
    foreach ($file in @(
        'native/tracking/mediapipe/engine.cpp',
        'native/tracking/mediapipe/runtime.cpp',
        'native/tracking/mediapipe/c_api.cpp',
        'native/tracking/mediapipe/tracker.def',
        'native/tracking/mediapipe/patches/0001-msvc-preprocessor.patch',
        'native/tracking/mediapipe/patches/glog-static.diff',
        'scripts/build-tracker.ps1', 'scripts/check-tracker-sdk.mjs',
        'tests/unit/test_real_tracker.cpp',
        'docs/design/adr/0021-real-mediapipe-windows-backend.md'
    )) {
        if (-not (Test-Path -LiteralPath $file)) { throw "T11 missing $file" }
    }
    & node scripts/check-tracker-sdk.mjs --self-test --sdk (Join-Path $BuildDir $Config)
    if ($LASTEXITCODE -ne 0) { throw 'T11 model/export integrity failed' }
    & node scripts/check-tracker-sdk.mjs --sdk app/bin
    if ($LASTEXITCODE -ne 0) { throw 'T11 app bundle model/export integrity failed' }
    & node scripts/check-licenses.mjs --self-test
    if ($LASTEXITCODE -ne 0) { throw 'T11 licence manifest failed' }
    $list = (& ctest --test-dir $BuildDir -C $Config -N -L task_T11) -join "`n"
    foreach ($test in @('avatar_real_tracker_contract', 'avatar_tracker_model_integrity', 'tracker_sdk_integrity', 'avatar_tracker_worker_contract')) {
        if (-not $list.Contains($test)) { throw "T11 CTest missing $test" }
    }
    & ctest --test-dir $BuildDir -C $Config -L 'task_T09|task_T11' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'T11 real/scripted contract tests failed' }
    $tracking = Get-Content docs/design/tracking.md -Raw
    if (-not $tracking.Contains('ADR-0021') -or -not $tracking.Contains('T11')) {
        throw 'T11 tracking design is not aligned'
    }
    $adr = Get-Content docs/design/adr/0003-mediapipe-as-tracking-backend.md -Raw
    if ($adr -notmatch '\*\*Status:\*\* Accepted') { throw 'T11 MediaPipe decision was not validated' }
    # Findings are local workspace files, deliberately absent in clean checkouts.
    foreach ($note in @('docs/progress/T11.md', 'docs/progress/T11-tracker-results.md')) {
        if (Test-Path -LiteralPath $note) {
            $text = Get-Content $note -Raw
            if (-not $text.Contains('Follow-ups')) { throw "T11 note lacks Follow-ups: $note" }
        }
    }
    Write-Host 'T11 gate: PASS (real tracker, five exports, model SHA256, T09/T11 contracts)'
} finally { Pop-Location }
