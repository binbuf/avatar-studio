#requires -Version 5.1
<#
T25a: single instance and activation.

The native suite owns the lock, the activation channel, the crash reclamation
and the CAP-002 contention path. The headless Godot phase drives the real main
scene: the first launch claims the lock, a separate probe process activates it
and exits, a second adapter reports secondary and delivers, and --new-instance
gets a distinct identity with the virtual camera and output registration off.
#>
[CmdletBinding()]
param([string]$BuildDir = 'build', [string]$Config = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Push-Location $root
try {
    & ctest --test-dir $BuildDir -C $Config -R '^avatar_instance_unit$' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Single-instance unit suite failed' }

    $tests = (& ctest --test-dir $BuildDir -C $Config -N -R '^avatar_instance_unit$') -join "`n"
    if ($tests -notmatch 'Total Tests: 1') { throw 'T25a unit suite is missing' }

    $godot = Join-Path $root '.tools/godot/Godot_v4.7.2-stable_win64_console.exe'
    if (-not (Test-Path -LiteralPath $godot)) { throw 'Pinned Windows Godot required for T25a' }
    $probe = Join-Path $root (Join-Path $BuildDir "$Config/avatar_instance_probe.exe")
    if (-not (Test-Path -LiteralPath $probe)) { throw 'avatar_instance_probe is missing' }

    $result = Join-Path $root '.tools/T25a-single-instance.json'
    $log = Join-Path $root '.tools/T25a-single-instance.log'
    $errors = Join-Path $root '.tools/T25a-single-instance-error.log'
    foreach ($path in @($result, $log, $errors)) {
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
    $appId = 't25a-godot-' + [guid]::NewGuid().ToString('N').Substring(0, 10)
    $env:AVATAR_INSTANCE_PROBE = (Resolve-Path -LiteralPath $probe).Path
    $env:AVATAR_T25A_RESULT = $result
    try {
        $process = Start-Process -FilePath $godot -WindowStyle Hidden -ArgumentList @(
            '--headless', '--path', 'app', '--script', 'res://tests/single_instance.gd',
            '--quit-after', 60000, '--', '--single-instance', '--instance-app-id', $appId
        ) -RedirectStandardOutput $log -RedirectStandardError $errors -PassThru -Wait
    } finally {
        Remove-Item Env:\AVATAR_INSTANCE_PROBE -ErrorAction SilentlyContinue
        Remove-Item Env:\AVATAR_T25A_RESULT -ErrorAction SilentlyContinue
    }
    Get-Content -LiteralPath $log | Write-Host
    $errorText = if (Test-Path -LiteralPath $errors) { Get-Content -LiteralPath $errors -Raw } else { '' }
    if ($errorText) { Write-Host $errorText }
    if ($process.ExitCode -ne 0) { throw 'Single-instance Godot contract failed' }
    if ($errorText -match '(?im)(SCRIPT ERROR|SHADER ERROR|^ERROR:)') {
        throw 'Single-instance Godot contract logged an engine error'
    }
    if (-not (Test-Path -LiteralPath $result)) { throw 'Single-instance evidence is missing' }

    $evidence = Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
    if (-not $evidence.passed) {
        throw ('Single-instance evidence failed: ' + (($evidence.failures) -join '; '))
    }
    if (-not [bool]$evidence.primary.primary) { throw 'First launch did not own the lock' }
    if (-not [bool]$evidence.primary.channel_ready) { throw 'Activation channel was not ready' }
    if ([int]$evidence.probe.exit_code -ne 0) { throw 'Second launch did not exit 0' }
    if ([string]$evidence.second.state -ne 'secondary-activated') {
        throw 'Second launch did not activate the first'
    }
    if (-not [bool]$evidence.second.secondary) { throw 'Second launch took the primary lock' }
    if ([int]$evidence.activation.activation_count -lt 2) {
        throw 'The running instance did not receive both activations'
    }
    if (-not [bool]$evidence.activation.window_raised) { throw 'Activation did not raise the window' }
    if ([string]$evidence.activation.notice.code -ne 'CFG-007') {
        throw 'Second-instance notice is missing its code'
    }
    if ([string]$evidence.activation.notice.sentence -ne 'Avatar Studio is already running') {
        throw 'Second-instance notice is not the reviewed copy'
    }
    if (-not [bool]$evidence.isolated.primary) { throw 'New instance did not become primary' }
    if (-not [bool]$evidence.isolated.identity.new_instance) {
        throw 'New instance identity is not marked'
    }
    if ([bool]$evidence.isolated.identity.virtual_camera_enabled) {
        throw 'New instance enabled the virtual camera'
    }
    if ([bool]$evidence.isolated.identity.output_registration_enabled) {
        throw 'New instance enabled output registration'
    }
    if ([string]$evidence.isolated.identity.shared_memory_name -eq
        [string]$evidence.primary.identity.shared_memory_name) {
        throw 'New instance reused the shared-memory name'
    }
    if (-not [bool]$evidence.normal_second.secondary) {
        throw 'Normal second launch did not stay secondary'
    }

    $arch = Get-Content docs/design/architecture.md -Raw
    if ($arch -notmatch 'Single instance \(T25a\)') {
        throw 'architecture.md does not record the realised single-instance lock'
    }
    $ux = Get-Content docs/design/editor-ux.md -Raw
    if ($ux -notmatch 'Second instance \(T25a\)') {
        throw 'editor-ux.md does not record the second-instance copy'
    }
    $diag = Get-Content docs/design/diagnostics.md -Raw
    if ($diag -notmatch 'CFG-007') { throw 'diagnostics.md does not register CFG-007' }
    $map = Get-Content docs/design/implementation-map.md -Raw
    if ($map -notmatch 'Single instance, activation') {
        throw 'implementation-map.md does not record T25a'
    }
    if (-not (Test-Path -LiteralPath docs/design/adr/0036-single-instance-and-activation.md)) {
        throw 'Missing ADR-0036'
    }

    & node scripts/check-design-links.mjs
    if ($LASTEXITCODE -ne 0) { throw 'Design-link check failed' }
    & powershell -NoProfile -ExecutionPolicy Bypass -File scripts/format-check.ps1 -Require
    if ($LASTEXITCODE -ne 0) { throw 'Formatting check failed' }
    Write-Host ('T25a gate: PASS (lock, activation, second launch exit, crash reclaim, ' +
        'CAP-002 contention, --new-instance isolation)')
} finally { Pop-Location }
