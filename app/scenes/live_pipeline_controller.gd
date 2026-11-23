extends RefCounted

## M1 live pipeline controller (T26).
##
## Owns the native `AvatarLivePipeline`, the T24 camera ring and the decal pass
## built on the runtime's avatar. It feeds the real capture/tracker/FaceRig
## values into the Live session; the HUD owns the words and the output gate.
##
## The decal is display-only: the output path still samples the single avatar
## SubViewport, and the output gate decides blank versus avatar.

const Surface = preload("res://scenes/face_decal_surface.gd")
const Flat = preload("res://scenes/face_decal_flat.gd")
const Defaults = preload("res://scenes/face_calibration_defaults.gd")
const Calibration = preload("res://scenes/live_calibration.gd")

const REGION_IDS := ["eye_l", "eye_r", "mouth"]

var pipeline: Object = null
var ring: Object = null
var decal = null
var model: Node3D = null
var face: Dictionary = {}
var calibration: Dictionary = {}
var calibration_path := Calibration.PATH
var last_state: Dictionary = {}
var last_error := ""
var running := false
var using_surface := true
var decal_visible := true
var preview_enabled := false
var options: Dictionary = {}
var base_options: Dictionary = {}
var region_rest: Array = []



func setup(runtime: Node, options: Dictionary = {}) -> bool:
	pipeline = ClassDB.instantiate("AvatarLivePipeline")
	if pipeline == null:
		last_error = "AvatarLivePipeline is not registered"
		return false
	ring = ClassDB.instantiate("AvatarCameraRing")
	if ring == null:
		last_error = "AvatarCameraRing is not registered"
		return false
	if not bool(ring.call("initialize", 4)):
		last_error = String(ring.call("get_last_error"))
		return false
	pipeline.call("set_camera_ring", ring)
	base_options = options.duplicate(true)
	self.options = options.duplicate(true)
	if runtime != null:
		model = runtime.call("get_avatar_model")
	if model != null and is_instance_valid(model):
		_build_decal()
	if not bool(pipeline.call("initialize", options)):
		last_error = String(pipeline.call("get_last_error"))
		return false
	pipeline.call("set_preview_enabled", preview_enabled)
	last_error = ""
	return true


func _build_decal() -> void:
	var aabb: AABB = model.call("get_model_aabb")
	if aabb.size == Vector3.ZERO:
		last_error = "avatar has no bounds"
		return
	var values := Calibration.defaults(aabb)
	var loaded := Calibration.load_file(calibration_path)
	if bool(loaded.get("ok", false)):
		var saved := Calibration.normalise(loaded.get("values", {}), aabb)
		if saved.is_empty():
			last_error = "saved calibration rejected; using defaults"
		else:
			values = saved
	elif not bool(loaded.get("missing", false)):
		last_error = "saved calibration unreadable: " + String(loaded.get("error", ""))
	_build_from_values(values)


func _build_from_values(values: Dictionary) -> void:
	face = values.face
	if not bool(model.call("configure_head_region", values.head, face, null)):
		# A saved cut can be rejected if the avatar's up axis changed. Keep the
		# face mapping and derive a valid head cut rather than dropping the decal.
		var fallback: Dictionary = Defaults.make_head_region(face)
		if not bool(model.call("configure_head_region", fallback, face, null)):
			last_error = "head region rejected"
			return
		values.head = fallback
	var surface = Surface.new()
	if surface.build(model, face, values.regions):
		decal = surface
		using_surface = true
		calibration = values
		return
	var flat = Flat.new()
	if flat.build(model, face, values.regions):
		decal = flat
		using_surface = false
		calibration = values
		return
	last_error = String(surface.get_last_error())


## Applies a complete calibration to the live model and decal. `configure_head`
## is false for face/region-only edits so dragging an eye slider does not reset
## the head pose. Returns {ok, error}.
func apply_calibration(values: Dictionary, configure_head := true) -> Dictionary:
	if model == null or not is_instance_valid(model):
		return {"ok": false, "error": "no avatar model"}
	var aabb: AABB = model.call("get_model_aabb")
	var resolved := Calibration.normalise(values, aabb)
	if resolved.is_empty():
		return {"ok": false, "error": "calibration values are invalid"}
	if configure_head:
		if not bool(model.call("configure_head_region", resolved.head, resolved.face, null)):
			return {"ok": false, "error": "head region rejected"}
	face = resolved.face
	if decal != null:
		if not bool(decal.set_face(face)):
			return {"ok": false, "error": String(decal.get_last_error())}
		for entry in resolved.regions:
			var id := str(entry.id)
			if not bool(decal.set_region_geometry(id, entry.center, entry.size)):
				return {"ok": false, "error": String(decal.get_last_error())}
			decal.set_region_feather(id, float(entry.feather))
	calibration = resolved
	last_error = ""
	return {"ok": true, "error": ""}


## Persists the current calibration to the controller's path. The Setup window
## calls this only from Apply to model, never while a slider is moving.
func save_calibration() -> Dictionary:
	if calibration.is_empty():
		return {"ok": false, "path": calibration_path, "error": "no calibration to save"}
	return Calibration.save(calibration, calibration_path)


## Restores the AABB-derived provisional mapping and applies it live. It does
## not write the file; the operator presses Apply to persist the reset.
func reset_calibration() -> Dictionary:
	if model == null or not is_instance_valid(model):
		return {"ok": false, "error": "no avatar model", "values": {}}
	var aabb: AABB = model.call("get_model_aabb")
	var values := Calibration.defaults(aabb)
	var applied := apply_calibration(values, true)
	return {"ok": bool(applied.get("ok", false)), "error": String(applied.get("error", "")),
			"values": calibration.duplicate(true)}


## Re-reads the persisted mapping and rebuilds the decal. The test uses this to
## prove the restart path without a second process.
func reload_calibration() -> Dictionary:
	if model == null or not is_instance_valid(model):
		return {"ok": false, "error": "no avatar model"}
	if decal != null:
		decal.clear()
		decal = null
	_build_decal()
	return {"ok": not calibration.is_empty(), "error": last_error,
			"calibration": calibration.duplicate(true)}


func set_calibration_path(path: String) -> void:
	calibration_path = path


func set_preview_enabled(enabled: bool) -> void:
	preview_enabled = enabled
	if pipeline != null:
		pipeline.call("set_preview_enabled", enabled)


## Restarts the native pipeline with new capture/tracker options. The decal and
## calibration stay; only the camera source, tracker and ring binding restart.
## Used by the Setup window's capture-mode and tracker-input selectors.
func restart(new_options: Dictionary) -> Dictionary:
	if pipeline == null:
		return {"ok": false, "error": "pipeline is not initialized"}
	stop()
	pipeline.call("shutdown")
	options = new_options.duplicate(true)
	if not bool(pipeline.call("initialize", options)):
		last_error = String(pipeline.call("get_last_error"))
		return {"ok": false, "error": last_error}
	pipeline.call("set_camera_ring", ring)
	pipeline.call("set_preview_enabled", preview_enabled)
	if not start():
		return {"ok": false, "error": last_error}
	last_error = ""
	return {"ok": true, "error": ""}


func set_options(new_options: Dictionary) -> void:
	options = new_options.duplicate(true)


func get_options() -> Dictionary:
	return options.duplicate(true)



func reinitialize(next_options: Dictionary) -> bool:
	if pipeline == null:
		return false
	pipeline.call("stop")
	running = false
	options = next_options.duplicate(true)
	if not bool(pipeline.call("initialize", options)):
		last_error = String(pipeline.call("get_last_error"))
		return false
	pipeline.call("set_preview_enabled", preview_enabled)
	if not bool(pipeline.call("start")):
		last_error = String(pipeline.call("get_last_error"))
		return false
	running = true
	last_state = {}
	last_error = ""
	return true


func use_still(path: String) -> bool:
	var next := base_options.duplicate(true)
	next["source"] = "still"
	next["still_image"] = path
	return reinitialize(next)


func use_camera() -> bool:
	var next := base_options.duplicate(true)
	next["source"] = "camera"
	next["still_image"] = ""
	return reinitialize(next)


func start() -> bool:
	if pipeline == null:
		return false
	if not bool(pipeline.call("start")):
		last_error = String(pipeline.call("get_last_error"))
		running = false
		return false
	running = true
	return true


func stop() -> void:
	if pipeline != null:
		pipeline.call("stop")
	running = false


func tick() -> Dictionary:
	if pipeline == null or not running:
		return {}
	var state: Dictionary = pipeline.call("tick")
	last_state = state
	_apply_decal(state)
	return state


func _apply_decal(state: Dictionary) -> void:
	if decal == null:
		return
	var show := should_show_decal(state)
	if show != decal_visible:
		decal_visible = show
		for id in REGION_IDS:
			decal.set_region_enabled(id, show)
	if not show:
		return
	var slot: Dictionary = state.get("camera_slot", {})
	if bool(slot.get("valid", false)):
		decal.set_camera_slot(slot)
	var homography: PackedFloat32Array = state.get("canonical_to_image", PackedFloat32Array())
	if homography.size() == 9:
		decal.set_canonical_to_image(_basis_from_row_major(homography))
	var regions: Array = state.get("regions", [])
	region_rest = state.get("region_rest", region_rest)
	for index in range(mini(regions.size(), REGION_IDS.size())):
		# FaceRig publishes absolute canonical centroids; the shader adds the
		# anchor to the authored region centre, so pass the delta from rest.
		var anchor: Vector2 = regions[index]
		if index < region_rest.size():
			anchor -= region_rest[index]
		decal.set_region_anchor(REGION_IDS[index], anchor)
	var head: PackedFloat32Array = state.get("head_from_camera", PackedFloat32Array())
	if model != null and is_instance_valid(model) and head.size() == 16:
		model.call("set_head_rotation", _head_rotation(head))


## The setup decal follows the FaceRig hold: `features_valid` stays true for
## the 150 ms loss window even when the tracker already reports the face lost.
## The OBS gate keeps using the face state, so it blanks on the same loss while
## the diagnostic surface still shows the held pose. Tracker-unavailable never
## holds.
static func should_show_decal(state: Dictionary) -> bool:
	if state.is_empty():
		return false
	if not bool(state.get("tracker_available", true)):
		return false
	return bool(state.get("features_valid", false)) and bool(state.get("have_output", false))



## Row-major 3x3 canonical -> image homography as a Godot Basis. The shader
## evaluates `canonical_to_image * vec3(u, v, 1)`, so the Basis columns are the
## matrix columns.
static func _basis_from_row_major(h: PackedFloat32Array) -> Basis:
	return Basis(Vector3(h[0], h[3], h[6]), Vector3(h[1], h[4], h[7]),
			Vector3(h[2], h[5], h[8]))


## FaceRig publishes `head_from_camera` (the inverse pose). The avatar needs
## `camera_from_head`; for a rotation that is the transpose, which is exactly
## the stored matrix read as Godot Basis columns.
static func _head_rotation(m: PackedFloat32Array) -> Quaternion:
	var basis := Basis(Vector3(m[0], m[1], m[2]), Vector3(m[4], m[5], m[6]),
			Vector3(m[8], m[9], m[10]))
	if basis.determinant() <= 0.0:
		return Quaternion.IDENTITY
	return basis.get_rotation_quaternion()


func record_submit() -> void:
	if pipeline != null:
		pipeline.call("record_transport_submit")


func get_state() -> Dictionary:
	if pipeline == null:
		return {}
	return pipeline.call("get_state")


func get_calibration() -> Dictionary:
	return calibration.duplicate(true)


func get_region_rest() -> Array:
	return region_rest.duplicate()


## Dense tracker landmarks in normalised image coordinates, cached by the
## native adapter once per tracker result.
func get_landmarks() -> PackedVector2Array:
	if pipeline == null:
		return PackedVector2Array()
	return pipeline.call("get_landmarks")


func get_face_box() -> Rect2:
	if pipeline == null:
		return Rect2()
	return pipeline.call("get_face_box")


func get_preview_slot() -> Dictionary:
	return last_state.get("preview_slot", {})



func get_stats() -> Dictionary:
	if pipeline == null:
		return {}
	return pipeline.call("get_stats")


func get_config() -> Dictionary:
	if pipeline == null:
		return {}
	return pipeline.call("get_config")


func get_build_info() -> Dictionary:
	if pipeline == null:
		return {}
	return pipeline.call("get_build_info")


func reset_stats() -> void:
	if pipeline != null:
		pipeline.call("reset_stats")


func get_ring_stats() -> Dictionary:
	if ring == null:
		return {}
	return ring.call("get_stats")


func has_decal() -> bool:
	return decal != null


func is_surface_decal() -> bool:
	return decal != null and using_surface
