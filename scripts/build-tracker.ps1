#requires -Version 5.1
<#
.SYNOPSIS
Build the pinned, CPU-only MediaPipe tracker behind the five-function C ABI.
.DESCRIPTION
All downloads, extraction and Bazel caches stay under the project .tools/ directory.
MSYS2 bash and MSVC are prerequisites. CMake never downloads or compiles MediaPipe.
The output SDK is .tools/tracker-sdk; models are bundled, never fetched at runtime.
#>
[CmdletBinding()]
param([int]$Jobs = 6, [string]$Bash = 'C:\msys64\usr\bin\bash.exe')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
$work = Join-Path $root '.tools/t11'
$commit = 'cad7f3ab99ebf175947e40c5252c642612aae927'
$source = Join-Path $work "mediapipe-$commit"
$sdk = Join-Path $root '.tools/tracker-sdk'
if (-not (Test-Path -LiteralPath $Bash)) { throw "MSYS2 bash missing: $Bash" }
if ($Jobs -lt 1) { throw 'Jobs must be positive' }
New-Item -ItemType Directory -Force -Path $work, $sdk | Out-Null
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
function Fetch([string]$Name, [string]$Url, [string]$Hash) {
    $path = Join-Path $work $Name
    if ((Test-Path -LiteralPath $path) -and (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -eq $Hash) {
        return $path
    }
    Write-Host "tracker: downloading $Name"
    $partial = "$path.part"
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $partial
    if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -ne $Hash) { throw "SHA256 mismatch: $Name" }
    Move-Item -LiteralPath $partial -Destination $path -Force
    return $path
}
$zip = Fetch 'mediapipe.zip' "https://github.com/google-ai-edge/mediapipe/archive/$commit.zip" '6B7A48CBC3CA16048198C7A8482EF6D888E39A6A1C4641C60AC560277933F9C4'
$bazel = Fetch 'bazel.exe' 'https://github.com/bazelbuild/bazel/releases/download/6.5.0/bazel-6.5.0-windows-x86_64.exe' '6EAE8E7F28E1B68B833503D1A58CAF139C11E52DE19DF0D787D974653A0EA4C6'
$opencv = Fetch 'opencv-3.4.10.exe' 'https://github.com/opencv/opencv/releases/download/3.4.10/opencv-3.4.10-vc14_vc15.exe' 'BE79C1D4B73A4C6E74ED78CAEC342E6EF877B3D9C3C8A84DCB2499E2D8D4494D'
if (-not (Test-Path -LiteralPath $source)) { Expand-Archive -LiteralPath $zip -DestinationPath $work }
$opencvRoot = Join-Path $work 'opencv3410'
if (-not (Test-Path -LiteralPath (Join-Path $opencvRoot 'opencv/build/x64/vc15/lib/opencv_world3410.lib'))) {
    $process = Start-Process -FilePath $opencv -ArgumentList '-y', "-o$opencvRoot" -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "OpenCV extraction failed: $($process.ExitCode)" }
}
$workspacePath = Join-Path $source 'WORKSPACE'
$workspace = [IO.File]::ReadAllText($workspacePath)
$localOpenCV = (Join-Path $opencvRoot 'opencv/build').Replace('\', '/')
$workspace = $workspace.Replace('C:\\opencv\\build', $localOpenCV)
if (-not $workspace.Contains('avatar_glog_static.diff')) {
    $workspace = $workspace.Replace('"@//third_party:com_github_glog_glog_windows_patch.diff",', '"@//third_party:com_github_glog_glog_windows_patch.diff",' + "`n" + '        "@//third_party:avatar_glog_static.diff",')
}
[IO.File]::WriteAllText($workspacePath, $workspace)
Copy-Item (Join-Path $root 'native/tracking/mediapipe/patches/glog-static.diff') (Join-Path $source 'third_party/avatar_glog_static.diff') -Force
Push-Location $source
try {
    # Earlier local spikes wrote this header with CRLF. Match the pristine archive
    # and the LF-pinned patch so both fresh extraction and cached rebuilds work.
    $header = Join-Path $source 'mediapipe/tasks/cc/core/task_api_factory.h'
    [IO.File]::WriteAllText($header, [IO.File]::ReadAllText($header).Replace("`r`n", "`n"))
    $patch = Join-Path $root 'native/tracking/mediapipe/patches/0001-msvc-preprocessor.patch'
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & git apply --check $patch 2>&1 | Out-Null
    $patchNeeded = $LASTEXITCODE -eq 0
    $ErrorActionPreference = $savedPreference
    if ($patchNeeded) {
        & git apply $patch
        if ($LASTEXITCODE -ne 0) { throw 'MediaPipe MSVC patch failed' }
    } else {
        $ErrorActionPreference = 'Continue'
        & git apply --reverse --check $patch 2>&1 | Out-Null
        $alreadyPatched = $LASTEXITCODE -eq 0
        $ErrorActionPreference = $savedPreference
        if (-not $alreadyPatched) { throw 'MediaPipe source differs from the pinned patch' }
    }
    $wrapper = Join-Path $source 'avatar_tracker'
    New-Item -ItemType Directory -Force -Path $wrapper, (Join-Path $wrapper 'avatar/tracker') | Out-Null
    Copy-Item (Join-Path $root 'native/tracking/mediapipe/*') $wrapper -Force -Exclude patches
    Copy-Item (Join-Path $root 'native/tracking/include/avatar/tracker/tracker_c_api.h') (Join-Path $wrapper 'avatar/tracker') -Force
    $env:BAZEL_SH = $Bash
    $cache = (Join-Path $work 'bazel-root').Replace('\', '/')
    $buildArguments = @("--output_user_root=$cache", '--batch', 'build', "--jobs=$Jobs", '-c', 'opt', '--copt=/DTFL_STATIC_LIBRARY_BUILD', '--repo_env=HERMETIC_PYTHON_VERSION=3.12', '--define', 'MEDIAPIPE_DISABLE_GPU=1', '//avatar_tracker:libavatartracker.dll', '//avatar_tracker:avatar_tracker_bench')
    Write-Host ('>> bazel ' + ($buildArguments -join ' '))
    & $bazel @buildArguments
    if ($LASTEXITCODE -ne 0) { throw "MediaPipe tracker build failed: $LASTEXITCODE" }
    Copy-Item 'bazel-bin/avatar_tracker/libavatartracker.dll', 'bazel-bin/avatar_tracker/avatar_tracker_bench.exe' $sdk -Force
} finally { Pop-Location }
Copy-Item (Join-Path $opencvRoot 'opencv/build/x64/vc15/bin/opencv_world3410.dll') $sdk -Force
$modelDir = Join-Path $sdk 'tracker-models'
New-Item -ItemType Directory -Force -Path $modelDir | Out-Null
$manifest = Get-Content (Join-Path $root 'native/licenses.json') -Raw | ConvertFrom-Json
$model = @($manifest.components | Where-Object id -eq 'face_landmarker_bundle')[0]
$modelCache = Fetch 'face_landmarker.task' $model.url $model.sha256
Copy-Item -LiteralPath $modelCache -Destination (Join-Path $modelDir 'face_landmarker.task') -Force
Copy-Item (Join-Path $source 'LICENSE') (Join-Path $sdk 'LICENSE-mediapipe.txt') -Force
Copy-Item (Join-Path $opencvRoot 'opencv/LICENSE.txt') (Join-Path $sdk 'LICENSE-opencv.txt') -Force
Write-Host "tracker: built $sdk"
