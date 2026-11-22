extends RefCounted

## Synthetic camera textures and canonical face data for the T20 flat-decal
## tests. Everything here is generated: no third-party images and no camera.
##
## The camera image is unmirrored. Canonical +u is the subject's right, so the
## homography maps canonical +u to the image's left side. The eye_r patch is
## therefore on the viewer's left, matching the model's own right side.

const TEXTURE_SIZE := 256
const COVERAGE_K := 0.45
const COVERAGE_U0 := 0.0
const COVERAGE_V0 := -0.15
const MARKER_IMAGE := Vector2(0.30, 0.45)


static func make_face(aabb: AABB) -> Dictionary:
	var centre := aabb.get_center()
	var scale := maxf(aabb.size.y * 0.32, 0.02)
	return {
		"origin": Vector3(centre.x, centre.y, aabb.position.z - 0.001),
		"normal": Vector3(0.0, 0.0, -1.0),
		"up": Vector3.UP,
		"right": Vector3.RIGHT,
		"scale": scale,
	}


static func make_regions() -> Array:
	return [
		{
			"id": "eye_l",
			"type": "eye",
			"center": Vector2(-0.5, 0.0),
			"size": Vector2(0.46, 0.34),
			"rotation_deg": 0.0,
			"shape": "ellipse",
			"feather": 0.18,
			"opacity": 1.0,
			"gain": Vector3.ONE,
			"lift": Vector3.ZERO,
		},
		{
			"id": "eye_r",
			"type": "eye",
			"center": Vector2(0.5, 0.0),
			"size": Vector2(0.46, 0.34),
			"rotation_deg": 0.0,
			"shape": "ellipse",
			"feather": 0.18,
			"opacity": 1.0,
			"gain": Vector3.ONE,
			"lift": Vector3.ZERO,
		},
		{
			"id": "mouth",
			"type": "mouth",
			"center": Vector2(0.0, -0.82),
			"size": Vector2(0.62, 0.46),
			"rotation_deg": 0.0,
			"shape": "rounded_rect",
			"feather": 0.22,
			"opacity": 1.0,
			"gain": Vector3.ONE,
			"lift": Vector3.ZERO,
		},
	]


## Canonical (u, v) -> image (x, y). Positive k is the unmirrored mapping:
## image x decreases as canonical u (the subject's right) increases.
static func make_homography(k: float = COVERAGE_K, u0: float = COVERAGE_U0,
		v0: float = COVERAGE_V0) -> Basis:
	return Basis(
		Vector3(-k, 0.0, 0.0),
		Vector3(0.0, -k, 0.0),
		Vector3(0.5 + k * u0, 0.5 + k * v0, 1.0))


static func make_camera_image(size: int = TEXTURE_SIZE) -> Image:
	var image := Image.create(size, size, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.55, 0.5, 0.45, 1.0))
	# eye_l samples the right side of the unmirrored image (subject's left).
	_fill_rect(image, Rect2(0.58, 0.30, 0.30, 0.40), Color(0.9, 0.1, 0.1, 1.0))
	# eye_r samples the left side (subject's right).
	_fill_rect(image, Rect2(0.12, 0.30, 0.30, 0.40), Color(0.1, 0.1, 0.9, 1.0))
	# mouth samples the lower centre.
	_fill_rect(image, Rect2(0.30, 0.62, 0.40, 0.33), Color(0.1, 0.8, 0.2, 1.0))
	# Asymmetric marker: canonical +u, viewer's left. A mirrored shader puts it
	# on the wrong eye.
	_draw_disc(image, MARKER_IMAGE, 0.02, Color(1.0, 0.0, 1.0, 1.0))
	return image


static func make_black_image(size: int = TEXTURE_SIZE) -> Image:
	var image := Image.create(size, size, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.0, 0.0, 0.0, 1.0))
	return image


static func make_marker_image(size: int = TEXTURE_SIZE) -> Image:
	var image := Image.create(size, size, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.05, 0.05, 0.05, 1.0))
	_draw_disc(image, MARKER_IMAGE, 0.02, Color(1.0, 0.0, 1.0, 1.0))
	return image


static func make_solid_image(value: float = 0.5, size: int = 32) -> Image:
	var image := Image.create(size, size, false, Image.FORMAT_RGBA8)
	image.fill(Color(value, value, value, 1.0))
	return image


static func srgb_to_linear(value: float) -> float:
	if value <= 0.04045:
		return value / 12.92
	return pow((value + 0.055) / 1.055, 2.4)


static func srgb_to_linear_rgb(value: Vector3) -> Vector3:
	return Vector3(srgb_to_linear(value.x), srgb_to_linear(value.y), srgb_to_linear(value.z))


static func sample_bilinear(image: Image, uv: Vector2) -> Vector3:
	var size := image.get_size()
	var x := uv.x * size.x - 0.5
	var y := uv.y * size.y - 0.5
	var x0 := floori(x)
	var y0 := floori(y)
	var fx := x - x0
	var fy := y - y0
	var c00 := _pixel(image, x0, y0)
	var c10 := _pixel(image, x0 + 1, y0)
	var c01 := _pixel(image, x0, y0 + 1)
	var c11 := _pixel(image, x0 + 1, y0 + 1)
	var top := c00.lerp(c10, fx)
	var bottom := c01.lerp(c11, fx)
	return top.lerp(bottom, fy)


static func _pixel(image: Image, x: int, y: int) -> Vector3:
	var size := image.get_size()
	var colour := image.get_pixel(clampi(x, 0, size.x - 1), clampi(y, 0, size.y - 1))
	return Vector3(colour.r, colour.g, colour.b)


static func _fill_rect(image: Image, rect: Rect2, colour: Color) -> void:
	var size := image.get_size()
	var x0 := clampi(floori(rect.position.x * size.x), 0, size.x)
	var y0 := clampi(floori(rect.position.y * size.y), 0, size.y)
	var x1 := clampi(ceili((rect.position.x + rect.size.x) * size.x), 0, size.x)
	var y1 := clampi(ceili((rect.position.y + rect.size.y) * size.y), 0, size.y)
	if x1 > x0 and y1 > y0:
		image.fill_rect(Rect2i(x0, y0, x1 - x0, y1 - y0), colour)


static func _draw_disc(image: Image, centre: Vector2, radius: float, colour: Color) -> void:
	var size := image.get_size()
	var cx := centre.x * size.x
	var cy := centre.y * size.y
	var r := radius * size.x
	var x0 := clampi(floori(cx - r), 0, size.x - 1)
	var x1 := clampi(ceili(cx + r), 0, size.x - 1)
	var y0 := clampi(floori(cy - r), 0, size.y - 1)
	var y1 := clampi(ceili(cy + r), 0, size.y - 1)
	for y in range(y0, y1 + 1):
		for x in range(x0, x1 + 1):
			if Vector2(x + 0.5 - cx, y + 0.5 - cy).length() <= r:
				image.set_pixel(x, y, colour)
