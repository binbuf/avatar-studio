extends Control

## Mapping pane for the T25c Setup window.
##
## A canonical-face-space view drawn from the same values the decal shader
## consumes: the authored region rects, the live region centroids and their
## rest-delta anchors, the head cut plane and falloff, and the pivot at the face
## origin. It is not a decorative diagram.
##
## Direct manipulation (same contract as the camera and model panes):
## - with a region mode active, drag a box to set that region's centre and size;
## - with no mode, drag a cutout outline to move it, or drag the square handle at
##   its bottom-right corner to scale it.
##
## The pointer is converted to canonical face coordinates directly from this
## view's grid. The setup window subtracts the FaceRig anchor and persists the
## authored region.

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
const REGION_IDS := ["eye_l", "eye_r", "mouth"]
const HIT_RADIUS := 26.0
const HANDLE_SIZE := 10.0

const EXTENT := 1.8

signal region_selected(id: String)
signal region_dragged(id: String, center: Vector2)
signal region_drawn(id: String, center: Vector2, size: Vector2)
signal region_resized(id: String, size: Vector2)

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
var draw_mode := ""
var polygons: Dictionary = {}

var _scale := 1.0
var _origin := Vector2.ZERO
var _moving := false
var _move_id := ""
var _drawing := false
var _draw_start := Vector2.ZERO
var _draw_current := Vector2.ZERO
var _resizing := false


func _ready() -> void:
	clip_contents = true
	mouse_filter = Control.MOUSE_FILTER_STOP
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


func get_selected() -> String:
	return selected_id


## "" keeps the move/scale interaction. A region id makes a drag draw a box.
func set_draw_mode(id: String) -> void:
	draw_mode = id if REGION_IDS.has(id) else ""
	_moving = false
	_drawing = false
	_resizing = false
	queue_redraw()


func get_draw_mode() -> String:
	return draw_mode


func get_region_polygons() -> Dictionary:
	return polygons.duplicate(true)


## Converts a pane-space point to canonical face coordinates.
func canonical_from_screen(point: Vector2) -> Variant:
	if _scale <= 0.0:
		return null
	var local := (point - _origin) / _scale
	return Vector2(local.x, -local.y)


func screen_from_canonical(center: Vector2) -> Vector2:
	return _map(center)


## Converts a pane-space box into a canonical centre/size.
func canonical_rect_from_screen_rect(screen_rect: Rect2) -> Dictionary:
	var corners := [
		screen_rect.position,
		Vector2(screen_rect.end.x, screen_rect.position.y),
		screen_rect.end,
		Vector2(screen_rect.position.x, screen_rect.end.y),
	]
	var min_u := INF
	var max_u := -INF
	var min_v := INF
	var max_v := -INF
	for corner in corners:
		var canonical: Variant = canonical_from_screen(corner)
		if canonical == null:
			return {}
		var point: Vector2 = canonical
		min_u = minf(min_u, point.x)
		max_u = maxf(max_u, point.x)
		min_v = minf(min_v, point.y)
		max_v = maxf(max_v, point.y)
	var size := Vector2(max_u - min_u, max_v - min_v)
	if size.x <= 0.0 or size.y <= 0.0:
		return {}
	return {
		"center": Vector2((min_u + max_u) * 0.5, (min_v + max_v) * 0.5),
		"size": size,
	}


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
	polygons = {}
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
	_draw_preview()


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
		var id := String(region.get("id", ""))
		var colour: Color = REGION_COLOURS[index % REGION_COLOURS.size()]
		var centre: Vector2 = region.get("center", Vector2.ZERO)
		var size: Vector2 = region.get("size", Vector2(0.1, 0.1))
		var rotation := float(region.get("rotation", deg_to_rad(float(
				region.get("rotation_deg", 0.0)))))
		var shape := String(region.get("shape", "ellipse"))
		var selected := id == selected_id
		# Authored rect: dim; effective rect (authored + live anchor delta): bright.
		var authored_points := _region_points(centre, size, rotation, shape)
		_draw_region_points(authored_points, colour * Color(1, 1, 1, 0.35), false, 1.0)
		if index < effective.size():
			var effective_points := _region_points(effective[index], size, rotation, shape)
			polygons[id] = effective_points
			var width := 3.0 if selected else 2.0
			_draw_region_points(effective_points,
					colour.lightened(0.2) if selected else colour, true, width)
			draw_line(_map(centre), _map(effective[index]),
					colour * Color(1, 1, 1, 0.6), 1.0)
			if selected:
				_draw_handle(effective_points, colour)
		var label := "%s %.2f,%.2f" % [_short_id(id), centre.x, centre.y]
		_text(_map(centre + Vector2(-size.x * 0.5, size.y * 0.5 + 0.12)), label, colour, 10)


func _region_points(centre: Vector2, size: Vector2, rotation: float,
		shape: String) -> PackedVector2Array:
	var points := PackedVector2Array()
	if size.x <= 0.0 or size.y <= 0.0:
		return points
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
	return points


func _draw_region_points(points: PackedVector2Array, colour: Color, fill: bool,
		width: float) -> void:
	if points.size() < 3:
		return
	if fill:
		draw_colored_polygon(points, colour * Color(1, 1, 1, 0.10))
	for index in range(points.size()):
		draw_line(points[index], points[(index + 1) % points.size()], colour, width)


func _draw_handle(points: PackedVector2Array, colour: Color) -> void:
	var handle := _bounds_handle(points)
	if handle.size.x <= 0.0:
		return
	draw_rect(handle, colour, true)
	draw_rect(handle, Color(0.0, 0.0, 0.0, 0.55), false, 1.0)


func _draw_preview() -> void:
	if not _drawing:
		return
	var box := _drag_rect()
	if box.size.x < 1.0 or box.size.y < 1.0:
		return
	draw_rect(box, Color(1.0, 1.0, 1.0, 0.95), false, 2.0)
	draw_rect(box, Color(1.0, 1.0, 1.0, 0.10), true)
	draw_string(ThemeDB.fallback_font, box.position + Vector2(4, -4),
			"draw " + _short_id(draw_mode), HORIZONTAL_ALIGNMENT_LEFT, -1, 11,
			Color(1.0, 1.0, 1.0, 0.95))


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


func _gui_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT:
		if event.pressed:
			if draw_mode != "":
				_drawing = true
				_draw_start = event.position
				_draw_current = event.position
				accept_event()
				return
			if selected_id != "" and _handle_rect().has_point(event.position):
				_resizing = true
				accept_event()
				return
			var id := _hit_region(event.position)
			if id != "":
				selected_id = id
				_moving = true
				_move_id = id
				region_selected.emit(id)
				queue_redraw()
				accept_event()
		elif _drawing:
			_drawing = false
			var box := _drag_rect()
			queue_redraw()
			accept_event()
			if box.size.x >= 4.0 and box.size.y >= 4.0:
				var result := canonical_rect_from_screen_rect(box)
				if not result.is_empty():
					region_drawn.emit(draw_mode, Vector2(result.center),
							Vector2(result.size))
		elif _moving:
			_moving = false
			_move_id = ""
			accept_event()
		elif _resizing:
			_resizing = false
			accept_event()
	elif event is InputEventMouseMotion:
		if _drawing:
			_draw_current = event.position
			queue_redraw()
			accept_event()
		elif _moving:
			var canonical: Variant = canonical_from_screen(event.position)
			if canonical != null:
				region_dragged.emit(_move_id, canonical)
			accept_event()
		elif _resizing:
			_apply_resize(event.position)
			accept_event()


func _apply_resize(point: Vector2) -> void:
	if selected_id == "":
		return
	var index := REGION_IDS.find(selected_id)
	if index < 0 or index >= effective.size():
		return
	var canonical: Variant = canonical_from_screen(point)
	if canonical == null:
		return
	var centre: Vector2 = effective[index]
	var size := Vector2(absf(Vector2(canonical).x - centre.x) * 2.0,
			absf(Vector2(canonical).y - centre.y) * 2.0)
	size = Vector2(maxf(size.x, 0.02), maxf(size.y, 0.02))
	region_resized.emit(selected_id, size)


func _drag_rect() -> Rect2:
	if _draw_start == _draw_current:
		return Rect2()
	var position := Vector2(minf(_draw_start.x, _draw_current.x),
			minf(_draw_start.y, _draw_current.y))
	var size := Vector2(absf(_draw_current.x - _draw_start.x),
			absf(_draw_current.y - _draw_start.y))
	return Rect2(position, size)


func _handle_rect() -> Rect2:
	if selected_id == "" or not polygons.has(selected_id):
		return Rect2()
	return _bounds_handle(polygons[selected_id])


func _bounds_handle(points: PackedVector2Array) -> Rect2:
	if points.size() < 3:
		return Rect2()
	var bounds := Rect2(points[0], Vector2.ZERO)
	for point in points:
		bounds = bounds.expand(point)
	return Rect2(bounds.end - Vector2(HANDLE_SIZE, HANDLE_SIZE),
			Vector2(HANDLE_SIZE, HANDLE_SIZE))


func _hit_region(point: Vector2) -> String:
	var nearest := ""
	var nearest_distance := INF
	for id in polygons.keys():
		var polygon: PackedVector2Array = polygons[id]
		if polygon.size() >= 3 and Geometry2D.is_point_in_polygon(point, polygon):
			return id
		if polygon.size() >= 3:
			var distance := point.distance_to(_polygon_centre(polygon))
			if distance < nearest_distance:
				nearest_distance = distance
				nearest = id
	if nearest_distance <= HIT_RADIUS:
		return nearest
	return ""


func _polygon_centre(polygon: PackedVector2Array) -> Vector2:
	var sum := Vector2.ZERO
	for point in polygon:
		sum += point
	return sum / float(maxi(polygon.size(), 1))


func _short_id(id: String) -> String:
	match id:
		"eye_l":
			return "L"
		"eye_r":
			return "R"
		"mouth":
			return "M"
	return id


func _text(position: Vector2, text: String, colour: Color, size: int) -> void:
	draw_string(ThemeDB.fallback_font, position, text, HORIZONTAL_ALIGNMENT_LEFT, -1, size,
			colour)
