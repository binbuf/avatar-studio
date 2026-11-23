extends SceneTree

## M1 golden-image suite (T26).
##
## Renders the reviewed scene matrix through the real surface decal pass and
## compares each frame with a committed baseline using SSIM >= 0.995 plus a
## max-delta guard inside the decal's own bounding box. Baselines change only in
## a reviewed commit (see docs/design/testing-and-quality.md).
##
##   --render          required; a headless run writes a failure JSON
##   --update-goldens  rewrites the baselines (review the diff before committing)

const Golden = preload("res://tests/golden_image.gd")
const Fixtures = preload("res://tests/face_decal_flat_fixtures.gd")
const Surface = preload("res://scenes/face_decal_surface.gd")
const Defaults = preload("res://scenes/face_calibration_defaults.gd")

const GOLDEN_DIR := "res://../tests/golden/m1"
const ARTIFACT_DIR := "res://../.tools/T26-golden-artifacts"
const SIZE := 320
const AVATARS := [["flat", "surface_flat"], ["sphere", "surface_sphere"],
		["sculpted", "surface_sculpted"]]
const MOTIONS := ["still", "yaw-sweep", "fast-nod"]
const LIGHTING := ["key", "key-fill"]

var failures: Array[String] = []
var result := {
	"schema": "avatar_m1_golden.result.v1",
	"passed": false,
	"rendered": false,
	"renderer": "",
	"goldens": 0,
	"cases": [],
	"failures": [],
}
var rendering := false
var update_goldens := false
var viewport: SubViewport
var stage: Node3D
var camera: Camera3D
var key_light: DirectionalLight3D
var fill_light: DirectionalLight3D
var decal = Surface.new()
var camera_image: Image = Fixtures.make_camera_image()
var homography: Basis = Fixtures.make_homography()


func _initialize() -> void:
	rendering = "--render" in OS.get_cmdline_user_args()
	update_goldens = "--update-goldens" in OS.get_cmdline_user_args()
	_run.call_deferred()


func check(ok: bool, message: String) -> void:
	if not ok:
		failures.append(message)
		printerr("FAIL: ", message)


func setup() -> void:
	viewport = SubViewport.new()
	viewport.size = Vector2i(SIZE, SIZE)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.transparent_bg = false
	root.add_child(viewport)
	stage = Node3D.new()
	viewport.add_child(stage)
	camera = Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.near = 0.01
	camera.far = 20.0
	stage.add_child(camera)
	var world := WorldEnvironment.new()
	world.environment = Environment.new()
	world.environment.background_mode = Environment.BG_COLOR
	world.environment.background_color = Color(0.03, 0.03, 0.03)
	world.environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	world.environment.ambient_light_color = Color(0.18, 0.18, 0.20)
	world.environment.ambient_light_energy = 1.0
	world.environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	stage.add_child(world)
	key_light = DirectionalLight3D.new()
	key_light.rotation_degrees = Vector3(-40, -30, 0)
	key_light.light_energy = 1.2
	stage.add_child(key_light)
	fill_light = DirectionalLight3D.new()
	fill_light.rotation_degrees = Vector3(-20, 150, 0)
	fill_light.light_energy = 0.0
	stage.add_child(fill_light)
	decal.set_camera_texture(ImageTexture.create_from_image(camera_image))
	decal.set_canonical_to_image(homography)


func model(fixture: String, submesh: String = "") -> Node3D:
	var m: Node3D = ClassDB.instantiate("AvatarModel")
	stage.add_child(m)
	if not submesh.is_empty():
		m.call("set_face_submesh", submesh)
	var path := "res://../tests/fixtures/models/" + fixture + ".glb"
	check(m.call("import_glb", ProjectSettings.globalize_path(path)) == OK, "import " + fixture)
	return m


func configure(m: Node3D) -> void:
	var aabb: AABB = m.call("get_model_aabb")
	var face := Defaults.make_face(aabb)
	check(m.call("configure_head_region", Defaults.make_head_region(face), face, null),
			"calibrated cut")
	check(decal.build(m, face, Defaults.make_regions()),
			"build surface decal: " + String(decal.get_last_error()))
	camera.size = maxf(aabb.size.y * 1.45, 0.12)
	var centre := aabb.get_center()
	camera.position = centre + Vector3(0, 0, -maxf(aabb.size.y * 3.0, 0.8))
	camera.look_at(centre)
	camera.current = true


func passes(m: Node3D) -> Array[MeshInstance3D]:
	var out: Array[MeshInstance3D] = []
	for child in m.find_children("*", "MeshInstance3D", true, false):
		var instance := child as MeshInstance3D
		if instance != null and instance.name == "HeadDecal":
			out.append(instance)
	return out


func visible(m: Node3D, value: bool) -> void:
	for pass_mesh in passes(m):
		pass_mesh.visible = value


func capture() -> Image:
	await process_frame
	await RenderingServer.frame_post_draw
	return viewport.get_texture().get_image()


func set_lighting(kind: String) -> void:
	fill_light.light_energy = 0.6 if kind == "key-fill" else 0.0


func motion_rotation(motion: String) -> Quaternion:
	match motion:
		"yaw-sweep":
			return Quaternion.from_euler(Vector3(0.0, -30.0, 0.0) * PI / 180.0)
		"fast-nod":
			return Quaternion.from_euler(Vector3(22.0, 8.0, -6.0) * PI / 180.0)
	return Quaternion.IDENTITY


func render_case(name: String, m: Node3D, rotation: Quaternion) -> void:
	m.call("set_head_rotation", rotation)
	visible(m, false)
	var base := await capture()
	visible(m, true)
	var actual := await capture()
	var decal_rect := Golden.changed_rect(base, actual)
	check(decal_rect.size.x > 0 and decal_rect.size.y > 0,
			name + " decal covers a visible region")
	check_golden(name, actual, decal_rect)
	visible(m, false)


func check_golden(name: String, actual: Image, decal_rect: Rect2i) -> void:
	var path := ProjectSettings.globalize_path("%s/%s/%s.png" % [GOLDEN_DIR,
			RenderingServer.get_current_rendering_method(), name])
	if update_goldens:
		DirAccess.make_dir_recursive_absolute(path.get_base_dir())
		check(actual.save_png(path) == OK, "write baseline " + name)
		result["goldens"] = int(result["goldens"]) + 1
		return
	var stored := Image.load_from_file(path)
	if stored == null or stored.is_empty():
		check(false, "baseline exists: " + path)
		return
	var value := Golden.ssim(actual, stored)
	var decal_delta := Golden.max_delta(actual, stored, decal_rect)
	var global_delta := Golden.max_delta_image(actual, stored)
	var ok := value >= Golden.SSIM_MIN and decal_delta <= Golden.MAX_DELTA
	result["cases"].append({
		"name": name,
		"ssim": value,
		"decal_max_delta": decal_delta,
		"global_max_delta": global_delta,
		"decal_bbox": [decal_rect.position.x, decal_rect.position.y,
				decal_rect.size.x, decal_rect.size.y],
		"passed": ok,
	})
	result["goldens"] = int(result["goldens"]) + 1
	if not ok:
		write_artifacts(name, actual, stored)
		check(false, "%s: SSIM %.6f (min %.3f), decal max delta %d (max %d)" % [name, value,
				Golden.SSIM_MIN, decal_delta, Golden.MAX_DELTA])


func write_artifacts(name: String, actual: Image, baseline: Image) -> void:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(ARTIFACT_DIR))
	var directory := ProjectSettings.globalize_path(ARTIFACT_DIR)
	actual.save_png(directory + "/" + name + ".actual.png")
	baseline.save_png(directory + "/" + name + ".baseline.png")
	Golden.diff_image(actual, baseline).save_png(directory + "/" + name + ".diff.png")
	printerr("artifacts: ", directory, "/", name, ".{actual,baseline,diff}.png")


func scene_matrix() -> void:
	for avatar in AVATARS:
		var label: String = avatar[0]
		var m := model(String(avatar[1]))
		configure(m)
		for lighting in LIGHTING:
			set_lighting(lighting)
			for motion in MOTIONS:
				await render_case("%s-%s-%s" % [label, motion, lighting], m,
						motion_rotation(motion))
		decal.clear()
		m.free()


func marker_case() -> void:
	var m := model("surface_flat")
	configure(m)
	set_lighting("key")
	visible(m, false)
	var base := await capture()
	visible(m, true)
	var actual := await capture()
	var decal_rect := Golden.changed_rect(base, actual)
	check_golden("marker-asymmetric", actual, decal_rect)
	# The camera texture puts the magenta dot on the viewer's left (the
	# subject's right cheek). A mirrored or swapped decal puts it on the right.
	var marker := _colour_centroid(actual, Color(1.0, 0.0, 1.0), decal_rect)
	check(marker.x >= 0.0 and marker.x < float(decal_rect.get_center().x),
			"asymmetric marker stays on the subject's right cheek")
	result["marker_x"] = marker.x
	result["decal_center_x"] = float(decal_rect.get_center().x)
	decal.clear()
	m.free()


func _colour_centroid(image: Image, colour: Color, rect: Rect2i) -> Vector2:
	var total := 0.0
	var sum := 0.0
	for y in range(rect.position.y, rect.end.y):
		for x in range(rect.position.x, rect.end.x):
			var pixel := image.get_pixel(x, y)
			if pixel.r > 0.55 and pixel.b > 0.55 and pixel.g < 0.35:
				sum += float(x)
				total += 1.0
	if total == 0.0:
		return Vector2(-1.0, -1.0)
	return Vector2(sum / total, 0.0)


func framing_cases() -> void:
	var m := model("surface_flat")
	configure(m)
	set_lighting("key")
	var aabb: AABB = m.call("get_model_aabb")
	var face: Dictionary = decal.face
	var centre := aabb.get_center()
	var distance := maxf(aabb.size.y * 3.0, 0.8)
	camera.size = aabb.size.y * 1.45
	camera.position = centre + Vector3(0, 0, -distance)
	camera.look_at(centre)
	await render_case("framing-model-fit", m, Quaternion.IDENTITY)
	camera.size = aabb.size.y * 0.65
	camera.position = face.origin + Vector3(0, 0, -distance)
	camera.look_at(face.origin)
	await render_case("framing-head-fit", m, Quaternion.IDENTITY)
	camera.size = aabb.size.y * 0.9
	camera.position = centre + Vector3(0, 0, -distance)
	camera.look_at(centre)
	await render_case("framing-zoom-in", m, Quaternion.IDENTITY)
	camera.size = aabb.size.y * 1.45
	camera.position = centre + Vector3(aabb.size.x * 0.25, 0, -distance)
	camera.look_at(centre)
	await render_case("framing-offset", m, Quaternion.IDENTITY)
	decal.clear()
	m.free()


func head_region_cases() -> void:
	var cut := model("surface_sphere")
	configure(cut)
	set_lighting("key")
	await render_case("head-cut-yaw-left", cut,
			Quaternion.from_euler(Vector3(0.0, -30.0, 0.0) * PI / 180.0))
	await render_case("head-cut-yaw-right", cut,
			Quaternion.from_euler(Vector3(0.0, 30.0, 0.0) * PI / 180.0))
	decal.clear()
	cut.free()

	var node := model("head_node", "face")
	var face := {"origin": Vector3(0, 1.65, -0.15), "normal": Vector3(0, 0, -1),
		"up": Vector3.UP, "scale": 0.3}
	check(node.call("configure_head_region", {"mode": "node", "node": "head"}, face, null),
			"node deformation configure")
	check(decal.build(node, face, Defaults.make_regions()), "node surface build")
	camera.size = 2.5
	camera.position = Vector3(0, 1.05, -4)
	camera.look_at(Vector3(0, 1.05, 0))
	await render_case("head-node-yaw-left", node,
			Quaternion.from_euler(Vector3(0.0, -30.0, 0.0) * PI / 180.0))
	await render_case("head-node-yaw-right", node,
			Quaternion.from_euler(Vector3(0.0, 30.0, 0.0) * PI / 180.0))
	decal.clear()
	node.free()


func barrel_case() -> void:
	var m := model("surface_sphere")
	configure(m)
	set_lighting("key")
	decal.set_camera_texture(ImageTexture.create_from_image(_barrel_warp(camera_image, 0.08)))
	await render_case("barrel-k008", m, Quaternion.IDENTITY)
	decal.set_camera_texture(ImageTexture.create_from_image(camera_image))
	decal.clear()
	m.free()


## Synthetic radial lens warp: source = centre + (p - centre) / (1 + k1 * r^2).
## It is a stable visual case, not a calibrated lens model.
func _barrel_warp(source: Image, k1: float) -> Image:
	var out := Image.create(source.get_width(), source.get_height(), false, Image.FORMAT_RGBA8)
	var centre := Vector2(source.get_width(), source.get_height()) * 0.5
	for y in range(source.get_height()):
		for x in range(source.get_width()):
			var p := Vector2(x + 0.5, y + 0.5) - centre
			var r2 := p.length_squared() / (centre.length_squared())
			var scale := 1.0 + k1 * r2
			var src := centre + p / scale
			var sx := clampi(floori(src.x), 0, source.get_width() - 1)
			var sy := clampi(floori(src.y), 0, source.get_height() - 1)
			out.set_pixel(x, y, source.get_pixel(sx, sy))
	return out


func blank_cases() -> void:
	var m := model("surface_flat")
	configure(m)
	set_lighting("key")
	visible(m, false)
	var base := await capture()
	visible(m, true)
	for case in ["no-face", "missing-mouth", "tracker-timeout"]:
		for id in ["eye_l", "eye_r", "mouth"]:
			decal.set_region_enabled(id, false)
		var actual := await capture()
		var changed := Golden.changed_rect(base, actual)
		check(changed.size.x == 0 and changed.size.y == 0,
				"blank state %s shows no camera pixels" % case)
		result["cases"].append({"name": "blank-" + case, "camera_pixels": changed.size.x})
		for id in ["eye_l", "eye_r", "mouth"]:
			decal.set_region_enabled(id, true)
	decal.clear()
	m.free()


func _run() -> void:
	if not rendering or DisplayServer.get_name() == "headless":
		result["failures"] = ["--render with a real display is required"]
		result["passed"] = false
		_write_result()
		quit(2)
		return
	setup()
	await scene_matrix()
	await marker_case()
	await framing_cases()
	await head_region_cases()
	await barrel_case()
	await blank_cases()
	result["rendered"] = true
	result["renderer"] = RenderingServer.get_current_rendering_method()
	result["failures"] = failures
	result["passed"] = failures.is_empty()
	_write_result()
	print("T26 golden: ", JSON.stringify({"passed": result["passed"],
			"goldens": result["goldens"], "failures": failures}))
	quit(0 if failures.is_empty() else 1)


func _write_result() -> void:
	var path := ProjectSettings.globalize_path("res://../.tools/T26-golden.json")
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file != null:
		file.store_string(JSON.stringify(result, "  "))

