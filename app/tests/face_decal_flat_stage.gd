extends Node3D

## T20 flat-decal test scene: the avocado, a synthetic camera texture, and the
## three region quads. The scene is built on ready so the test and a manual run
## share the same wiring. Rendering is driven by app/tests/face_decal_flat.gd.

const FaceDecalFlatScript = preload("res://scenes/face_decal_flat.gd")
const Fixtures = preload("res://tests/face_decal_flat_fixtures.gd")

@onready var camera: Camera3D = $Camera

var avatar: Node3D = null
var decal = null
var face: Dictionary = {}
var regions: Array = []
var camera_image: Image = null
var camera_texture: ImageTexture = null
var homography := Basis.IDENTITY


func _ready() -> void:
	_build()


func _build() -> void:
	_setup_environment()
	avatar = ClassDB.instantiate("AvatarModel") as Node3D
	if avatar == null:
		printerr("face_decal_flat_stage: AvatarModel is not registered")
		return
	avatar.name = "Avocado"
	add_child(avatar)
	var path := ProjectSettings.globalize_path("res://../assets/models/avocado_half.glb")
	if int(avatar.call("import_glb", path)) != OK:
		printerr("face_decal_flat_stage: avocado import failed")
		return
	var aabb: AABB = avatar.call("get_model_aabb")
	face = Fixtures.make_face(aabb)
	regions = Fixtures.make_regions()
	_frame_camera(aabb)
	decal = FaceDecalFlatScript.new()
	if not decal.build(avatar, face, regions):
		printerr("face_decal_flat_stage: decal build failed: " + decal.get_last_error())
		return
	face = decal.get_face()
	regions = decal.regions
	set_camera_image(Fixtures.make_camera_image())
	set_homography(Fixtures.make_homography())


func set_camera_image(image: Image) -> void:
	camera_image = image
	camera_texture = ImageTexture.create_from_image(image)
	if decal != null and decal.is_built():
		decal.set_camera_texture(camera_texture)


func set_homography(value: Basis) -> void:
	homography = value
	if decal != null and decal.is_built():
		decal.set_canonical_to_image(value)


func set_model_visible(value: bool) -> void:
	for mesh in _model_meshes():
		mesh.visible = value


func set_decal_visible(value: bool) -> void:
	if decal == null or not decal.is_built():
		return
	for id in decal.get_quad_ids():
		decal.get_quad(id).visible = value


func set_region_enabled(id: String, value: bool) -> void:
	if decal != null and decal.is_built():
		decal.set_region_enabled(id, value)


func model_mesh_count() -> int:
	return _model_meshes().size()


func _frame_camera(aabb: AABB) -> void:
	var centre := aabb.get_center()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = maxf(aabb.size.y * 1.45, 0.12)
	camera.near = 0.01
	camera.far = 10.0
	camera.position = Vector3(centre.x, centre.y, face.origin.z - 0.8)
	camera.look_at(Vector3(centre.x, centre.y, face.origin.z), Vector3.UP)
	camera.current = true


func _setup_environment() -> void:
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.03, 0.03, 0.03)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.7, 0.7, 0.7)
	environment.ambient_light_energy = 0.6
	environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var world := WorldEnvironment.new()
	world.name = "WorldEnvironment"
	world.environment = environment
	add_child(world)
	var key := DirectionalLight3D.new()
	key.name = "KeyLight"
	key.rotation_degrees = Vector3(-35.0, -25.0, 0.0)
	key.light_energy = 1.5
	add_child(key)


func _model_meshes() -> Array[MeshInstance3D]:
	var meshes: Array[MeshInstance3D] = []
	if avatar != null:
		_collect_meshes(avatar, meshes)
	return meshes


func _collect_meshes(node: Node, meshes: Array[MeshInstance3D]) -> void:
	for child in node.get_children():
		if child is MeshInstance3D:
			if not str(child.name).begins_with("FaceDecalFlat_"):
				meshes.append(child)
		_collect_meshes(child, meshes)
