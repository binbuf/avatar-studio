extends Control

## Model pane overlay for the T25c Setup window.
##
## The final pane shows the avatar SubViewport. This overlay draws the three
## face-region cutouts on top of that image and lets the operator edit them in
## two ways:
##
## - **Draw** (when a region mode is active): drag a box on the model. The box
##   corners are unprojected onto the same face plane the decal shader uses and
##   become the region centre and size, exactly like drawing on the camera.
## - **Move / scale** (when no draw mode is active): click a cutout outline to
##   select and drag it, or drag the square handle at its bottom-right corner to
##   scale it about its centre.
##
## The projection is the same face plane the decal shader uses: the authored
## region centre plus the live FaceRig anchor is a canonical (u, v) point, which
## is transformed by the avatar's presentation transform and projected by the
## avatar camera. Dragging unprojects the pointer onto that plane.
##
## The OBS output never includes this overlay.

signal region_selected(id: String)
signal region_dragged(id: String, center: Vector2)
signal region_drawn(id: String, center: Vector2, size: Vector2)
signal region_resized(id: String, size: Vector2)

const REGION_IDS := ["eye_l", "eye_r", "mouth"]
const REGION_COLOURS := [
	Color(0.35, 0.85, 1.0, 1.0),
	Color(0.55, 0.95, 0.65, 1.0),
	Color(1.0, 0.60, 0.75, 1.0),
]
const ELLIPSE_STEPS := 48
const HIT_RADIUS := 26.0
const HANDLE_SIZE := 10.0

var face: Dictionary = {}
var regions: Array = []
var anchors: Array = []
var model: Node3D = null
var camera: Camera3D = null
var viewport_size := Vector2i(1920, 1080)
var selected_id := ""
var display_rect := Rect2()
var polygons: Dictionary = {}
var draw_mode := ""

var _dragging := false
var _drag_id := ""
var _drawing := false
var _draw_start := Vector2.ZERO
var _draw_current := Vector2.ZERO
var _resizing := false


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_STOP
	resized.connect(queue_redraw)


func set_data(face_values: Dictionary, region_list: Array, anchor_list: Array,
		model_node: Node3D, camera_node: Camera3D, target_size: Vector2i,
		selected: String) -> void:
	face = face_values
	regions = region_list
	anchors = anchor_list
	model = model_node
	camera = camera_node
	viewport_size = target_size
	selected_id = selected
	queue_redraw()


func set_selected(id: String) -> void:
	selected_id = id
	queue_redraw()


func get_selected() -> String:
	return selected_id


## "" keeps the move/scale interaction. A region id makes a drag draw a box.
func set_draw_mode(id: String) -> void:
	draw_mode = id if REGION_IDS.has(id) else ""
	_dragging = false
	_drawing = false
	_resizing = false
	queue_redraw()


func get_draw_mode() -> String:
	return draw_mode


func get_polygons() -> Dictionary:
	return polygons.duplicate(true)


## Projects a canonical face point to pane-space coordinates. Returns null when
## the model/camera is missing or the point is behind the camera.
func screen_from_canonical(center: Vector2) -> Variant:
	return _local_to_screen(_canonical_to_local(center.x, center.y))


## Unprojects a pane-space point onto the face plane and returns canonical
## (u, v) coordinates. Returns null when the ray misses the plane.
func canonical_from_screen(point: Vector2) -> Variant:
	if face.is_empty() or model == null or camera == null:
		return null
	if not is_instance_valid(model) or not is_instance_valid(camera):
		return null
	var rect := _display_rect()
	if rect.size.x <= 1.0 or rect.size.y <= 1.0:
		return null
	if not rect.has_point(point):
		return null
	var viewport_point := (point - rect.position) / rect.size * Vector2(viewport_size)
	var origin := camera.project_ray_origin(viewport_point)
	var direction := camera.project_ray_normal(viewport_point)
	var world_normal := (model.global_transform.basis * Vector3(face.normal)).normalized()
	var world_origin := model.global_transform * Vector3(face.origin)
	var denominator := direction.dot(world_normal)
	if absf(denominator) < 1e-6:
		return null
	var distance := (world_origin - origin).dot(world_normal) / denominator
	if distance < 0.0:
		return null
	var hit := origin + direction * distance
	var local := model.global_transform.affine_inverse() * hit
	var normal := Vector3(face.normal).normalized()
	var up := Vector3(face.up).normalized()
	var right := normal.cross(up).normalized()
	var delta := local - Vector3(face.origin)
	var scale := maxf(float(face.scale), 0.0001)
	return Vector2(delta.dot(right) / scale, delta.dot(up) / scale)


## Converts a pane-space box into a canonical centre/size by unprojecting its
## four corners. Returns {} when the face plane is not available.
func canonical_rect_from_screen_rect(screen_rect: Rect2) -> Dictionary:
	var rect := _display_rect()
	if rect.size.x <= 1.0 or rect.size.y <= 1.0:
		return {}
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
		var clamped := Vector2(
				clampf(corner.x, rect.position.x, rect.end.x),
				clampf(corner.y, rect.position.y, rect.end.y))
		var canonical: Variant = canonical_from_screen(clamped)
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


func _gui_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT:
		if event.pressed:
			var rect := _display_rect()
			if not rect.has_point(event.position):
				return
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
				_dragging = true
				_drag_id = id
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
		elif _resizing:
			_resizing = false
			accept_event()
		elif _dragging:
			_dragging = false
			_drag_id = ""
			accept_event()
	elif event is InputEventMouseMotion:
		if _drawing:
			_draw_current = event.position
			queue_redraw()
			accept_event()
		elif _resizing:
			_apply_resize(event.position)
			accept_event()
		elif _dragging:
			var canonical: Variant = canonical_from_screen(event.position)
			if canonical != null:
				region_dragged.emit(_drag_id, canonical)
			accept_event()


func _apply_resize(point: Vector2) -> void:
	if selected_id == "":
		return
	var index := REGION_IDS.find(selected_id)
	if index < 0 or index >= regions.size():
		return
	var canonical: Variant = canonical_from_screen(point)
	if canonical == null:
		return
	var region: Dictionary = regions[index]
	var centre: Vector2 = region.get("center", Vector2.ZERO)
	if index < anchors.size():
		centre += Vector2(anchors[index])
	var size := Vector2(absf(Vector2(canonical).x - centre.x) * 2.0,
			absf(Vector2(canonical).y - centre.y) * 2.0)
	size = Vector2(maxf(size.x, 0.02), maxf(size.y, 0.02))
	region_resized.emit(selected_id, size)


func _draw() -> void:
	display_rect = _display_rect()
	polygons = {}
	if face.is_empty() or regions.is_empty():
		return
	if model == null or camera == null:
		return
	if not is_instance_valid(model) or not is_instance_valid(camera):
		return
	if display_rect.size.x <= 1.0 or display_rect.size.y <= 1.0:
		return

	draw_rect(display_rect, Color(1.0, 1.0, 1.0, 0.10), false, 1.0)

	for index in range(regions.size()):
		var region: Dictionary = regions[index]
		var id := String(region.get("id", ""))
		var centre: Vector2 = region.get("center", Vector2.ZERO)
		if index < anchors.size():
			centre += Vector2(anchors[index])
		var size: Vector2 = region.get("size", Vector2(0.1, 0.1))
		var rotation := deg_to_rad(float(region.get("rotation_deg", 0.0)))
		var shape := String(region.get("shape", "ellipse"))
		var polygon := _region_polygon(centre, size, rotation, shape)
		if polygon.size() < 3:
			continue
		polygons[id] = polygon
		var selected := id == selected_id
		var colour: Color = REGION_COLOURS[index % REGION_COLOURS.size()]
		if selected:
			colour = colour.lightened(0.25)
		draw_colored_polygon(polygon, colour * Color(1, 1, 1, 0.12 if selected else 0.06))
		for point_index in range(polygon.size()):
			draw_line(polygon[point_index], polygon[(point_index + 1) % polygon.size()],
					colour, 2.5 if selected else 1.5)
		var centre_screen: Variant = screen_from_canonical(centre)
		if centre_screen != null:
			draw_circle(centre_screen, 4.0 if selected else 3.0, colour)
		draw_string(ThemeDB.fallback_font, polygon[0] + Vector2(4, -4),
				_short_id(id), HORIZONTAL_ALIGNMENT_LEFT, -1, 11, colour)
		if selected:
			_draw_handle(colour)

	_draw_preview()


func _draw_handle(colour: Color) -> void:
	var handle := _handle_rect()
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
	var polygon: PackedVector2Array = polygons[selected_id]
	if polygon.size() < 3:
		return Rect2()
	var bounds := Rect2(polygon[0], Vector2.ZERO)
	for point in polygon:
		bounds = bounds.expand(point)
	return Rect2(bounds.end - Vector2(HANDLE_SIZE, HANDLE_SIZE),
			Vector2(HANDLE_SIZE, HANDLE_SIZE))


func _display_rect() -> Rect2:
	var area := Rect2(Vector2.ZERO, size)
	if viewport_size.x <= 0 or viewport_size.y <= 0:
		return area
	if area.size.x <= 1.0 or area.size.y <= 1.0:
		return area
	var viewport_aspect := float(viewport_size.x) / float(viewport_size.y)
	var result := area.size
	if result.x / result.y > viewport_aspect:
		result.x = result.y * viewport_aspect
	else:
		result.y = result.x / viewport_aspect
	return Rect2(area.position + (area.size - result) * 0.5, result)


func _canonical_to_local(u: float, v: float) -> Vector3:
	var normal := Vector3(face.normal).normalized()
	var up := Vector3(face.up).normalized()
	var right := normal.cross(up).normalized()
	return Vector3(face.origin) + (right * u + up * v) * float(face.scale)


func _local_to_screen(local: Vector3) -> Variant:
	if model == null or camera == null:
		return null
	if not is_instance_valid(model) or not is_instance_valid(camera):
		return null
	var rect := _display_rect()
	if rect.size.x <= 1.0 or rect.size.y <= 1.0:
		return null
	var world := model.global_transform * local
	if camera.is_position_behind(world):
		return null
	var viewport_point := camera.unproject_position(world)
	return rect.position + (viewport_point / Vector2(viewport_size)) * rect.size


func _region_polygon(centre: Vector2, size: Vector2, rotation: float,
		shape: String) -> PackedVector2Array:
	var out := PackedVector2Array()
	if size.x <= 0.0 or size.y <= 0.0:
		return out
	var local_points: Array = []
	if shape == "ellipse":
		for step in range(ELLIPSE_STEPS):
			var angle := TAU * float(step) / float(ELLIPSE_STEPS)
			local_points.append(Vector2(cos(angle) * size.x * 0.5,
					sin(angle) * size.y * 0.5))
	else:
		var half := Vector2(size.x * 0.5, size.y * 0.5)
		local_points = [Vector2(-half.x, -half.y), Vector2(half.x, -half.y),
				Vector2(half.x, half.y), Vector2(-half.x, half.y)]
	for point in local_points:
		var canonical: Vector2 = centre + (point as Vector2).rotated(rotation)
		var projected: Variant = _local_to_screen(_canonical_to_local(canonical.x,
				canonical.y))
		if projected == null:
			return PackedVector2Array()
		out.append(projected)
	return out


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
