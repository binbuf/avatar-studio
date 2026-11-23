extends Control

## Mapping pane for the T25c Setup window.
##
## A canonical-face-space view drawn from the same values the decal shader
## consumes: the authored region rects, the live region centroids and their
## rest-delta anchors, the head cut plane and falloff, and the pivot at the face
## origin. It is not a decorative diagram; moving a slider changes these values.

const Calibration = preload("res://scenes/live_calibration.gd")

const BACKGROUND := Color(0.035, 0.050, 0.070, 1.0)
const GRID_MINOR := Color(0.20, 0.27, 0.36, 0.45)
const GRID_MAJOR := Color(0.35, 0.46, 0.60, 0.75)
const AXIS_U := Color(1.0, 0.40, 0.40, 0.95)
const AXIS_V := Color(0.40, 1.0, 0.50, 0.95)
const CUT_COLOUR := Color(1.0, 0.75, 0.25, 0.95)
const FALLOFF_COLOUR := Color(1.0, 0.75, 0.25, 0.14)
const PIVOT_COLOUR := Color(0.95, 0.95, 1.0, 0.95)
const TEXT_COLOUR := Color(0.90, 0.94, 1.0, 1.0)
const MUTED := Color(0.62, 0.70, 0.82, 1.0)
const REGION_COLOURS := [
	Color(0.35, 0.85, 1.0, 1.0),
	Color(0.55, 0.95, 0.65, 1.0),
	Color(1.0, 0.60, 0.75, 1.0),
]

const EXTENT := 1.8

var calibration: Dictionary = {}
var authored: Array = []
var effective: Array = []
var live_centroids: Array = []
var rest_centroids: Array = []
var region_count := 0
var head_distance := 1.5
var head_falloff := 1.0
var face_scale := 1.0
var pivot := Vector2.ZERO
var draw_rect_area := Rect2()
var homography := PackedFloat32Array()
var selected_id := ""

var _scale := 1.0
var _origin := Vector2.ZERO


func _ready() -> void:
	clip_contents = true
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	resized.connect(queue_redraw)


## `values` is the working calibration; `state` is one pipeline tick (absolute
## canonical centroids and their rest values).
func set_data(values: Dictionary, state: Dictionary) -> void:
	calibration = values
	authored = values.get("regions", [])
	live_centroids = state.get("regions", [])
	rest_centroids = state.get("region_rest", [])
	region_count = authored.size()
	effective = []
	for index in range(authored.size()):
		var centre: Vector2 = authored[index].get("center", Vector2.ZERO)
		if index < live_centroids.size() and index < rest_centroids.size():
			centre += Vector2(live_centroids[index]) - Vector2(rest_centroids[index])
		effective.append(centre)
	head_distance = Calibration.head_distance(values) if not values.is_empty() else 1.5
	head_falloff = Calibration.head_falloff(values) if not values.is_empty() else 1.0
	face_scale = float(values.get("face", {}).get("scale", 1.0))
	pivot = Vector2.ZERO
	homography = state.get("canonical_to_image", PackedFloat32Array())
	queue_redraw()


func set_selected(id: String) -> void:
	if selected_id == id:
		return
	selected_id = id
	queue_redraw()


func _map(point: Vector2) -> Vector2:
	return _origin + Vector2(point.x * _scale, -point.y * _scale)


func _prepare() -> void:
	draw_rect_area = get_rect()
	if draw_rect_area.size.x <= 1.0 or draw_rect_area.size.y <= 1.0:
		return
	_scale = minf(draw_rect_area.size.x, draw_rect_area.size.y) / (2.0 * EXTENT)
	_origin = draw_rect_area.position + draw_rect_area.size * 0.5


func _draw() -> void:
	_prepare()
	if draw_rect_area.size.x <= 1.0:
		return
	draw_rect(draw_rect_area, BACKGROUND, true)

	var steps := 8
	for index in range(-steps, steps + 1):
		var value := float(index) / float(steps) * EXTENT
		var colour := GRID_MAJOR if index == 0 else GRID_MINOR
		var width := 1.5 if index == 0 else 1.0
		draw_line(_map(Vector2(-EXTENT, value)), _map(Vector2(EXTENT, value)), colour, width)
		draw_line(_map(Vector2(value, -EXTENT)), _map(Vector2(value, EXTENT)), colour, width)

	# Axes: +u is the subject's right, +v toward the forehead.
	draw_line(_map(Vector2.ZERO), _map(Vector2(1.45, 0.0)), AXIS_U, 2.0)
	draw_line(_map(Vector2(1.45, 0.0)), _map(Vector2(1.30, 0.07)), AXIS_U, 2.0)
	draw_line(_map(Vector2(1.45, 0.0)), _map(Vector2(1.30, -0.07)), AXIS_U, 2.0)
	draw_line(_map(Vector2.ZERO), _map(Vector2(0.0, 1.45)), AXIS_V, 2.0)
	draw_line(_map(Vector2(0.0, 1.45)), _map(Vector2(0.07, 1.30)), AXIS_V, 2.0)
	draw_line(_map(Vector2(0.0, 1.45)), _map(Vector2(-0.07, 1.30)), AXIS_V, 2.0)
	_text(_map(Vector2(1.47, 0.02)), "+u", AXIS_U, 11)
	_text(_map(Vector2(0.04, 1.47)), "+v", AXIS_V, 11)

	_draw_head_cut()
	_draw_pivot()
	_draw_regions()
	_draw_anchors()
	_draw_values()


func _draw_values() -> void:
	var origin := draw_rect_area.position + Vector2(6, 14)
	_text(origin, "scale %.3f  cut %.2f  falloff %.2f" % [face_scale, head_distance,
			head_falloff], TEXT_COLOUR, 10)
	if homography.size() != 9:
		return
	var y := draw_rect_area.position.y + draw_rect_area.size.y - 40
	_text(Vector2(draw_rect_area.position.x + 6, y),
			"H %.2f %.2f %.2f" % [homography[0], homography[1], homography[2]], MUTED, 9)
	_text(Vector2(draw_rect_area.position.x + 6, y + 11),
			"  %.2f %.2f %.2f" % [homography[3], homography[4], homography[5]], MUTED, 9)
	_text(Vector2(draw_rect_area.position.x + 6, y + 22),
			"  %.3f %.3f %.3f" % [homography[6], homography[7], homography[8]], MUTED, 9)


func _draw_head_cut() -> void:
	# AvatarModel places the cut at face.origin - up*scale*distance, so its
	# canonical v is -distance. The falloff band extends from the cut toward the
	# face by falloff/scale.
	var cut_v := -head_distance
	var band_top := cut_v + maxf(head_falloff, 0.0)
	var left := Vector2(-EXTENT, cut_v)
	var right := Vector2(EXTENT, cut_v)
	draw_rect(Rect2(_map(Vector2(-EXTENT, band_top)),
			Vector2(2.0 * EXTENT * _scale, maxf(0.0, (band_top - cut_v)) * _scale)),
			FALLOFF_COLOUR, true)
	draw_dashed_line(_map(left), _map(right), CUT_COLOUR, 2.0, 8.0)
	_text(_map(Vector2(-EXTENT + 0.05, cut_v + 0.06)),
			"head cut v=%.2f  falloff %.2f" % [cut_v, head_falloff], CUT_COLOUR, 11)


func _draw_pivot() -> void:
	var centre := _map(pivot)
	draw_line(centre + Vector2(-7, 0), centre + Vector2(7, 0), PIVOT_COLOUR, 2.0)
	draw_line(centre + Vector2(0, -7), centre + Vector2(0, 7), PIVOT_COLOUR, 2.0)
	draw_arc(centre, 5.0, 0.0, TAU, 20, PIVOT_COLOUR, 1.0)
	_text(centre + Vector2(8, -6), "pivot (0,0)", PIVOT_COLOUR, 10)


func _draw_regions() -> void:
	for index in range(authored.size()):
		var region: Dictionary = authored[index]
		var colour: Color = REGION_COLOURS[index % REGION_COLOURS.size()]
		var centre: Vector2 = region.get("center", Vector2.ZERO)
		var size: Vector2 = region.get("size", Vector2(0.1, 0.1))
		var rotation := float(region.get("rotation", deg_to_rad(float(
				region.get("rotation_deg", 0.0)))))
		var shape := String(region.get("shape", "ellipse"))
		var selected := String(region.get("id", "")) == selected_id
		# Authored rect: dim; effective rect (authored + live anchor delta): bright.
		_draw_region(centre, size, rotation, shape, colour * Color(1, 1, 1, 0.35), false,
				1.0)
		if index < effective.size():
			var width := 3.0 if selected else 2.0
			_draw_region(effective[index], size, rotation, shape,
					colour.lightened(0.2) if selected else colour, true, width)
			draw_line(_map(centre), _map(effective[index]), colour * Color(1, 1, 1, 0.6), 1.0)
		var label := "%s %.2f,%.2f" % [_short_id(String(region.get("id", "?"))),
				centre.x, centre.y]
		_text(_map(centre + Vector2(-size.x * 0.5, size.y * 0.5 + 0.12)), label, colour, 10)


func _short_id(id: String) -> String:
	match id:
		"eye_l":
			return "L"
		"eye_r":
			return "R"
		"mouth":
			return "M"
	return id


func _draw_region(centre: Vector2, size: Vector2, rotation: float, shape: String,
		colour: Color, fill: bool, width := 2.0) -> void:
	if size.x <= 0.0 or size.y <= 0.0:
		return
	var points := PackedVector2Array()
	if shape == "ellipse":
		for step in range(32):
			var angle := TAU * float(step) / 32.0
			var local := Vector2(cos(angle) * size.x * 0.5, sin(angle) * size.y * 0.5)
			points.append(_map(centre + local.rotated(rotation)))
	else:
		for corner in [Vector2(-0.5, -0.5), Vector2(0.5, -0.5), Vector2(0.5, 0.5),
				Vector2(-0.5, 0.5)]:
			var local := Vector2(corner.x * size.x, corner.y * size.y)
			points.append(_map(centre + local.rotated(rotation)))
	if fill:
		draw_colored_polygon(points, colour * Color(1, 1, 1, 0.10))
	for index in range(points.size()):
		draw_line(points[index], points[(index + 1) % points.size()], colour, width)


func _draw_anchors() -> void:
	for index in range(mini(live_centroids.size(), 3)):
		var live: Vector2 = live_centroids[index]
		var centre := _map(live)
		var colour: Color = REGION_COLOURS[index % REGION_COLOURS.size()]
		draw_line(centre + Vector2(-4, -4), centre + Vector2(4, 4), colour, 1.5)
		draw_line(centre + Vector2(-4, 4), centre + Vector2(4, -4), colour, 1.5)
		if index < rest_centroids.size():
			var rest := _map(rest_centroids[index])
			draw_line(rest + Vector2(-2, 0), rest + Vector2(2, 0),
					colour * Color(1, 1, 1, 0.55), 1.0)
			draw_line(rest + Vector2(0, -2), rest + Vector2(0, 2),
					colour * Color(1, 1, 1, 0.55), 1.0)


func _text(position: Vector2, text: String, colour: Color, size: int) -> void:
	draw_string(ThemeDB.fallback_font, position, text, HORIZONTAL_ALIGNMENT_LEFT, -1, size,
			colour)
