extends Control

## Overlay layer for the T25c camera pane. It is a sibling drawn after the
## camera ColorRect, so landmarks, the face box, the homography quad and the
## region anchors stay visible over the raw feed.

const LANDMARK_COLOUR := Color(0.35, 0.95, 1.0, 0.75)
const BOX_COLOUR := Color(1.0, 0.82, 0.30, 0.95)
const QUAD_COLOUR := Color(0.45, 1.0, 0.55, 0.95)
const ANCHOR_COLOUR := Color(1.0, 0.45, 0.75, 1.0)
const TEXT_COLOUR := Color(0.92, 0.95, 1.0, 1.0)

var image_rect := Rect2()
var landmarks := PackedVector2Array()
var face_box := Rect2()
var quad_points := PackedVector2Array()
var anchors: Array = []
var has_frame := false
var preview_enabled := true


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE


func set_overlay(rect: Rect2, points: PackedVector2Array, box: Rect2, quad: PackedVector2Array,
		anchor_points: Array, frame_visible: bool, preview_on: bool) -> void:
	image_rect = rect
	landmarks = points
	face_box = box
	quad_points = quad
	anchors = anchor_points
	has_frame = frame_visible
	preview_enabled = preview_on
	queue_redraw()


func _to_screen(point: Vector2) -> Vector2:
	return image_rect.position + Vector2(point.x * image_rect.size.x, point.y * image_rect.size.y)


func _clamp_to_image(point: Vector2) -> Vector2:
	return Vector2(clampf(point.x, image_rect.position.x, image_rect.end.x),
			clampf(point.y, image_rect.position.y, image_rect.end.y))


func _draw() -> void:
	if image_rect.size.x <= 1.0 or image_rect.size.y <= 1.0:
		return
	if not preview_enabled:
		draw_string(ThemeDB.fallback_font, image_rect.position + Vector2(12, 24),
				"Camera preview off (overlays only)", HORIZONTAL_ALIGNMENT_LEFT, -1, 13,
				TEXT_COLOUR)
	elif not has_frame:
		draw_string(ThemeDB.fallback_font, image_rect.position + Vector2(12, 24),
				"Waiting for a camera frame", HORIZONTAL_ALIGNMENT_LEFT, -1, 13, TEXT_COLOUR)

	for point in landmarks:
		if point.x < 0.0 or point.x > 1.0 or point.y < 0.0 or point.y > 1.0:
			continue
		draw_circle(_to_screen(point), 1.4, LANDMARK_COLOUR)

	if face_box.size.x > 0.0 and face_box.size.y > 0.0:
		var box := Rect2(_to_screen(face_box.position),
				Vector2(face_box.size.x * image_rect.size.x,
						face_box.size.y * image_rect.size.y))
		draw_rect(box, BOX_COLOUR, false, 2.0)
		draw_string(ThemeDB.fallback_font, box.position + Vector2(4, -4),
				"face box", HORIZONTAL_ALIGNMENT_LEFT, -1, 11, BOX_COLOUR)

	if quad_points.size() == 4:
		var clamped := PackedVector2Array()
		for point in quad_points:
			clamped.append(_clamp_to_image(point))
		for index in range(4):
			draw_line(clamped[index], clamped[(index + 1) % 4], QUAD_COLOUR, 2.0)
		draw_line(clamped[0], clamped[2], QUAD_COLOUR * Color(1, 1, 1, 0.35), 1.0)
		draw_line(clamped[1], clamped[3], QUAD_COLOUR * Color(1, 1, 1, 0.35), 1.0)
		draw_string(ThemeDB.fallback_font, clamped[0] + Vector2(4, -4),
				"H quad", HORIZONTAL_ALIGNMENT_LEFT, -1, 11, QUAD_COLOUR)

	for entry in anchors:
		var centre: Vector2 = entry.position
		if not image_rect.has_point(centre):
			continue
		draw_line(centre + Vector2(-5, 0), centre + Vector2(5, 0), ANCHOR_COLOUR, 2.0)
		draw_line(centre + Vector2(0, -5), centre + Vector2(0, 5), ANCHOR_COLOUR, 2.0)
		draw_string(ThemeDB.fallback_font, centre + Vector2(7, -5), String(entry.label),
				HORIZONTAL_ALIGNMENT_LEFT, -1, 11, ANCHOR_COLOUR)
