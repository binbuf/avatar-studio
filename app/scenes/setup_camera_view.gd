extends Control

## Camera pane for the T25c Setup window.
##
## Draws the raw T24 camera slot with the same NV12 decode the decal uses, then
## an overlay child draws the live tracking data on top: landmarks, face box,
## the canonical-to-image homography quad and the three region anchors. This is
## a diagnostic view; the pixels never enter the avatar SubViewport or the OBS
## output.

const SHADER_PATH := "res://shaders/camera_preview.gdshader"
const Overlay = preload("res://scenes/setup_camera_overlay.gd")

var preview_enabled := true
var has_frame := false
var landmark_count := 0
var anchor_count := 0
var face_box := Rect2()
var quad_points: PackedVector2Array = []
var image_rect := Rect2()

var _frame: ColorRect = null
var _overlay: Control = null
var _material: ShaderMaterial = null
var _aspect := 4.0 / 3.0
var _landmarks := PackedVector2Array()
var _anchors: Array = []
var _homography := PackedFloat32Array()
var _normalised_box := Rect2()
var _slot_key := ""


func _ready() -> void:
	clip_contents = true
	mouse_filter = Control.MOUSE_FILTER_IGNORE
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
## coordinates, `homography` is the row-major 3x3 used by the decal shader, and
## `anchors` are the three absolute canonical centroids in eye_l/eye_r/mouth
## order.
func set_overlay(landmarks: PackedVector2Array, box: Rect2, homography: PackedFloat32Array,
		anchors: Array) -> void:
	_landmarks = landmarks
	landmark_count = landmarks.size()
	_normalised_box = box
	face_box = box
	_homography = homography
	_anchors = anchors
	anchor_count = anchors.size()
	quad_points = _project_quad()
	_push_overlay()


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
	return image_rect.position + Vector2(point.x * image_rect.size.x, point.y * image_rect.size.y)


func _project(u: float, v: float) -> Variant:
	if _homography.size() != 9:
		return null
	var w := _homography[6] * u + _homography[7] * v + _homography[8]
	if absf(w) < 1e-9:
		return null
	var x := (_homography[0] * u + _homography[1] * v + _homography[2]) / w
	var y := (_homography[3] * u + _homography[4] * v + _homography[5]) / w
	if not is_finite(x) or not is_finite(y):
		return null
	return Vector2(x, y)


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


func _push_overlay() -> void:
	if _overlay == null:
		return
	var anchor_points: Array = []
	var names := ["eye L", "eye R", "mouth"]
	for index in range(mini(_anchors.size(), names.size())):
		var anchor: Vector2 = _anchors[index]
		var projected: Variant = _project(anchor.x, anchor.y)
		if projected == null:
			continue
		anchor_points.append({"position": _to_screen(projected), "label": names[index]})
	_overlay.call("set_overlay", image_rect, _landmarks, _normalised_box, quad_points,
			anchor_points, has_frame, preview_enabled)
