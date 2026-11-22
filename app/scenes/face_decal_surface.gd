class_name FaceDecalSurface
extends RefCounted
## Surface mode owns one AvatarModel shared-geometry decal pass. Calibration is
## in realised rest model space; presentation and head motion need no rebinding.
## Package decoding, camera/pose timestamp pairing and lighting belong to callers.
const Settings = preload("res://scenes/face_decal_flat.gd")
const SHADER_PATH := "res://shaders/face_decal_surface.gdshader"
var head: Node3D = null
var face: Dictionary = {}
var regions: Array[Dictionary] = []
var camera_texture: Texture2D = null
var canonical_to_image := Basis.IDENTITY
var material: ShaderMaterial = null
var _enabled: Dictionary = {}
var _last_error := ""

func build(model: Node3D, face_surface: Dictionary, region_list: Array) -> bool:
	if model == null or not is_instance_valid(model) or not model.has_method("create_decal_pass"):
		return _fail("AvatarModel required")
	var settings := Settings.new()
	var resolved_face: Dictionary = settings._normalise_face(face_surface)
	if resolved_face.is_empty():
		return _fail(settings.get_last_error())
	var resolved: Array[Dictionary] = []
	var seen: Dictionary = {}
	for raw in region_list:
		var region: Dictionary = settings._normalise_region(raw)
		if region.is_empty():
			return _fail(settings.get_last_error())
		if seen.has(region.id):
			return _fail("duplicate region id: " + str(region.id))
		seen[region.id] = true
		resolved.append(region)
	if resolved.is_empty():
		return _fail("at least one face region required")
	var next_material := ShaderMaterial.new()
	next_material.shader = load(SHADER_PATH)
	next_material.render_priority = 1
	# All validation happens before replacing the existing pass.
	if model.call("create_decal_pass", next_material) == null:
		return _fail("loaded AvatarModel with usable decal geometry required")
	if is_instance_valid(head) and head != model:
		head.call("clear_decal_pass")
	head = model
	face = resolved_face
	regions = resolved
	material = next_material
	_enabled.clear()
	for region in regions:
		_enabled[region.id] = true
	material.set_shader_parameter("face_origin", face.origin)
	material.set_shader_parameter("face_right", face.right)
	material.set_shader_parameter("face_up", face.up)
	material.set_shader_parameter("face_normal", face.normal)
	material.set_shader_parameter("face_scale", face.scale)
	set_camera_texture(camera_texture)
	set_canonical_to_image(canonical_to_image)
	_upload_regions()
	_last_error = ""
	return true

func clear() -> void:
	if is_instance_valid(head):
		head.call("clear_decal_pass")
	head = null
	material = null
	face = {}
	regions.clear()
	_enabled.clear()
	_last_error = ""

func is_built() -> bool:
	return is_instance_valid(head) and material != null

func get_last_error() -> String:
	return _last_error

func get_face() -> Dictionary:
	return face.duplicate(true)

func get_region(id: String) -> Dictionary:
	for region in regions:
		if region.id == id:
			return region
	return {}

func set_camera_texture(texture: Texture2D) -> void:
	camera_texture = texture
	if material != null:
		material.set_shader_parameter("camera_tex", texture)

func set_canonical_to_image(value: Basis) -> bool:
	if not value.x.is_finite() or not value.y.is_finite() or not value.z.is_finite() \
			or absf(value.determinant()) < 1e-12:
		return _fail("canonical_to_image must be finite and invertible")
	canonical_to_image = value
	if material != null:
		material.set_shader_parameter("canonical_to_image", value)
	return true

func set_region_anchor(id: String, anchor: Vector2) -> bool:
	var region := get_region(id)
	if region.is_empty() or not anchor.is_finite():
		return _fail("known region and finite anchor required")
	region.anchor = anchor
	_upload_regions()
	return true

func set_region_opacity(id: String, opacity: float) -> bool:
	var region := get_region(id)
	if region.is_empty() or not is_finite(opacity) or opacity < 0.0 or opacity > 1.0:
		return _fail("known region and opacity in [0, 1] required")
	region.opacity = opacity
	_upload_regions()
	return true

func set_region_enabled(id: String, enabled: bool) -> bool:
	if not _enabled.has(id):
		return _fail("unknown region: " + id)
	_enabled[id] = enabled
	_upload_regions()
	return true

func _upload_regions() -> void:
	if material == null:
		return
	var rects: Array[Vector4] = []
	var anchors: Array[Vector2] = []
	var styles: Array[Vector4] = []
	var gains: Array[Vector3] = []
	var lifts: Array[Vector3] = []
	var opacities: Array[float] = []
	for region in regions:
		rects.append(Vector4(region.center.x, region.center.y, region.size.x, region.size.y))
		anchors.append(region.anchor)
		styles.append(Vector4(region.rotation, region.feather, region.shape, region.corner_radius))
		gains.append(region.gain)
		lifts.append(region.lift)
		opacities.append(region.opacity if _enabled[region.id] else 0.0)
	material.set_shader_parameter("region_count", regions.size())
	# Fixed-size uniform arrays are always fully populated, including unused slots.
	while rects.size() < 3:
		rects.append(Vector4(0, 0, 1, 1))
		anchors.append(Vector2.ZERO)
		styles.append(Vector4.ZERO)
		gains.append(Vector3.ONE)
		lifts.append(Vector3.ZERO)
		opacities.append(0.0)
	material.set_shader_parameter("region_rect", rects)
	material.set_shader_parameter("region_anchor", anchors)
	material.set_shader_parameter("region_style", styles)
	material.set_shader_parameter("colour_gain", gains)
	material.set_shader_parameter("colour_lift", lifts)
	material.set_shader_parameter("region_opacity", opacities)

func _fail(message: String) -> bool:
	_last_error = message
	return false
