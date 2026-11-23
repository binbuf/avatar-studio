extends Control

## Overlay layer for the T25c camera pane. It is a sibling drawn after the
## camera ColorRect, so landmarks, the face box, the homography quad, the
## authored region shapes and the region anchors stay visible over the raw feed.
##
## The authored region shapes are projected through the live homography, which
## makes this pane WYSIWYG for the draw interaction: the box the operator drags
## becomes the ellipse/rounded rect shown here and in the decal.

const LANDMARK_COLOUR := Color(0.35, 0.95, 1.0, 0.75)
const BOX_COLOUR := Color(1.0, 0.82, 0.30, 0.95)
const QUAD_COLOUR := Color(0.45, 1.0, 0.55, 0.95)
const ANCHOR_COLOUR := Color(1.0, 0.45, 0.75, 1.0)
const TEXT_COLOUR := Color(0.92, 0.95, 1.0, 1.0)
const DRAW_COLOUR := Color(1.0, 1.0, 1.0, 0.95)
const HELD_COLOUR := Color(1.0, 0.72, 0.35, 0.95)
const REGION_COLOURS := [
	Color(0.35, 0.85, 1.0, 1.0),
	Color(0.55, 0.95, 0.65, 1.0),
	Color(1.0, 0.60, 0.75, 1.0),
]

var image_rect := Rect2()
var landmarks := PackedVector2Array()
var face_box := Rect2()
var quad_points := PackedVector2Array()
var anchors: Array = []
var region_outlines: Array = []
var drag_rect := Rect2()
var draw_mode := ""
var selected_id := ""
var has_frame := false
var preview_enabled := true
var have_mapping := true
var tracking_live := true


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE


func set_overlay(rect: Rect2, points: PackedVector2Array, box: Rect2, quad: PackedVector2Array,
		anchor_points: Array, frame_visible: bool, preview_on: bool,
		outlines: Array, drag: Rect2, mode: String, selected: String,
		mapping := true, live := true) -> void:
	image_rect = rect
	landmarks = points
	face_box = box
	quad_points = quad
	anchors = anchor_points
	has_frame = frame_visible
	preview_enabled = preview_on
	region_outlines = outlines
	drag_rect = drag
	draw_mode = mode
	selected_id = selected
	have_mapping = mapping
	tracking_live = live
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

	_draw_regions()
	_draw_anchors()
	_draw_drag()
	_draw_mapping_state()


func _draw_mapping_state() -> void:
	if not have_mapping:
		draw_string(ThemeDB.fallback_font, image_rect.position + Vector2(12, 44),
				"No live face mapping yet - move into frame", HORIZONTAL_ALIGNMENT_LEFT, -1,
				12, HELD_COLOUR)
	elif not tracking_live:
		draw_string(ThemeDB.fallback_font, image_rect.position + Vector2(12, 44),
				"Mapping held - no live face", HORIZONTAL_ALIGNMENT_LEFT, -1, 12,
				HELD_COLOUR)


func _draw_regions() -> void:
	for entry in region_outlines:
		var points: PackedVector2Array = entry.get("points", PackedVector2Array())
		if points.size() < 3:
			continue
		var index := int(entry.get("colour_index", 0))
		var colour: Color = REGION_COLOURS[index % REGION_COLOURS.size()]
		var selected := bool(entry.get("selected", false))
		if not tracking_live:
			colour = HELD_COLOUR
		elif selected:
			colour = colour.lightened(0.25)
		draw_colored_polygon(points, colour * Color(1, 1, 1, 0.12 if selected else 0.06))
		for point_index in range(points.size()):
			if tracking_live:
				draw_line(points[point_index], points[(point_index + 1) % points.size()],
						colour, 2.5 if selected else 1.5)
			else:
				draw_dashed_line(points[point_index],
						points[(point_index + 1) % points.size()], colour, 1.5, 4.0)
		if selected:
			var handle := _handle_rect(points)
			draw_rect(handle, colour, true)
			draw_rect(handle, Color(0.0, 0.0, 0.0, 0.55), false, 1.0)
		var label := _short_id(String(entry.get("id", "?")))
		draw_string(ThemeDB.fallback_font, points[0] + Vector2(4, -4), label,
				HORIZONTAL_ALIGNMENT_LEFT, -1, 11, colour)


func _draw_anchors() -> void:
	for entry in anchors:
		var centre: Vector2 = entry.position
		if not image_rect.has_point(centre):
			continue
		draw_line(centre + Vector2(-5, 0), centre + Vector2(5, 0), ANCHOR_COLOUR, 2.0)
		draw_line(centre + Vector2(0, -5), centre + Vector2(0, 5), ANCHOR_COLOUR, 2.0)
		draw_string(ThemeDB.fallback_font, centre + Vector2(7, -5), String(entry.label),
				HORIZONTAL_ALIGNMENT_LEFT, -1, 11, ANCHOR_COLOUR)


func _handle_rect(points: PackedVector2Array) -> Rect2:
	if points.size() < 3:
		return Rect2()
	var bounds := Rect2(points[0], Vector2.ZERO)
	for point in points:
		bounds = bounds.expand(point)
	return Rect2(bounds.end - Vector2(10, 10), Vector2(10, 10))


func _draw_drag() -> void:
	if drag_rect.size.x < 1.0 or drag_rect.size.y < 1.0:
		return
	draw_rect(drag_rect, DRAW_COLOUR, false, 2.0)
	draw_rect(drag_rect, DRAW_COLOUR * Color(1, 1, 1, 0.10), true)
	var label := "draw " + _short_id(draw_mode)
	draw_string(ThemeDB.fallback_font, drag_rect.position + Vector2(4, -4), label,
			HORIZONTAL_ALIGNMENT_LEFT, -1, 11, DRAW_COLOUR)


func _short_id(id: String) -> String:
	match id:
		"eye_l":
			return "L"
		"eye_r":
			return "R"
		"mouth":
			return "M"
	return id
