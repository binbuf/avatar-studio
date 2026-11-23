extends Control

## Camera pane for the T25c Setup window.
##
## Draws the raw T24 camera slot with the same NV12 decode the decal uses, then
## an overlay child draws the live tracking data on top: landmarks, face box,
## the canonical-to-image homography quad and the three region anchors. This is
## a diagnostic view; the pixels never enter the avatar SubViewport or the OBS
## output.
##
## Direct manipulation (same contract as the mapping and model panes):
## - with a region mode active, drag a box to set that region's centre and size;
## - with no mode, drag a cutout outline to move it, or drag the square handle at
##   its bottom-right corner to scale it.
##
## The box/pointer is inverted through the live homography into canonical face
## coordinates. The setup window subtracts the FaceRig anchor and persists the
## authored region.

const SHADER_PATH := "res://shaders/camera_preview.gdshader"
const Overlay = preload("res://scenes/setup_camera_overlay.gd")
const REGION_IDS := ["eye_l", "eye_r", "mouth"]
const HIT_RADIUS := 26.0
const HANDLE_SIZE := 10.0

signal region_drawn(id: String, center: Vector2, size: Vector2)
signal region_selected(id: String)
signal region_dragged(id: String, center: Vector2)
signal region_resized(id: String, size: Vector2)

var preview_enabled := true
var has_frame := false
var landmark_count := 0
var anchor_count := 0
var face_box := Rect2()
var quad_points: PackedVector2Array = []
var image_rect := Rect2()
var draw_mode := ""
var region_outlines: Array = []

var _frame: ColorRect = null
var _overlay: Control = null
var _material: ShaderMaterial = null
var _aspect := 4.0 / 3.0
var _landmarks := PackedVector2Array()
var _anchors: Array = []
var _regions: Array = []
var _selected_id := ""
var _homography := PackedFloat32Array()
var _normalised_box := Rect2()
var _slot_key := ""
var _polygons: Dictionary = {}
var _have_mapping := false
var _mapping_mode := "live"

var _dragging := false
var _drag_start := Vector2.ZERO
var _drag_current := Vector2.ZERO
var _moving := false
var _move_id := ""
var _move_offset := Vector2.ZERO
var _resizing := false


func _ready() -> void:
	clip_contents = true
	mouse_filter = Control.MOUSE_FILTER_STOP
	_frame = ColorRect.new()
	_frame.name = "CameraImage"
	_frame.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_material = ShaderMaterial.new()
	_material.shader = load(SHADER_PATH)
	_frame.material = _material
	add_child(_frame)
	_overlay = Overlay.new()
	_overlay.name = "CameraOverlay"
	_overlay.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(_overlay)
	resized.connect(_layout)
	_layout()


func set_preview_enabled(enabled: bool) -> void:
	preview_enabled = enabled
	_apply_frame_visibility()
	_push_overlay()


## "" keeps the move/scale interaction. `eye_l`, `eye_r` or `mouth` makes a
## drag draw a box. Re-applying the current mode is a no-op so a selection
## signal (which syncs the mode buttons) cannot cancel an in-progress drag.
func set_draw_mode(id: String) -> void:
	var next := id if REGION_IDS.has(id) else ""
	if next == draw_mode:
		return
	draw_mode = next
	_dragging = false
	_moving = false
	_resizing = false
	_push_overlay()


func get_draw_mode() -> String:
	return draw_mode


## Rebinds the shader to one ring slot. Called every frame by the Setup window;
## texture uniforms only change when the slot or its generation changes.
func update_slot(slot: Dictionary) -> void:
	var valid := bool(slot.get("valid", false)) and preview_enabled
	has_frame = valid
	_apply_frame_visibility()
	if not valid:
		_push_overlay()
		return
	var width := int(slot.get("width", 0))
	var height := int(slot.get("height", 0))
	if width > 0 and height > 0:
		_aspect = float(width) / float(height)
	var key := "%d:%d:%d" % [int(slot.get("slot", -1)), int(slot.get("generation", 0)),
			int(slot.get("sequence", 0))]
	if key != _slot_key:
		_slot_key = key
		_material.set_shader_parameter("camera_is_yuv", bool(slot.get("is_yuv", false)))
		_material.set_shader_parameter("camera_matrix", int(slot.get("matrix", 0)))
		_material.set_shader_parameter("camera_range", int(slot.get("range", 0)))
		_material.set_shader_parameter("camera_bgra", bool(slot.get("bgra", false)))
		_material.set_shader_parameter("camera_tex", slot.get("rgb_texture", null))
		_material.set_shader_parameter("camera_y_tex", slot.get("y_texture", null))
		_material.set_shader_parameter("camera_uv_tex", slot.get("uv_texture", null))
	_layout()
	_push_overlay()


## Overlay data from one pipeline tick. `landmarks` are normalised image
## coordinates, `homography` is the row-major 3x3 used by the decal shader,
## `anchors` are the three absolute canonical centroids in eye_l/eye_r/mouth
## order, and `regions` are the effective regions (authored centre + FaceRig
## anchor) to draw and manipulate. `mapping_mode` is `live` while the rig is
## tracking, `held` while the last good mapping is held through a loss,
## `fallback` while the face box stands in for a missing solve, and `none` when
## there is nothing to project through.
func set_overlay(landmarks: PackedVector2Array, box: Rect2, homography: PackedFloat32Array,
		anchors: Array, regions: Array, selected_id: String, mapping_mode := "live") -> void:
	_landmarks = landmarks
	landmark_count = landmarks.size()
	_normalised_box = box
	face_box = box
	_homography = homography
	_anchors = anchors
	anchor_count = anchors.size()
	_regions = regions
	_selected_id = selected_id
	_mapping_mode = mapping_mode
	_have_mapping = mapping_mode != "none" and homography.size() == 9
	quad_points = _project_quad()
	_push_overlay()


## Converts a normalised image-space rect into the canonical face centre/size
## the decal uses. Returns {} when the homography is missing or not invertible.
func canonical_rect_from_image_rect(image_rect_norm: Rect2) -> Dictionary:
	if _homography.size() != 9:
		return {}
	var inverse := _invert_homography(_homography)
	if inverse.size() != 9:
		return {}
	var corners := [
		image_rect_norm.position,
		Vector2(image_rect_norm.end.x, image_rect_norm.position.y),
		image_rect_norm.end,
		Vector2(image_rect_norm.position.x, image_rect_norm.end.y),
	]
	var min_u := INF
	var max_u := -INF
	var min_v := INF
	var max_v := -INF
	for corner in corners:
		var canonical: Variant = _apply_homography(inverse, corner.x, corner.y)
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


## Converts a pane-space rect (the same coordinates `_gui_input` receives) into
## canonical face units. The rect is clamped to the visible camera image.
func canonical_rect_from_screen_rect(screen_rect: Rect2) -> Dictionary:
	if image_rect.size.x <= 1.0 or image_rect.size.y <= 1.0:
		return {}
	var position := (screen_rect.position - image_rect.position) / image_rect.size
	var size := screen_rect.size / image_rect.size
	var x0 := clampf(position.x, 0.0, 1.0)
	var y0 := clampf(position.y, 0.0, 1.0)
	var x1 := clampf(position.x + size.x, 0.0, 1.0)
	var y1 := clampf(position.y + size.y, 0.0, 1.0)
	if x1 - x0 <= 0.001 or y1 - y0 <= 0.001:
		return {}
	return canonical_rect_from_image_rect(Rect2(x0, y0, x1 - x0, y1 - y0))


## Unprojects a pane-space point through the inverse homography. Returns the
## effective canonical point, or null when the point is off the camera image.
func canonical_from_screen(point: Vector2) -> Variant:
	if _homography.size() != 9:
		return null
	if image_rect.size.x <= 1.0 or image_rect.size.y <= 1.0:
		return null
	if not image_rect.has_point(point):
		return null
	var inverse := _invert_homography(_homography)
	if inverse.size() != 9:
		return null
	var normalised := (point - image_rect.position) / image_rect.size
	return _apply_homography(inverse, normalised.x, normalised.y)


## Projects an effective canonical point back onto the camera pane.
func screen_from_canonical(center: Vector2) -> Variant:
	var projected: Variant = _apply_homography(_homography, center.x, center.y)
	if projected == null:
		return null
	return _to_screen(projected)


func get_region_polygons() -> Dictionary:
	return _polygons.duplicate(true)


## Canonical centre of an effective region, as the pane currently draws it.
func get_region_center(id: String) -> Variant:
	var index := REGION_IDS.find(id)
	if index < 0 or index >= _regions.size():
		return null
	return Vector2(_regions[index].get("center", Vector2.ZERO))


## Offset from the pointer to the region centre at grab time, so a drag moves
## the cutout with the pointer instead of snapping its centre under it.
func _move_offset_for(id: String, point: Vector2) -> Vector2:
	var centre: Variant = get_region_center(id)
	var canonical: Variant = canonical_from_screen(point)
	if centre == null or canonical == null:
		return Vector2.ZERO
	return Vector2(centre) - Vector2(canonical)


func _gui_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT:
		if event.pressed:
			if not image_rect.has_point(event.position):
				return
			if draw_mode != "":
				if not _have_mapping:
					return
				_dragging = true
				_drag_start = event.position
				_drag_current = event.position
				accept_event()
				return
			if _selected_id != "" and _handle_rect().has_point(event.position):
				_resizing = true
				accept_event()
				return
			var id := _hit_region(event.position)
			if id != "":
				_selected_id = id
				_moving = true
				_move_id = id
				_move_offset = _move_offset_for(id, event.position)
				region_selected.emit(id)
				_push_overlay()
				accept_event()
		elif _dragging:
			_dragging = false
			var rect := _drag_rect()
			_push_overlay()
			accept_event()
			if rect.size.x >= 4.0 and rect.size.y >= 4.0:
				_emit_region(rect)
		elif _moving:
			_moving = false
			_move_id = ""
			accept_event()
		elif _resizing:
			_resizing = false
			accept_event()
	elif event is InputEventMouseMotion:
		if _dragging:
			_drag_current = event.position
			_push_overlay()
			accept_event()
		elif _moving:
			var canonical: Variant = canonical_from_screen(event.position)
			if canonical != null:
				region_dragged.emit(_move_id, Vector2(canonical) + _move_offset)
			accept_event()
		elif _resizing:
			_apply_resize(event.position)
			accept_event()


func _emit_region(screen_rect: Rect2) -> void:
	var result := canonical_rect_from_screen_rect(screen_rect)
	if result.is_empty():
		return
	region_drawn.emit(draw_mode, Vector2(result.center), Vector2(result.size))


func _apply_resize(point: Vector2) -> void:
	if _selected_id == "":
		return
	var index := REGION_IDS.find(_selected_id)
	if index < 0 or index >= _regions.size():
		return
	var canonical: Variant = canonical_from_screen(point)
	if canonical == null:
		return
	var centre: Vector2 = _regions[index].get("center", Vector2.ZERO)
	var size := Vector2(absf(Vector2(canonical).x - centre.x) * 2.0,
			absf(Vector2(canonical).y - centre.y) * 2.0)
	size = Vector2(maxf(size.x, 0.02), maxf(size.y, 0.02))
	region_resized.emit(_selected_id, size)


func _drag_rect() -> Rect2:
	if _drag_start == _drag_current:
		return Rect2()
	var position := Vector2(minf(_drag_start.x, _drag_current.x),
			minf(_drag_start.y, _drag_current.y))
	var size := Vector2(absf(_drag_current.x - _drag_start.x),
			absf(_drag_current.y - _drag_start.y))
	return Rect2(position, size)


func _apply_frame_visibility() -> void:
	if _material != null:
		_material.set_shader_parameter("has_frame", has_frame)
	if _frame != null:
		_frame.visible = has_frame


func _layout() -> void:
	if _frame == null:
		return
	var area := get_rect()
	if area.size.x <= 1.0 or area.size.y <= 1.0:
		return
	var image_size := Vector2(area.size.x, area.size.x / maxf(_aspect, 0.01))
	if image_size.y > area.size.y:
		image_size = Vector2(area.size.y * _aspect, area.size.y)
	var offset := (area.size - image_size) * 0.5
	image_rect = Rect2(area.position + offset, image_size)
	_frame.position = image_rect.position
	_frame.size = image_rect.size
	_push_overlay()


func _to_screen(point: Vector2) -> Vector2:
	return image_rect.position + Vector2(point.x * image_rect.size.x,
			point.y * image_rect.size.y)


func _project(u: float, v: float) -> Variant:
	return _apply_homography(_homography, u, v)


func _project_quad() -> PackedVector2Array:
	var points := PackedVector2Array()
	if _homography.size() != 9:
		return points
	for corner in [Vector2(-1.0, -1.0), Vector2(1.0, -1.0), Vector2(1.0, 1.0),
			Vector2(-1.0, 1.0)]:
		var projected: Variant = _project(corner.x, corner.y)
		if projected == null:
			return PackedVector2Array()
		points.append(_to_screen(projected))
	return points


func _project_regions() -> Array:
	var out: Array = []
	_polygons = {}
	if _homography.size() != 9 or _regions.is_empty():
		return out
	for index in range(_regions.size()):
		var region: Dictionary = _regions[index]
		var id := String(region.get("id", ""))
		var centre: Vector2 = region.get("center", Vector2.ZERO)
		var size: Vector2 = region.get("size", Vector2(0.1, 0.1))
		var rotation := deg_to_rad(float(region.get("rotation_deg", 0.0)))
		var shape := String(region.get("shape", "ellipse"))
		var points := _project_region(centre, size, rotation, shape)
		if points.size() < 3:
			continue
		_polygons[id] = points
		out.append({
			"id": id,
			"points": points,
			"selected": id == _selected_id,
			"colour_index": index,
		})
	return out


func _project_region(centre: Vector2, size: Vector2, rotation: float,
		shape: String) -> PackedVector2Array:
	var out := PackedVector2Array()
	var local_points: Array = []
	if shape == "ellipse":
		for step in range(40):
			var angle := TAU * float(step) / 40.0
			local_points.append(Vector2(cos(angle) * size.x * 0.5,
					sin(angle) * size.y * 0.5))
	else:
		local_points = [Vector2(-0.5, -0.5), Vector2(0.5, -0.5), Vector2(0.5, 0.5),
				Vector2(-0.5, 0.5)]
	for point in local_points:
		var local: Vector2 = point
		if shape != "ellipse":
			local = Vector2(local.x * size.x, local.y * size.y)
		var canonical: Vector2 = centre + local.rotated(rotation)
		var projected: Variant = _apply_homography(_homography, canonical.x, canonical.y)
		if projected == null:
			return PackedVector2Array()
		out.append(_to_screen(projected))
	return out


func _handle_rect() -> Rect2:
	if _selected_id == "" or not _polygons.has(_selected_id):
		return Rect2()
	var polygon: PackedVector2Array = _polygons[_selected_id]
	if polygon.size() < 3:
		return Rect2()
	var bounds := Rect2(polygon[0], Vector2.ZERO)
	for point in polygon:
		bounds = bounds.expand(point)
	return Rect2(bounds.end - Vector2(HANDLE_SIZE, HANDLE_SIZE),
			Vector2(HANDLE_SIZE, HANDLE_SIZE))


func _hit_region(point: Vector2) -> String:
	var nearest := ""
	var nearest_distance := INF
	for id in _polygons.keys():
		var polygon: PackedVector2Array = _polygons[id]
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


func _push_overlay() -> void:
	if _overlay == null:
		return
	var anchor_points: Array = []
	var names := ["eye L", "eye R", "mouth"]
	if _have_mapping:
		for index in range(mini(_anchors.size(), names.size())):
			var anchor: Vector2 = _anchors[index]
			var projected: Variant = _project(anchor.x, anchor.y)
			if projected == null:
				continue
			anchor_points.append({"position": _to_screen(projected), "label": names[index]})
	region_outlines = _project_regions() if _have_mapping else []
	_overlay.call("set_overlay", image_rect, _landmarks, _normalised_box, quad_points,
			anchor_points, has_frame, preview_enabled, region_outlines,
			_drag_rect(), draw_mode, _selected_id, _mapping_mode)


## Inverts a row-major 3x3 homography. Returns an empty array when singular.
static func _invert_homography(h: PackedFloat32Array) -> PackedFloat32Array:
	if h.size() != 9:
		return PackedFloat32Array()
	var a := h[0]
	var b := h[1]
	var c := h[2]
	var d := h[3]
	var e := h[4]
	var f := h[5]
	var g := h[6]
	var i := h[7]
	var j := h[8]
	var determinant := a * (e * j - f * i) - b * (d * j - f * g) + c * (d * i - e * g)
	if absf(determinant) < 1e-12:
		return PackedFloat32Array()
	var inverse := PackedFloat32Array()
	inverse.resize(9)
	inverse[0] = (e * j - f * i) / determinant
	inverse[1] = (c * i - b * j) / determinant
	inverse[2] = (b * f - c * e) / determinant
	inverse[3] = (f * g - d * j) / determinant
	inverse[4] = (a * j - c * g) / determinant
	inverse[5] = (c * d - a * f) / determinant
	inverse[6] = (d * i - e * g) / determinant
	inverse[7] = (b * g - a * i) / determinant
	inverse[8] = (a * e - b * d) / determinant
	return inverse


## Applies a row-major 3x3 homography to (x, y). Returns null on a degenerate w.
static func _apply_homography(h: PackedFloat32Array, x: float, y: float) -> Variant:
	if h.size() != 9:
		return null
	var w := h[6] * x + h[7] * y + h[8]
	if absf(w) < 1e-9:
		return null
	var px := (h[0] * x + h[1] * y + h[2]) / w
	var py := (h[3] * x + h[4] * y + h[5]) / w
	if not is_finite(px) or not is_finite(py):
		return null
	return Vector2(px, py)
