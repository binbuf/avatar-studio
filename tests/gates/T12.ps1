<#
.SYNOPSIS
    T12 gate: the named landmark map, pinned backend tables, asymmetric-marker test and
    profile correspondences exist and pass.

.DESCRIPTION
    Runs after the native build and the fast tests. Checks:

      1. The landmark map, canonical frame, profile correspondence and test files exist.
      2. Every documented name is present, both backend tables are pinned, unknown names are
         represented as unavailable rather than guessed, and duplicate indices are reported.
      3. The asymmetric-marker, canonical handedness, subset fallback and profile round-trip
         cases are present in the test file.
      4. CMake builds the new sources and registers the suite under task_T12.
      5. The task_T12 CTest label lists and passes avatar_tracking_unit.
      6. tracking.md, user-data.md, the implementation map, architecture and README record
         the realised map and ADR-0022; the design-link checker passes.
      7. docs/progress/T12.md, when present, has Follow-ups and Hand-off.

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

# 1. Required implementation, test and decision files.
$mapHeader = Read-Required 'native/tracking/include/avatar/tracking/landmark_map.h'
$frameHeader = Read-Required 'native/tracking/include/avatar/tracking/canonical_face_frame.h'
$profileHeader = Read-Required 'native/tracking/include/avatar/tracking/profile_correspondence.h'
$null = Read-Required 'native/tracking/src/landmark_map.cpp'
$null = Read-Required 'native/tracking/src/canonical_face_frame.cpp'
$profileImpl = Read-Required 'native/tracking/src/profile_correspondence.cpp'
$landmarkTests = Read-Required 'tests/unit/test_landmark_map.cpp'
$adr = Read-Required 'docs/design/adr/0022-named-landmark-map-and-asymmetric-marker.md'

# 2. The map contract and both pinned tables.
if ($mapHeader -ne '') {
    foreach ($name in @(
            'eye_outer_r', 'eye_inner_r', 'eye_outer_l', 'eye_inner_l',
            'iris_center_r', 'iris_center_l', 'nose_bridge', 'nose_tip',
            'nose_left', 'nose_right', 'mouth_corner_r', 'mouth_corner_l',
            'mouth_upper_center', 'mouth_lower_center', 'chin', 'jaw_left',
            'jaw_right', 'temple_l', 'temple_r', 'cheek_l', 'cheek_r',
            'forehead_center')) {
        if ($mapHeader -notmatch [regex]::Escape($name)) {
            Add-Failure "landmark_map.h is missing the '$name' binding"
        }
    }
    foreach ($needle in @(
            'kMediaPipeFaceLandmarkerTable', 'kOnnxFaceMeshV2Table',
            'kMediaPipeFaceLandmarkerPin', 'kOnnxFaceMeshV2Pin',
            'mediapipe-0.10.21', '337d58218b5b1cc597ca3c67360880b920f6ce7b',
            'kNoLandmarkIndex', 'resolve(std::string_view', 'duplicate_indices',
            'for_backend', 'kDefaultLandmarkCount')) {
        if ($mapHeader -notmatch [regex]::Escape($needle)) {
            Add-Failure "landmark_map.h is missing '$needle'"
        }
    }
}
if ($frameHeader -ne '') {
    foreach ($needle in @('CanonicalFaceFrame', 'eye_centres', 'check_handedness',
                          'axis_u', 'axis_v', 'aspect')) {
        if ($frameHeader -notmatch [regex]::Escape($needle)) {
            Add-Failure "canonical_face_frame.h is missing '$needle'"
        }
    }
}
if ($profileHeader -ne '') {
    foreach ($needle in @('kMinimumSamplingCorrespondences', 'ProfileLandmarks',
                          'build_profile_correspondences', 'missing_names',
                          'duplicate_indices', 'to_json')) {
        if ($profileHeader -notmatch [regex]::Escape($needle)) {
            Add-Failure "profile_correspondence.h is missing '$needle'"
        }
    }
    if ($profileHeader -notmatch 'kMinimumSamplingCorrespondences\s*=\s*8') {
        Add-Failure 'profile_correspondence.h does not require eight correspondences'
    }
}
if ($profileImpl -ne '' -and $profileImpl -notmatch 'rig_003') {
    Add-Failure 'profile_correspondence.cpp does not raise RIG-003'
}

# 3. The cases the task names must have a test each.
if ($landmarkTests -ne '') {
    foreach ($needle in @(
            'landmark_map_resolves_every_documented_name',
            'landmark_map_unknown_names_are_ignored',
            'landmark_map_duplicate_indices_are_reported',
            'asymmetric_marker_subject_right_maps_to_viewer_left',
            'asymmetric_marker_swapped_table_fails',
            'canonical_axes_point_to_subject_right_and_forehead',
            'profile_correspondences_require_eight_inliers',
            'profile_correspondences_fall_back_and_raise_rig_003',
            'profile_correspondences_report_duplicate_indices',
            'profile_round_trips_through_json_without_losing_names')) {
        if ($landmarkTests -notmatch [regex]::Escape($needle)) {
            Add-Failure "test_landmark_map.cpp is missing the '$needle' case"
        }
    }
}

# 4. CMake wiring.
$cmake = Read-Required 'CMakeLists.txt'
if ($cmake -ne '') {
    foreach ($needle in @('native/tracking/src/landmark_map.cpp',
                          'native/tracking/src/canonical_face_frame.cpp',
                          'native/tracking/src/profile_correspondence.cpp',
                          'tests/unit/test_landmark_map.cpp', 'task_T12')) {
        if ($cmake -notmatch [regex]::Escape($needle)) {
            Add-Failure "CMakeLists.txt is missing '$needle'"
        }
    }
}

# 5. CTest registration and execution.
$ctestBase = @('--test-dir', (Join-Path $root $BuildDir), '-C', $Config)
$taskList = Invoke-Capture 'ctest' ($ctestBase + @('-N', '-L', 'task_T12'))
if ($taskList.Code -ne 0 -or $taskList.Output -notmatch 'avatar_tracking_unit') {
    Add-Failure 'CTest does not list avatar_tracking_unit under label task_T12 (build first)'
}
$run = Invoke-Capture 'ctest' ($ctestBase + @('-L', 'task_T12', '--output-on-failure'))
if ($run.Code -ne 0) {
    Add-Failure "the task_T12 tests failed: $($run.Output)"
}

# 6. Design records and the implementation map.
$tracking = Read-Required 'docs/design/tracking.md'
if ($tracking -ne '') {
    foreach ($needle in @('T12', 'ADR-0022', 'kMediaPipeFaceLandmarkerTable',
                          'kOnnxFaceMeshV2Table', 'RIG-003', 'asymmetric-marker',
                          'at least 8 usable correspondences')) {
        if ($tracking -notmatch [regex]::Escape($needle)) {
            Add-Failure "tracking.md does not document '$needle'"
        }
    }
}
$userData = Read-Required 'docs/design/user-data.md'
if ($userData -ne '' -and ($userData -notmatch 'T12' -or $userData -notmatch 'RIG-003')) {
    Add-Failure 'user-data.md does not record the T12 profile subset and RIG-003 fallback'
}
$map = Read-Required 'docs/design/implementation-map.md'
if ($map -ne '') {
    if ($map -notmatch 'current as of T(\d{2,3})') {
        Add-Failure 'implementation-map.md has no "current as of TNN" marker'
    } elseif ([int]$Matches[1] -lt 12) {
        Add-Failure "implementation-map.md is stale (current as of T$($Matches[1]), expected T12 or later)"
    }
    if ($map -notmatch '(?m)^\|[^\r\n]*Landmark map[^\r\n]*\|\s*`Implemented`\s*\|') {
        Add-Failure 'implementation-map.md has no Implemented landmark-map row'
    }
    if ($map -notmatch '(?m)^\|\s*\[ADR-0022\]\(adr/0022-named-landmark-map-and-asymmetric-marker\.md\)[^\r\n]*\|\s*Accepted\s*\|') {
        Add-Failure 'implementation-map.md does not list ADR-0022 as Accepted'
    }
}
$adrIndex = Read-Required 'docs/design/adr/README.md'
if ($adrIndex -ne '' -and
    $adrIndex -notmatch '(?m)^\|\s*\[0022\]\(0022-named-landmark-map-and-asymmetric-marker\.md\)\s*\|[^\r\n]*\|\s*Accepted\s*\|') {
    Add-Failure 'ADR-0022 is not indexed as Accepted in docs/design/adr/README.md'
}
if ($adr -ne '' -and $adr -notmatch '\*\*Status:\*\* Accepted') {
    Add-Failure 'ADR-0022 is not Accepted'
}
$architecture = Read-Required 'docs/design/architecture.md'
if ($architecture -ne '' -and
    $architecture -notmatch '(?m)^\|[^\r\n]*Named landmark map[^\r\n]*\|\s*Implemented') {
    Add-Failure 'architecture.md does not mark the named landmark map Implemented'
}
$designReadme = Read-Required 'docs/design/README.md'
if ($designReadme -ne '' -and $designReadme -notmatch 'T12') {
    Add-Failure 'docs/design/README.md does not record T12'
}
$testing = Read-Required 'docs/design/testing-and-quality.md'
if ($testing -ne '' -and $testing -notmatch 'Landmark map and profile correspondences \(T12\)') {
    Add-Failure 'testing-and-quality.md has no T12 landmark-map section'
}
& node (Join-Path $root 'scripts/check-design-links.mjs')
if ($LASTEXITCODE -ne 0) {
    Add-Failure 'design-link check failed after the T12 documentation change'
}

# 7. The task note is local; check it only when the planning workspace exists.
$progressPath = Join-Path $root 'docs/progress/T12.md'
if (Test-Path -LiteralPath $progressPath) {
    $progress = Get-Content -LiteralPath $progressPath -Raw
    foreach ($needle in @('Follow-ups', 'Hand-off')) {
        if ($progress -notmatch [regex]::Escape($needle)) {
            Add-Failure "docs/progress/T12.md is missing '$needle'"
        }
    }
} else {
    Write-Host 'T12 gate: note - docs/progress/T12.md not present (gitignored workspace note); skipped'
}

if ($failures.Count -gt 0) {
    Write-Host 'T12 gate: FAIL' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

$mode = if ($env:AVATAR_VERIFY_MODE) { $env:AVATAR_VERIFY_MODE } else { 'gate-only' }
Write-Host ("T12 gate: PASS ($mode; named landmark map, pinned tables, asymmetric marker, " +
    'profile correspondences, design records)')
exit 0
