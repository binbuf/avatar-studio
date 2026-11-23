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

const REGION_IDS := ["eye_l", "eye_r", "mouth"]

var pipeline: Object = null
var ring: Object = null
var decal = null
var model: Node3D = null
var face: Dictionary = {}
var last_state: Dictionary = {}
var last_error := ""
var running := false
var using_surface := true
var decal_visible := true


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
	if runtime != null:
		model = runtime.call("get_avatar_model")
	if model != null and is_instance_valid(model):
		_build_decal()
	if not bool(pipeline.call("initialize", options)):
		last_error = String(pipeline.call("get_last_error"))
		return false
	last_error = ""
	return true


func _build_decal() -> void:
	var aabb: AABB = model.call("get_model_aabb")
	if aabb.size == Vector3.ZERO:
		last_error = "avatar has no bounds"
		return
	face = Defaults.make_face(aabb)
	model.call("configure_head_region", Defaults.make_head_region(face), face, null)
	var surface = Surface.new()
	if surface.build(model, face, Defaults.make_regions()):
		decal = surface
		using_surface = true
		return
	var flat = Flat.new()
	if flat.build(model, face, Defaults.make_regions()):
		decal = flat
		using_surface = false
		return
	last_error = String(surface.get_last_error())


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
	var face_state := String(state.get("face", "unavailable"))
	var show := bool(state.get("features_valid", false)) \
			and (face_state == "tracked" or face_state == "degraded")
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
	for index in range(mini(regions.size(), REGION_IDS.size())):
		decal.set_region_anchor(REGION_IDS[index], regions[index])
	var head: PackedFloat32Array = state.get("head_from_camera", PackedFloat32Array())
	if model != null and is_instance_valid(model) and head.size() == 16:
		model.call("set_head_rotation", _head_rotation(head))


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
