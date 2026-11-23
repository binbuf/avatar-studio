class_name FaceDecalFlat
extends RefCounted

## Flat face decal pass: three quads parented to the head, one per region.
##
## The quad geometry is placed on the calibrated face plane in head space. The
## shader projects each fragment back to canonical face space, masks the region,
## and samples the camera through the canonical-to-image homography. The camera
## sampler never wraps and the fragment is discarded outside the mask or the
## camera frame.
##
## This class owns the quads and their materials; the caller owns the head node
## and the camera texture. Rebuild with [method build] after changing the face
## surface or region set. Call [method refresh_transform] when the head's global
## transform changes (the quads follow their parent; the world-space face
## uniforms do not).
##
## The face surface is expressed in the head node's local space. Pass the model
## root when the calibration is in rest model space; passing a separate head
## subtree requires the face plane to be expressed in that subtree's space.
##
## Package decoding is not part of this class: region dictionaries are already
## in canonical units and colour gain/lift are realised values. The package
## colour transform (exposure/temperature/saturation/contrast) is T38/T39.

const SHADER_PATH := "res://shaders/face_decal_flat.gdshader"
const REGION_IDS: Array[String] = ["eye_l", "eye_r", "mouth"]
const DEFAULT_FEATHER := 0.18
const DEFAULT_CORNER_RADIUS := 0.35
const FACE_OFFSET_SCALE := 0.01

var head: Node3D = null
var face: Dictionary = {}
var regions: Array[Dictionary] = []
var camera_texture: Texture2D = null
var camera_y_texture: Texture2D = null
var camera_uv_texture: Texture2D = null
var camera_is_yuv := false
var camera_matrix := 0
var camera_range := 0
var camera_bgra := false
var canonical_to_image := Basis.IDENTITY

var _quads: Dictionary = {}
var _materials: Dictionary = {}
var _last_error := ""


func build(head_node: Node3D, face_surface: Dictionary, region_list: Array) -> bool:
	if head_node == null or not is_instance_valid(head_node):
		return _fail("head node is required")
	var resolved_face := _normalise_face(face_surface)
	if resolved_face.is_empty():
		return false
	var resolved_regions: Array[Dictionary] = []
	var seen: Dictionary = {}
	for raw in region_list:
		var region := _normalise_region(raw)
		if region.is_empty():
			return false
		if seen.has(region.id):
			return _fail("duplicate region id: " + str(region.id))
		seen[region.id] = true
		resolved_regions.append(region)
	if resolved_regions.is_empty():
		return _fail("at least one face region is required")
	clear()
	head = head_node
	face = resolved_face
	regions = resolved_regions
	if not _create_quads():
		var message := _last_error
		clear()
		_last_error = message
		return false
	return true


func clear() -> void:
	for id in _quads.keys():
		var quad: Node = _quads[id]
		if is_instance_valid(quad):
			quad.free()
	_quads.clear()
	_materials.clear()
	head = null
	face = {}
	regions = []
	_last_error = ""


func is_built() -> bool:
	return is_instance_valid(head) and not _quads.is_empty()


func get_last_error() -> String:
	return _last_error


func get_quad_ids() -> Array[String]:
	var ids: Array[String] = []
	for id in _quads.keys():
		ids.append(str(id))
	return ids


func get_quad(id: String) -> MeshInstance3D:
	return _quads.get(id) as MeshInstance3D


func get_material(id: String) -> ShaderMaterial:
	return _materials.get(id) as ShaderMaterial


func get_region(id: String) -> Dictionary:
	for region in regions:
		if region.id == id:
			return region
	return {}


func get_face() -> Dictionary:
	return face.duplicate(true)


## Replaces the calibrated face surface (T25c manual mapping). The flat pass
## parents one quad per region, so the quads are rebuilt from the same values.
func set_face(face_surface: Dictionary) -> bool:
	var resolved := _normalise_face(face_surface)
	if resolved.is_empty():
		return false
	face = resolved
	if head == null or not is_instance_valid(head):
		return _fail("head node is required")
	return _rebuild_quads()


## Moves/resizes one region in canonical face units and rebuilds that region's
## quad. Flat is the fallback pass; the surface pass updates uniforms only.
func set_region_geometry(id: String, center: Vector2, size: Vector2) -> bool:
	var region := get_region(id)
	if region.is_empty():
		return _fail("unknown region: " + id)
	if not center.is_finite() or not size.is_finite():
		return _fail("region geometry must be finite")
	if size.x <= 0.0 or size.y <= 0.0:
		return _fail("region size must be positive")
	region.center = center
	region.size = size
	return _rebuild_quads()


func set_region_feather(id: String, feather: float) -> bool:
	var region := get_region(id)
	if region.is_empty():
		return _fail("unknown region: " + id)
	if not is_finite(feather) or feather < 0.0 or feather > 1.0:
		return _fail("region feather must be in [0, 1]")
	region.feather = feather
	var material := get_material(id)
	if material != null:
		material.set_shader_parameter("feather", feather)
	return true


func _rebuild_quads() -> bool:
	for id in _quads.keys():
		var quad: Node = _quads[id]
		if is_instance_valid(quad):
			quad.free()
	_quads.clear()
	_materials.clear()
	return _create_quads()


func set_camera_texture(texture: Texture2D, bgra := false) -> void:
	camera_texture = texture
	camera_is_yuv = false
	camera_bgra = bgra
	for id in _materials.keys():
		_apply_camera_uniforms(_materials[id] as ShaderMaterial)


## Binds an NV12 ring slot: R8 luma and RG8 chroma. The shader converts YUV to
## RGB; the CPU never does a full-frame conversion. `color_space` accepts
## bt601_limited, bt601_full, bt709_limited and bt709_full.
func set_camera_planes(y_texture: Texture2D, uv_texture: Texture2D,
		color_space := "bt601_limited") -> bool:
	if y_texture == null or uv_texture == null:
		return _fail("camera planes are required")
	var encoding := _camera_encoding(color_space)
	if encoding.is_empty():
		return _fail("unknown camera colour space: " + color_space)
	camera_y_texture = y_texture
	camera_uv_texture = uv_texture
	camera_texture = null
	camera_is_yuv = true
	camera_bgra = false
	camera_matrix = int(encoding.matrix)
	camera_range = int(encoding.range)
	for id in _materials.keys():
		_apply_camera_uniforms(_materials[id] as ShaderMaterial)
	return true


## Binds the slot returned by AvatarCameraRing.select(). This is the
## timestamp-paired path: the caller passes the slot whose timestamp matches the
## FaceRig result, not the newest camera frame.
func set_camera_slot(slot: Dictionary) -> bool:
	if not bool(slot.get("valid", false)):
		return _fail("camera slot is not valid")
	camera_matrix = int(slot.get("matrix", 0))
	camera_range = int(slot.get("range", 0))
	if bool(slot.get("is_yuv", false)):
		var y_texture: Texture2D = slot.get("y_texture")
		var uv_texture: Texture2D = slot.get("uv_texture")
		if y_texture == null or uv_texture == null:
			return _fail("camera slot has no NV12 textures")
		camera_y_texture = y_texture
		camera_uv_texture = uv_texture
		camera_texture = null
		camera_is_yuv = true
		camera_bgra = false
	else:
		var texture: Texture2D = slot.get("rgb_texture")
		if texture == null:
			return _fail("camera slot has no RGB texture")
		camera_texture = texture
		camera_y_texture = null
		camera_uv_texture = null
		camera_is_yuv = false
		camera_bgra = bool(slot.get("bgra", false))
	for id in _materials.keys():
		_apply_camera_uniforms(_materials[id] as ShaderMaterial)
	return true


static func _camera_encoding(color_space: String) -> Dictionary:
	match color_space:
		"bt601_limited":
			return {"matrix": 0, "range": 0}
		"bt601_full":
			return {"matrix": 0, "range": 1}
		"bt709_limited":
			return {"matrix": 1, "range": 0}
		"bt709_full":
			return {"matrix": 1, "range": 1}
		"srgb":
			return {"matrix": 0, "range": 1}
	return {}


func _apply_camera_uniforms(material: ShaderMaterial) -> void:
	material.set_shader_parameter("camera_tex", camera_texture)
	material.set_shader_parameter("camera_y_tex", camera_y_texture)
	material.set_shader_parameter("camera_uv_tex", camera_uv_texture)
	material.set_shader_parameter("camera_is_yuv", camera_is_yuv)
	material.set_shader_parameter("camera_matrix", camera_matrix)
	material.set_shader_parameter("camera_range", camera_range)
	material.set_shader_parameter("camera_bgra", camera_bgra)


func set_canonical_to_image(homography: Basis) -> bool:
	if not _is_finite_basis(homography):
		return _fail("canonical_to_image must be finite")
	if absf(homography.determinant()) < 1e-12:
		return _fail("canonical_to_image must be invertible")
	canonical_to_image = homography
	for id in _materials.keys():
		(_materials[id] as ShaderMaterial).set_shader_parameter(
				"canonical_to_image", canonical_to_image)
	return true


func set_region_anchor(id: String, anchor: Vector2) -> bool:
	if not anchor.is_finite():
		return _fail("region anchor must be finite")
	var region := get_region(id)
	if region.is_empty():
		return _fail("unknown region: " + id)
	region.anchor = anchor
	var material := get_material(id)
	if material != null:
		material.set_shader_parameter("region_anchor", anchor)
	return true


func set_region_opacity(id: String, opacity: float) -> bool:
	if not is_finite(opacity) or opacity < 0.0 or opacity > 1.0:
		return _fail("region opacity must be in [0, 1]")
	var region := get_region(id)
	if region.is_empty():
		return _fail("unknown region: " + id)
	region.opacity = opacity
	var material := get_material(id)
	if material != null:
		material.set_shader_parameter("opacity", opacity)
	return true


func set_region_enabled(id: String, enabled: bool) -> bool:
	var quad := get_quad(id)
	if quad == null:
		return _fail("unknown region: " + id)
	quad.visible = enabled
	return true


func refresh_transform() -> bool:
	if head == null or not is_instance_valid(head):
		return _fail("head node is required")
	var world := _world_face()
	if world.is_empty():
		return false
	for id in _materials.keys():
		var material := _materials[id] as ShaderMaterial
		material.set_shader_parameter("face_origin", world.origin)
		material.set_shader_parameter("face_right", world.right)
		material.set_shader_parameter("face_up", world.up)
		material.set_shader_parameter("face_scale", world.scale)
	return true


func _create_quads() -> bool:
	var world := _world_face()
	if world.is_empty():
		return false
	var shader := load(SHADER_PATH) as Shader
	if shader == null:
		return _fail("missing shader " + SHADER_PATH)
	var offset: float = maxf(float(world.scale) * FACE_OFFSET_SCALE, 0.0001)
	var world_right: Vector3 = world.right
	var world_up: Vector3 = world.up
	var world_origin: Vector3 = world.origin
	var world_normal: Vector3 = world.normal
	var world_scale: float = float(world.scale)
	for region in regions:
		var rotation: float = region.rotation
		var x_axis: Vector3 = (world_right * cos(rotation) + world_up * sin(rotation)).normalized()
		var y_axis: Vector3 = (-world_right * sin(rotation) + world_up * cos(rotation)).normalized()
		var centre: Vector3 = world_origin \
				+ (world_right * region.center.x + world_up * region.center.y) * world_scale \
				+ world_normal * offset

		var mesh := QuadMesh.new()
		mesh.size = Vector2(region.size.x, region.size.y) * world_scale

		var material := ShaderMaterial.new()
		material.shader = shader
		material.render_priority = 1
		_apply_camera_uniforms(material)
		material.set_shader_parameter("canonical_to_image", canonical_to_image)
		material.set_shader_parameter("region_uv", Vector4(
				region.center.x, region.center.y, region.size.x, region.size.y))
		material.set_shader_parameter("region_anchor", region.anchor)
		material.set_shader_parameter("region_rot", rotation)
		material.set_shader_parameter("feather", region.feather)
		material.set_shader_parameter("region_shape", region.shape)
		material.set_shader_parameter("corner_radius", region.corner_radius)
		material.set_shader_parameter("colour_gain", region.gain)
		material.set_shader_parameter("colour_lift", region.lift)
		material.set_shader_parameter("opacity", region.opacity)
		material.set_shader_parameter("face_origin", world_origin)
		material.set_shader_parameter("face_right", world_right)
		material.set_shader_parameter("face_up", world_up)
		material.set_shader_parameter("face_scale", world_scale)

		var quad := MeshInstance3D.new()
		quad.name = "FaceDecalFlat_" + str(region.id)
		quad.mesh = mesh
		quad.material_override = material
		quad.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		quad.extra_cull_margin = 0.05
		head.add_child(quad)
		quad.global_transform = Transform3D(Basis(x_axis, y_axis, world_normal), centre)
		_quads[region.id] = quad
		_materials[region.id] = material
	return true


func _world_face() -> Dictionary:
	if head == null or not is_instance_valid(head):
		_fail("head node is required")
		return {}
	if face.is_empty():
		_fail("face surface is required")
		return {}
	var transform := head.global_transform
	var scale: float = float(face.scale) * _uniform_scale(transform.basis)
	if not is_finite(scale) or scale <= 0.0:
		_fail("head transform has no usable scale")
		return {}
	return {
		"origin": transform * face.origin,
		"right": (transform.basis * face.right).normalized(),
		"up": (transform.basis * face.up).normalized(),
		"normal": (transform.basis * face.normal).normalized(),
		"scale": scale,
	}


func _normalise_face(raw: Dictionary) -> Dictionary:
	if raw.is_empty():
		_fail("face surface is required")
		return {}
	var origin: Variant = _as_vec3(raw.get("origin", null))
	var normal: Variant = _as_vec3(raw.get("normal", null))
	var up: Variant = _as_vec3(raw.get("up", null))
	var scale: Variant = raw.get("scale", null)
	if origin == null or normal == null or up == null:
		_fail("face origin, normal and up are required")
		return {}
	if not (origin as Vector3).is_finite() or not (normal as Vector3).is_finite() \
			or not (up as Vector3).is_finite():
		_fail("face vectors must be finite")
		return {}
	if not (scale is float or scale is int) or not is_finite(float(scale)):
		_fail("face scale must be a finite number")
		return {}
	if float(scale) <= 0.0:
		_fail("face scale must be positive")
		return {}
	var n := (normal as Vector3).normalized()
	var u := (up as Vector3).normalized()
	if n.is_zero_approx() or u.is_zero_approx():
		_fail("face normal and up must be non-zero")
		return {}
	var right := n.cross(u)
	if right.length() < 1e-6:
		_fail("face up must not be parallel to the normal")
		return {}
	right = right.normalized()
	u = right.cross(n).normalized()
	return {
		"origin": origin as Vector3,
		"normal": n,
		"up": u,
		"right": right,
		"scale": float(scale),
	}


func _normalise_region(raw: Variant) -> Dictionary:
	if not (raw is Dictionary):
		_fail("region must be a dictionary")
		return {}
	var source: Dictionary = raw
	var id := str(source.get("id", ""))
	if not REGION_IDS.has(id):
		_fail("unknown region id: " + id)
		return {}
	var centre: Variant = _as_vec2(source.get("center", null))
	var size: Variant = _as_vec2(source.get("size", null))
	if centre == null or size == null:
		_fail("region %s needs centre and size" % id)
		return {}
	if not (centre as Vector2).is_finite() or not (size as Vector2).is_finite():
		_fail("region %s geometry must be finite" % id)
		return {}
	if (size as Vector2).x <= 0.0 or (size as Vector2).y <= 0.0:
		_fail("region %s size must be positive" % id)
		return {}
	var rotation_deg := float(source.get("rotation_deg", 0.0))
	var rotation := float(source.get("rotation", deg_to_rad(rotation_deg)))
	if not is_finite(rotation):
		_fail("region %s rotation must be finite" % id)
		return {}
	var feather := float(source.get("feather", DEFAULT_FEATHER))
	if not is_finite(feather) or feather < 0.0 or feather > 1.0:
		_fail("region %s feather must be in [0, 1]" % id)
		return {}
	var opacity := float(source.get("opacity", 1.0))
	if not is_finite(opacity) or opacity < 0.0 or opacity > 1.0:
		_fail("region %s opacity must be in [0, 1]" % id)
		return {}
	var shape_value: Variant = source.get("shape", "ellipse")
	var shape := 0
	if shape_value is int:
		shape = shape_value
	elif shape_value is String:
		match str(shape_value):
			"ellipse":
				shape = 0
			"rounded_rect":
				shape = 1
			_:
				_fail("region %s has unknown shape %s" % [id, shape_value])
				return {}
	else:
		_fail("region %s has an invalid shape" % id)
		return {}
	if shape != 0 and shape != 1:
		_fail("region %s shape must be ellipse or rounded_rect" % id)
		return {}
	var corner_radius := float(source.get("corner_radius", DEFAULT_CORNER_RADIUS))
	if not is_finite(corner_radius) or corner_radius < 0.0 or corner_radius > 1.0:
		_fail("region %s corner radius must be in [0, 1]" % id)
		return {}
	var anchor: Variant = _as_vec2(source.get("anchor", Vector2.ZERO))
	if anchor == null or not (anchor as Vector2).is_finite():
		_fail("region %s anchor must be finite" % id)
		return {}
	var gain: Variant = _as_vec3(source.get("gain", Vector3.ONE))
	var lift: Variant = _as_vec3(source.get("lift", Vector3.ZERO))
	if gain == null or lift == null:
		_fail("region %s colour gain/lift must be vectors" % id)
		return {}
	if not (gain as Vector3).is_finite() or not (lift as Vector3).is_finite():
		_fail("region %s colour gain/lift must be finite" % id)
		return {}
	return {
		"id": id,
		"center": centre,
		"size": size,
		"rotation": rotation,
		"feather": feather,
		"opacity": opacity,
		"shape": shape,
		"corner_radius": corner_radius,
		"anchor": anchor,
		"gain": gain,
		"lift": lift,
	}


func _as_vec2(value: Variant) -> Variant:
	if value is Vector2:
		return value
	if value is Array and (value as Array).size() >= 2:
		return Vector2(float(value[0]), float(value[1]))
	return null


func _as_vec3(value: Variant) -> Variant:
	if value is Vector3:
		return value
	if value is Array and (value as Array).size() >= 3:
		return Vector3(float(value[0]), float(value[1]), float(value[2]))
	return null


func _uniform_scale(basis: Basis) -> float:
	var scale := basis.get_scale()
	return (absf(scale.x) + absf(scale.y) + absf(scale.z)) / 3.0


func _is_finite_basis(basis: Basis) -> bool:
	return basis.x.is_finite() and basis.y.is_finite() and basis.z.is_finite()


func _fail(message: String) -> bool:
	_last_error = message
	return false
