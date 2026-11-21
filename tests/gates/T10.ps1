<#
.SYNOPSIS
    T10 gate: the Windows Media Foundation capture backend and its platform-neutral
    state machine exist, are tested without a camera, and enumerate real devices.

.DESCRIPTION
    Runs after the native build and the fast tests. Checks:

      1. The capture seam, state machine, Windows backend, smoke command and tests exist.
      2. The Media Foundation backend uses an async IMFSourceReader with an explicit media
         type and never enables advanced video processing.
      3. Device identity is the symbolic link; the state machine owns the CAP mapping, the
         2-second reconnect poll, the watchdog and suspend/resume.
      4. The unit suite covers negotiation, CAP-003, busy/permission, timestamps, CAP-005,
         the watchdog, reconnect, suspend/resume, ring drops and the latency histogram.
      5. CTest registers the capture suite and the --list-cameras smoke under task_T10, and
         both pass.
      6. --list-cameras exits 0 with or without a camera (it never opens a device).
      7. The design documents, the implementation map and ADR-0020 record the implemented
         behaviour.
      8. docs/progress/T10.md, when present, has Follow-ups and Hand-off.

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

# 1. Required implementation, tool and test files.
$deviceHeader = Read-Required 'native/capture/include/avatar/capture/camera_device.h'
$sourceHeader = Read-Required 'native/capture/include/avatar/capture/camera_source.h'
$sourceImpl = Read-Required 'native/capture/src/camera_source.cpp'
$deviceImpl = Read-Required 'native/capture/src/camera_device.cpp'
$mfImpl = Read-Required 'native/capture/windows/mf_capture.cpp'
$null = Read-Required 'native/capture/src/null_camera_backend.cpp'
$null = Read-Required 'native/capture/tools/list_cameras.cpp'
$cameraTests = Read-Required 'tests/unit/test_camera_source.cpp'
$cameraDesign = Read-Required 'docs/design/camera-capture.md'
$null = Read-Required 'docs/design/adr/0020-capture-state-machine-and-mf-backend.md'

# 2. The Media Foundation backend contract.
if ($mfImpl -ne '') {
    foreach ($needle in @(
            'IMFSourceReaderCallback',
            'MF_SOURCE_READER_ASYNC_CALLBACK',
            'MFVideoFormat_NV12',
            'MFVideoFormat_YUY2',
            'MF_MT_FRAME_SIZE',
            'MF_MT_FRAME_RATE',
            'MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK',
            'MFEnumDeviceSources',
            'SetCurrentMediaType',
            'MF_SOURCE_READER_FIRST_VIDEO_STREAM',
            'GetSampleTime',
            'MF_MT_DEFAULT_STRIDE',
            'MF_MT_YUV_MATRIX')) {
        if ($mfImpl -notmatch [regex]::Escape($needle)) {
            Add-Failure "mf_capture.cpp is missing '$needle'"
        }
    }
    if ($mfImpl -match 'MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING\s*,\s*TRUE') {
        Add-Failure 'mf_capture.cpp enables advanced video processing (silent conversion)'
    }
    if ($mfImpl -match 'MF_SOURCE_READER_D3D_MANAGER') {
        Add-Failure 'mf_capture.cpp uses the D3D manager; GPU zero-copy is out of scope for T10'
    }
}

# 3. The platform-neutral state machine owns policy and failure mapping.
if ($deviceHeader -ne '') {
    foreach ($needle in @('ICameraDeviceBackend', 'enumerate', 'capabilities', 'open',
                          'start', 'stop', 'close', 'create_platform_camera_backend',
                          'default_camera_preferences')) {
        if ($deviceHeader -notmatch [regex]::Escape($needle)) {
            Add-Failure "camera_device.h is missing '$needle'"
        }
    }
}
if ($sourceHeader -ne '') {
    foreach ($needle in @('class CameraSource', 'CameraState', 'CameraEvent', 'suspend',
                          'resume', 'reconnect_poll_ns', 'watchdog_multiplier',
                          'startup_grace_ns', 'publish_latency')) {
        if ($sourceHeader -notmatch [regex]::Escape($needle)) {
            Add-Failure "camera_source.h is missing '$needle'"
        }
    }
    if ($sourceHeader -notmatch [regex]::Escape("2'000'000'000ull")) {
        Add-Failure 'camera_source.h does not default the reconnect poll to 2 seconds'
    }
}
if ($sourceImpl -ne '') {
    foreach ($needle in @('cap_001', 'cap_002', 'cap_003', 'cap_004', 'cap_005',
                          'cap_006', 'cap_007', 'cap_008', 'device_restored',
                          'check_watchdog', 'check_shortfall', 'maybe_reconnect',
                          'demote_mode', 'observed_interval_ns_')) {
        if ($sourceImpl -notmatch [regex]::Escape($needle)) {
            Add-Failure "camera_source.cpp is missing '$needle'"
        }
    }
}
if ($deviceImpl -ne '') {
    foreach ($needle in @('kFormatPolicy', 'PixelFormat::nv12', 'PixelFormat::yuy2',
                          'PixelFormat::bgra8', 'classify_camera_hresult')) {
        if ($deviceImpl -notmatch [regex]::Escape($needle)) {
            Add-Failure "camera_device.cpp is missing '$needle'"
        }
    }
    if ($deviceImpl -match 'PixelFormat::mjpeg\s*,') {
        Add-Failure 'camera_device.cpp selects MJPEG; FrameRing cannot publish it without a decoder'
    }
}

# 4. Test coverage: the cases the task names must have a test each.
if ($cameraTests -ne '') {
    foreach ($needle in @(
            'camera_enumerates_opens_and_streams_the_first_device',
            'camera_steps_down_to_an_advertised_format_and_emits_cap_003',
            'camera_steps_down_after_the_backend_rejects_a_mode',
            'camera_maps_busy_and_permission_failures',
            'camera_reports_no_camera_with_cap_008',
            'camera_maps_source_timestamps_to_frame_clock_and_preserves_arrival',
            'camera_remaps_backwards_timestamps_once_per_episode',
            'camera_watchdog_restarts_a_stalled_stream_once_then_reports',
            'camera_demotes_a_mode_that_cannot_sustain_its_frame_rate',
            'camera_reconnects_after_device_loss_on_the_two_second_poll',
            'camera_reports_a_failed_reopen_once_and_recovers_later',
            'camera_maps_a_mid_stream_preemption_to_cap_002_and_recovers',
            'camera_suspend_and_resume_reset_the_timestamp_base',
            'camera_drops_frames_when_every_ring_slot_is_pinned',
            'camera_records_callback_to_publish_latency',
            'camera_default_preferences_follow_the_documented_order',
            'camera_hresult_classification_matches_the_documented_states')) {
        if ($cameraTests -notmatch [regex]::Escape($needle)) {
            Add-Failure "test_camera_source.cpp is missing the '$needle' case"
        }
    }
}

# 5. CTest registration and execution.
$cmake = Read-Required 'CMakeLists.txt'
if ($cmake -ne '') {
    foreach ($needle in @('avatar_camera_tests', 'avatar_list_cameras',
                          'native/capture/windows/mf_capture.cpp', 'task_T10')) {
        if ($cmake -notmatch [regex]::Escape($needle)) {
            Add-Failure "CMakeLists.txt is missing '$needle'"
        }
    }
}
$ctestBase = @('--test-dir', (Join-Path $root $BuildDir), '-C', $Config)
$taskList = Invoke-Capture 'ctest' ($ctestBase + @('-N', '-L', 'task_T10'))
if ($taskList.Code -ne 0 -or $taskList.Output -notmatch 'avatar_camera_unit') {
    Add-Failure 'CTest does not list avatar_camera_unit under label task_T10 (build first)'
}
if ($taskList.Code -ne 0 -or $taskList.Output -notmatch 'avatar_list_cameras_smoke') {
    Add-Failure 'CTest does not list avatar_list_cameras_smoke under label task_T10'
}
$run = Invoke-Capture 'ctest' ($ctestBase + @('-L', 'task_T10', '--output-on-failure'))
if ($run.Code -ne 0) {
    Add-Failure "the task_T10 tests failed: $($run.Output)"
}

# 6. The smoke command never opens a device and exits 0 without one.
$tool = Join-Path $root "$BuildDir/$Config/avatar_list_cameras.exe"
if (-not (Test-Path -LiteralPath $tool)) {
    $tool = Join-Path $root "$BuildDir/avatar_list_cameras"
}
if (-not (Test-Path -LiteralPath $tool)) {
    Add-Failure 'avatar_list_cameras executable is missing (build first)'
} else {
    $list = Invoke-Capture $tool @('--list-cameras')
    if ($list.Code -ne 0) {
        Add-Failure "--list-cameras failed with exit code $($list.Code): $($list.Output)"
    }
    if ($list.Output -notmatch 'capture devices|no camera found') {
        Add-Failure "--list-cameras produced no device summary: $($list.Output)"
    }
}

# 7. Design records and the implementation map.
if ($cameraDesign -ne '') {
    foreach ($needle in @('mf_capture.cpp', 'CameraSource', 'ICameraDeviceBackend',
                          'MF_SOURCE_READER_ASYNC_CALLBACK', 'CAP-003', 'CAP-005',
                          'device_restored', '2 s', 'suspend')) {
        if ($cameraDesign -notmatch [regex]::Escape($needle)) {
            Add-Failure "camera-capture.md does not document '$needle'"
        }
    }
}
$map = Read-Required 'docs/design/implementation-map.md'
if ($map -ne '') {
    if ($map -notmatch 'current as of T(\d{2,3})') {
        Add-Failure 'implementation-map.md has no "current as of TNN" marker'
    } elseif ([int]$Matches[1] -lt 10) {
        Add-Failure "implementation-map.md is stale (current as of T$($Matches[1]), expected T10 or later)"
    }
    if ($map -notmatch '(?m)^\|\s*\[ADR-0020\]\(adr/0020-capture-state-machine-and-mf-backend\.md\)[^\r\n]*\|\s*Accepted\s*\|') {
        Add-Failure 'implementation-map.md does not list ADR-0020 as Accepted'
    }
    if ($map -notmatch '(?m)^\|[^\r\n]*Windows backend: Media Foundation[^\r\n]*\|\s*`Implemented`\s*\|') {
        Add-Failure 'implementation-map.md has no Implemented Windows capture row'
    }
}
$adrIndex = Read-Required 'docs/design/adr/README.md'
if ($adrIndex -ne '' -and
    $adrIndex -notmatch '(?m)^\|\s*\[0020\]\(0020-capture-state-machine-and-mf-backend\.md\)\s*\|[^\r\n]*\|\s*Accepted\s*\|') {
    Add-Failure 'ADR-0020 is not indexed as Accepted in docs/design/adr/README.md'
}
$architecture = Read-Required 'docs/design/architecture.md'
if ($architecture -ne '' -and
    $architecture -notmatch '(?m)^\|[^\r\n]*Windows Media Foundation capture[^\r\n]*\|\s*Implemented') {
    Add-Failure 'architecture.md does not mark the Windows capture backend Implemented'
}

# 8. The task note is local; check it only when the planning workspace exists.
$progressPath = Join-Path $root 'docs/progress/T10.md'
if (Test-Path -LiteralPath $progressPath) {
    $progress = Get-Content -LiteralPath $progressPath -Raw
    foreach ($needle in @('Follow-ups', 'Hand-off')) {
        if ($progress -notmatch [regex]::Escape($needle)) {
            Add-Failure "docs/progress/T10.md is missing '$needle'"
        }
    }
} else {
    Write-Host 'T10 gate: note - docs/progress/T10.md not present (gitignored workspace note); skipped'
}

if ($failures.Count -gt 0) {
    Write-Host 'T10 gate: FAIL' -ForegroundColor Red
    foreach ($failure in $failures) {
        Write-Host "  - $failure" -ForegroundColor Red
    }
    exit 1
}

$mode = if ($env:AVATAR_VERIFY_MODE) { $env:AVATAR_VERIFY_MODE } else { 'gate-only' }
Write-Host ("T10 gate: PASS ($mode; capture state machine, Media Foundation backend, " +
    '--list-cameras smoke, design records)')
exit 0
