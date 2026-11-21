# golden_compare

Perceptual image comparator for golden tests. It compares two 8-bit RGB PPM (P6) images with:

- **SSIM** over luma, 8x8 windows, mean across windows. Default minimum: 0.995.
- **Max per-channel delta**: the largest absolute difference on any R, G or B sample. Default
  maximum: 8.

A comparison passes only when both thresholds pass. The CLI exits 0 on pass, 1 on a threshold
failure and 2 on a usage or I/O error.

## Usage

```powershell
golden_compare --actual build/actual.ppm --baseline tests/golden/frame.ppm `
  --artifacts build/golden-artifacts --name frame
```

| Argument | Meaning |
|---|---|
| `--actual` | Image produced by the current build (required). |
| `--baseline` | Stored reference image (required). |
| `--artifacts <dir>` | Write `<name>.actual.ppm`, `<name>.baseline.ppm`, `<name>.diff.ppm`. |
| `--name <stem>` | Artifact stem; default `golden`. |
| `--ssim-min <value>` | Override the SSIM floor. |
| `--max-delta <n>` | Override the per-channel delta ceiling. |
| `--self-test <dir>` | Run the built-in pass/fail/artifact checks and exit. |
| `--help` | Print usage. |

The diff image is the absolute difference amplified 8x and tinted red. It is written on both pass
and fail when `--artifacts` is given, so a CI job can attach all three files unconditionally.

## Why PPM

PPM needs no third-party decoder, is lossless, and is trivial to read and diff. If a later golden
task needs browser-viewable artifacts, add a PNG writer behind the same `Image` type; the comparison
logic does not care about the container.

## CTest

`golden_compare_cli` runs `golden_compare --self-test <build>/golden_compare_selftest` under the
`fast;task_T04` labels. It checks identical images pass, a visibly different pair fails, the
max-delta guard catches a single large pixel change, PPM round-trips exactly, and all three
artifacts are written for both outcomes.
