extends RefCounted

## Golden-image comparison for the M1 suite (T26).
##
## Mirrors `tests/tools/golden_compare` exactly: global SSIM over 8x8 luma
## windows with BT.601 luma and C1/C2 derived from L = 255, plus the maximum
## per-channel delta. The M1 suite adds a decal-bounding-box guard: SSIM is a
## global average and forgives a small shifted decal, so the max channel delta
## inside the decal's own bounding box is checked separately.

const SSIM_MIN := 0.995
const MAX_DELTA := 8
const WINDOW := 8


static func luma(image: Image, x: int, y: int) -> float:
	var colour := image.get_pixel(x, y)
	return 0.299 * colour.r * 255.0 + 0.587 * colour.g * 255.0 + 0.114 * colour.b * 255.0


## Global SSIM over 8x8 luma windows. Returns 0 for a size mismatch.
static func ssim(actual: Image, baseline: Image) -> float:
	if actual == null or baseline == null:
		return 0.0
	if actual.get_size() != baseline.get_size():
		return 0.0
	var width := actual.get_width()
	var height := actual.get_height()
	var c1 := pow(0.01 * 255.0, 2.0)
	var c2 := pow(0.03 * 255.0, 2.0)
	var total := 0.0
	var windows := 0
	for y0 in range(0, height, WINDOW):
		for x0 in range(0, width, WINDOW):
			var x1 := mini(x0 + WINDOW, width)
			var y1 := mini(y0 + WINDOW, height)
			var count := float((x1 - x0) * (y1 - y0))
			var sum_x := 0.0
			var sum_y := 0.0
			var sum_xx := 0.0
			var sum_yy := 0.0
			var sum_xy := 0.0
			for y in range(y0, y1):
				for x in range(x0, x1):
					var a := luma(actual, x, y)
					var b := luma(baseline, x, y)
					sum_x += a
					sum_y += b
					sum_xx += a * a
					sum_yy += b * b
					sum_xy += a * b
			var mean_x := sum_x / count
			var mean_y := sum_y / count
			var variance_x := maxf(sum_xx / count - mean_x * mean_x, 0.0)
			var variance_y := maxf(sum_yy / count - mean_y * mean_y, 0.0)
			var covariance := sum_xy / count - mean_x * mean_y
			var numerator := (2.0 * mean_x * mean_y + c1) * (2.0 * covariance + c2)
			var denominator := (mean_x * mean_x + mean_y * mean_y + c1) \
					* (variance_x + variance_y + c2)
			total += 1.0 if denominator <= 0.0 else numerator / denominator
			windows += 1
	return 0.0 if windows == 0 else total / float(windows)


static func max_delta(actual: Image, baseline: Image, rect: Rect2i) -> int:
	var maximum := 0
	var bounds := rect.intersection(Rect2i(Vector2i.ZERO, actual.get_size()))
	for y in range(bounds.position.y, bounds.end.y):
		for x in range(bounds.position.x, bounds.end.x):
			var a := actual.get_pixel(x, y)
			var b := baseline.get_pixel(x, y)
			maximum = maxi(maximum, roundi(absf(a.r - b.r) * 255.0))
			maximum = maxi(maximum, roundi(absf(a.g - b.g) * 255.0))
			maximum = maxi(maximum, roundi(absf(a.b - b.b) * 255.0))
	return maximum


static func max_delta_image(actual: Image, baseline: Image) -> int:
	return max_delta(actual, baseline, Rect2i(Vector2i.ZERO, actual.get_size()))


## Bounding box of pixels that differ by more than `threshold` (0..1). Used to
## derive the decal bounding box from the decal-on versus decal-off frames.
static func changed_rect(a: Image, b: Image, threshold := 1.0 / 255.0) -> Rect2i:
	var minimum := Vector2i(a.get_width(), a.get_height())
	var maximum := Vector2i(-1, -1)
	for y in range(a.get_height()):
		for x in range(a.get_width()):
			var ca := a.get_pixel(x, y)
			var cb := b.get_pixel(x, y)
			var delta := maxf(absf(ca.r - cb.r), maxf(absf(ca.g - cb.g), absf(ca.b - cb.b)))
			if delta > threshold:
				minimum.x = mini(minimum.x, x)
				minimum.y = mini(minimum.y, y)
				maximum.x = maxi(maximum.x, x)
				maximum.y = maxi(maximum.y, y)
	if maximum.x < 0:
		return Rect2i()
	return Rect2i(minimum, maximum - minimum + Vector2i.ONE)


## Amplified, red-tinted absolute difference, matching golden_compare.
static func diff_image(actual: Image, baseline: Image) -> Image:
	var diff := Image.create(actual.get_width(), actual.get_height(), false, Image.FORMAT_RGBA8)
	for y in range(actual.get_height()):
		for x in range(actual.get_width()):
			var a := actual.get_pixel(x, y)
			var b := baseline.get_pixel(x, y)
			var dr := mini(255.0, absf(a.r - b.r) * 255.0 * 8.0) / 255.0
			var dg := mini(255.0, absf(a.g - b.g) * 255.0 * 8.0) / 255.0
			var db := mini(255.0, absf(a.b - b.b) * 255.0 * 8.0) / 255.0
			diff.set_pixel(x, y, Color(dr, dg / 3.0, db / 3.0, 1.0))
	return diff
