<#
.SYNOPSIS
    Generates the replay fixtures used by the component tests.

.DESCRIPTION
    The checked-in fixture is deliberately tiny (64x48, 3 frames, ~14 KB). Large
    recorded clips are not tracked: -Large writes a 320x180, 30-frame clip to
    tests/fixtures/generated/, which is gitignored. The Y4M is C420jpeg and the
    sidecar is the JSON track documented in docs/design/testing-and-quality.md.
#>
[CmdletBinding()]
param(
    [switch]$Large
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
if ($Large) {
    $directory = Join-Path $root 'tests/fixtures/generated'
    $width = 320
    $height = 180
    $frameCount = 30
    $fileName = 'synthetic_320x180_30f.y4m'
} else {
    $directory = Join-Path $root 'tests/fixtures/replay'
    $width = 64
    $height = 48
    $frameCount = 3
    $fileName = 'tiny.y4m'
}

New-Item -ItemType Directory -Force -Path $directory | Out-Null
$sidecarName = [System.IO.Path]::ChangeExtension($fileName, '.json')
$chromaWidth = [int]($width / 2)
$chromaHeight = [int]($height / 2)

$stream = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($stream)
$header = "YUV4MPEG2 W$width H$height F30:1 Ip A1:1 C420jpeg`n"
$writer.Write([System.Text.Encoding]::ASCII.GetBytes($header))

$frames = New-Object System.Collections.Generic.List[object]

for ($frame = 0; $frame -lt $frameCount; $frame++) {
    $writer.Write([System.Text.Encoding]::ASCII.GetBytes("FRAME`n"))

    $luma = New-Object byte[] ($width * $height)
    for ($y = 0; $y -lt $height; $y++) {
        for ($x = 0; $x -lt $width; $x++) {
            # Limited-range luma: a moving vertical bar over a slow gradient.
            $value = 16 + (($x * 3 + $y + $frame * 40) % 220)
            $luma[$y * $width + $x] = [byte]$value
        }
    }
    $writer.Write($luma)

    $u = New-Object byte[] ($chromaWidth * $chromaHeight)
    $v = New-Object byte[] ($chromaWidth * $chromaHeight)
    for ($index = 0; $index -lt ($chromaWidth * $chromaHeight); $index++) {
        $u[$index] = [byte](100 + (($index + $frame * 7) % 40))
        $v[$index] = [byte](140 + (($index + $frame * 5) % 40))
    }
    $writer.Write($u)
    $writer.Write($v)

    $correspondences = @(
        [ordered]@{ name = 'eye_outer_r'; u = 0.70; v = 0.0; x = 0.40; y = 0.45; ideal_x = 0.40; ideal_y = 0.45 },
        [ordered]@{ name = 'eye_outer_l'; u = -0.70; v = 0.0; x = 0.60; y = 0.45; ideal_x = 0.60; ideal_y = 0.45 },
        [ordered]@{ name = 'nose_tip'; u = 0.0; v = -0.32; x = 0.50; y = 0.58; ideal_x = 0.50; ideal_y = 0.58 },
        [ordered]@{ name = 'mouth_corner_r'; u = 0.30; v = -0.78; x = 0.45; y = 0.66; ideal_x = 0.45; ideal_y = 0.66 },
        [ordered]@{ name = 'marker_right_cheek'; u = 0.62; v = -0.25; x = 0.42; y = 0.60; ideal_x = 0.42; ideal_y = 0.60 }
    )
    $frames.Add([ordered]@{
            timestamp_ns   = [uint64]($frame * 16666667)
            features_valid = $true
            pose           = @(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, -0.6, 0.0, 0.0, 0.0, 1.0)
            correspondences = $correspondences
        })
}

$writer.Flush()
$y4mPath = Join-Path $directory $fileName
[System.IO.File]::WriteAllBytes($y4mPath, $stream.ToArray())
$writer.Dispose()
$stream.Dispose()

$sidecar = [ordered]@{
    version     = 1
    width       = $width
    height      = $height
    color_space = 'bt601_limited'
    frames      = $frames
}
$json = $sidecar | ConvertTo-Json -Depth 8
$sidecarPath = Join-Path $directory $sidecarName
[System.IO.File]::WriteAllText($sidecarPath, $json, (New-Object System.Text.UTF8Encoding($false)))

Write-Host "gen_fixtures: wrote $y4mPath"
Write-Host "gen_fixtures: wrote $sidecarPath"
