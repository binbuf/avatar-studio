extends SceneTree
const Surface = preload("res://scenes/face_decal_surface.gd")
const Fixtures = preload("res://tests/face_decal_flat_fixtures.gd")
const Oracle = preload("res://tests/face_decal_surface_oracle.gd")
var failures: Array[String] = []
var result := {"schema": "avatar_face_decal_surface.result.v1", "goldens": 0, "stable_frames": 0, "cases": []}
var rendering := false
var performance := false
var update_goldens := false
var viewport: SubViewport
var stage: Node3D
var camera: Camera3D
var decal = Surface.new()
var image := Fixtures.make_camera_image()
var homography := Fixtures.make_homography()

func _initialize() -> void:
	rendering = "--render" in OS.get_cmdline_user_args()
	performance = "--perf" in OS.get_cmdline_user_args()
	update_goldens = "--update-goldens" in OS.get_cmdline_user_args()
	_run.call_deferred()

func check(ok: bool, message: String) -> void:
	if not ok:
		failures.append(message)
		printerr("FAIL: ", message)

func setup() -> void:
	viewport = SubViewport.new()
	viewport.size = Vector2i(320, 320)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	root.add_child(viewport)
	stage = Node3D.new()
	viewport.add_child(stage)
	camera = Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.near = .01
	camera.far = 10
	stage.add_child(camera)
	var world := WorldEnvironment.new()
	world.environment = Environment.new()
	world.environment.background_mode = Environment.BG_COLOR
	world.environment.background_color = Color(.03, .03, .03)
	world.environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	stage.add_child(world)
	decal.set_camera_texture(ImageTexture.create_from_image(image))
	decal.set_canonical_to_image(homography)

func model(name: String, submesh: String = "") -> Node3D:
	var m: Node3D = ClassDB.instantiate("AvatarModel")
	stage.add_child(m)
	if not submesh.is_empty():
		m.call("set_face_submesh", submesh)
	var path := "res://../tests/fixtures/models/" + name + ".glb"
	if name == "avocado":
		path = "res://../assets/models/avocado_half.glb"
	check(m.call("import_glb", ProjectSettings.globalize_path(path)) == OK, "import " + name)
	var meshes: Array[MeshInstance3D] = []
	Oracle.collect(m, meshes)
	for instance in meshes:
		for s in range(instance.mesh.get_surface_count()):
			var base := instance.mesh.surface_get_material(s) as BaseMaterial3D
			if base != null:
				base.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	return m

func configure(m: Node3D) -> void:
	var aabb: AABB = m.call("get_model_aabb")
	var face := Fixtures.make_face(aabb)
	var cut := {"mode": "cut", "cut": {"origin": face.origin - Vector3.UP*face.scale*1.5,
		"normal": Vector3.UP, "falloff": face.scale}}
	check(m.call("configure_head_region", cut, face, null), "calibrated cut")
	check(decal.build(m, face, Fixtures.make_regions()), "build surface: " + decal.get_last_error())
	camera.size = maxf(aabb.size.y * 1.45, .12)
	var centre := aabb.get_center()
	camera.position = centre + Vector3(0, 0, -maxf(aabb.size.y * 3, .8))
	camera.look_at(centre)
	camera.current = true

func passes(m: Node3D) -> Array[MeshInstance3D]:
	var meshes: Array[MeshInstance3D] = []
	Oracle.collect(m, meshes)
	var out: Array[MeshInstance3D] = []
	for mesh in meshes:
		var pass_mesh := mesh.get_node_or_null("HeadDecal") as MeshInstance3D
		if pass_mesh != null:
			out.append(pass_mesh)
	return out

func visible(m: Node3D, value: bool) -> void:
	for pass_mesh in passes(m):
		pass_mesh.visible = value

func contract() -> void:
	var m := model("head_transformed")
	configure(m)
	var original: ShaderMaterial = decal.material
	check(not decal.build(m, {}, Fixtures.make_regions()), "invalid face rejected")
	check(decal.material == original, "invalid build preserves pass")
	check(not decal.build(m, decal.face, [Fixtures.make_regions()[0], Fixtures.make_regions()[0]]), "duplicate region rejected")
	check(not decal.set_canonical_to_image(Basis(Vector3.ZERO, Vector3.ZERO, Vector3.ZERO)), "singular H rejected")
	check(not decal.set_region_opacity("eye_l", NAN), "NaN opacity rejected")
	check(not decal.set_region_anchor("missing", Vector2.ZERO), "unknown anchor rejected")
	check(decal.set_region_anchor("eye_l", Vector2(.02, .01)), "anchor update")
	check(decal.set_region_enabled("eye_l", false), "disable region")
	check((decal.material.get_shader_parameter("region_opacity") as Array)[0] == 0.0, "disabled opacity uploaded")
	check(decal.set_region_opacity("mouth", .4), "opacity update")
	check(decal.build(m, decal.face, Fixtures.make_regions()), "replace pass")
	var meshes: Array[MeshInstance3D] = []
	Oracle.collect(m, meshes)
	check(passes(m).size() == meshes.size(), "whole mesh fallback")
	var data: Array = m.call("get_head_surface_data")
	m.call("set_head_rotation", Quaternion.from_euler(Vector3(.2, .5, -.1)))
	for s in range(meshes.size()):
		var instance := meshes[s]
		check(instance.get_child_count() == 1, "replacement creates no duplicate draw")
		check((instance.get_node("HeadDecal") as MeshInstance3D).mesh == instance.mesh, "same live geometry")
		var arrays: Array = instance.mesh.surface_get_arrays(0)
		var points: PackedFloat32Array = arrays[Mesh.ARRAY_CUSTOM0]
		var normals: PackedFloat32Array = arrays[Mesh.ARRAY_CUSTOM1]
		var item: Dictionary = data[s]
		var transform: Transform3D = item.rest_transform
		var max_error := 0.0
		for i in range(item.rest_local.size()):
			var p: Vector3 = transform * item.rest_local[i]
			var n: Vector3 = (transform.basis.inverse().transposed() * item.rest_normals[i]).normalized()
			max_error = maxf(max_error, p.distance_to(Vector3(points[3*i], points[3*i+1], points[3*i+2])))
			max_error = maxf(max_error, n.distance_to(Vector3(normals[3*i], normals[3*i+1], normals[3*i+2])))
		check(max_error < .00001, "immutable rest position + inverse-transpose normal after deformation")
	decal.clear()
	check(passes(m).is_empty(), "clear removes pass")
	m.free()
	m = model("head_node", "face")
	configure(m)
	check(passes(m).size() == 1 and passes(m)[0].get_parent().name == "face", "authored face_submesh restricts draw")
	decal.clear()
	m.free()
	result["contract"] = failures.is_empty()

func capture() -> Image:
	await process_frame
	await RenderingServer.frame_post_draw
	return viewport.get_texture().get_image()

func difference(a: Image, b: Image) -> Dictionary:
	if a.get_size() != b.get_size():
		return {"mean": 1.0, "changed": 0, "large": 999999}
	var error := 0.0
	var changed := 0
	var large := 0
	for y in range(a.get_height()):
		for x in range(a.get_width()):
			var ca := a.get_pixel(x, y)
			var cb := b.get_pixel(x, y)
			var e := maxf(absf(ca.r-cb.r), maxf(absf(ca.g-cb.g), absf(ca.b-cb.b)))
			error += e
			if e > 2.0/255.0: changed += 1
			if e > .1: large += 1
	return {"mean": error / (a.get_width()*a.get_height()), "changed": changed, "large": large}

func scenario(m: Node3D, name: String, pose: String, q: Quaternion) -> void:
	if pose == "back":
		# Turn the whole head rigidly for the rear test. A 180-degree cut pose
		# folds the neck band through itself, leaving some original front visible.
		var aabb: AABB = m.call("get_model_aabb")
		var cut := {"mode": "cut", "cut": {"origin": aabb.position - Vector3.UP*decal.face.scale,
			"normal": Vector3.UP, "falloff": decal.face.scale*.25}}
		check(m.call("configure_head_region", cut, decal.face, null), "rigid rear-view test")
	m.call("set_head_rotation", q)
	visible(m, false)
	var base := await capture()
	var blank := Image.create(base.get_width(), base.get_height(), false, Image.FORMAT_RGBA8)
	blank.fill(base.get_pixel(0, 0))
	check(difference(base, blank).changed > 1000, name + "/" + pose + " model visible")
	visible(m, true)
	var actual := await capture()
	var reference: Dictionary = Oracle.render(m, camera, base, decal.face, decal.regions, homography, image, q)
	var expected: Image = reference.image
	var diff := difference(actual, expected)
	check(diff.mean < .003 and diff.large < 150, "%s/%s CPU surface oracle %s" % [name, pose, diff])
	var coverage := difference(base, actual)
	if pose == "back":
		check(coverage.changed == 0, name + " no decal on rear hemisphere")
	else:
		check(coverage.changed > 100, name + "/" + pose + " visible decal coverage")
	if name == "sculpted" and pose in ["yaw-left", "yaw-right"]:
		check(reference.occluded_eye_pixels > 10, pose + " nose occludes far-eye pixels")
	var renderer := RenderingServer.get_current_rendering_method()
	var path := ProjectSettings.globalize_path("res://../tests/golden/face-decal-surface/%s/%s-%s.png" % [renderer, name, pose])
	if update_goldens:
		DirAccess.make_dir_recursive_absolute(path.get_base_dir())
		check(expected.save_png(path) == OK, "save independent golden")
	else:
		var stored := Image.load_from_file(path)
		check(stored != null and not stored.is_empty(), "golden exists " + path)
		if stored != null and not stored.is_empty():
			check(difference(actual, stored).mean < .003, "committed golden " + name + "/" + pose)
	result.goldens += 1
	result.cases.append({"model": name, "pose": pose, "error": diff.mean, "large_errors": diff.large,
		"changed_pixels": coverage.changed, "occluded_eye_pixels": reference.occluded_eye_pixels})
	actual.save_png(ProjectSettings.globalize_path("res://../.tools/T21-%s-%s-actual.png" % [name, pose]))
	if pose == "front":
		for i in range(4):
			var repeated := await capture()
			check(difference(actual, repeated).changed == 0, name + " stable coplanar pass without z-fighting")
			result.stable_frames += 1

func golden_suite() -> void:
	for name in ["avocado", "flat", "sphere", "sculpted"]:
		var m := model(name if name == "avocado" else "surface_" + name)
		configure(m)
		for pose in ["front", "yaw-left", "yaw-right", "deformed", "back"]:
			var euler := Vector3.ZERO
			match pose:
				"yaw-left": euler.y = -30
				"yaw-right": euler.y = 30
				"deformed": euler = Vector3(18, 30, 12)
				"back": euler.y = 180
			await scenario(m, name, pose, Quaternion.from_euler(euler * PI / 180))
		if name == "sphere":
			configure(m)
			var settings := Fixtures.make_regions()
			settings[2].rotation_deg = 25.0
			settings[2].gain = Vector3(.75, .9, 1.1)
			settings[2].lift = Vector3(.03, .01, 0)
			check(decal.build(m, decal.face, settings), "sampling variant")
			decal.set_region_anchor("mouth", Vector2(.03, .02))
			decal.set_region_opacity("mouth", .42)
			homography.z.x += .3
			decal.set_canonical_to_image(homography)
			await scenario(m, name, "sampling", Quaternion.from_euler(Vector3(.1, .4, 0)))
			homography = Fixtures.make_homography()
			decal.set_canonical_to_image(homography)
		decal.clear()
		m.free()
	var node_model := model("head_node", "face")
	var node_face := {"origin": Vector3(0, 1.65, -.15), "normal": Vector3(0, 0, -1),
		"up": Vector3.UP, "scale": .3}
	check(node_model.call("configure_head_region", {"mode": "node", "node": "head"}, node_face, null), "node deformation configure")
	check(decal.build(node_model, node_face, Fixtures.make_regions()), "node surface build")
	camera.position = Vector3(0, 1.05, -4)
	camera.look_at(Vector3(0, 1.05, 0))
	camera.size = 2.5
	await scenario(node_model, "node", "deformed", Quaternion.from_euler(Vector3(.2, .5, -.15)))
	decal.clear()
	node_model.free()

func benchmark() -> void:
	Engine.max_fps = 0
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	viewport.size = Vector2i(1920, 1080)
	var m := model("head_cut")
	configure(m)
	check(m.call("get_triangle_count") == 20000, "20k-triangle perf fixture")
	visible(m, false)
	var base := await capture()
	visible(m, true)
	var actual := await capture()
	check(difference(base, actual).changed > 1000, "perf run renders visible decal pixels")
	actual.save_png(ProjectSettings.globalize_path("res://../.tools/T21-perf-actual.png"))
	RenderingServer.viewport_set_measure_render_time(viewport.get_viewport_rid(), true)
	var samples := {false: [], true: []}
	var draws := {}
	# Alternate complete blocks to reduce clock/thermal drift. GPU timestamps
	# cover viewport rendering, excluding readback, CPU deformation and vsync.
	for block in range(6):
		var enabled := block % 2 == 1
		visible(m, enabled)
		for i in range(90):
			await process_frame
			await RenderingServer.frame_post_draw
			if i >= 30:
				samples[enabled].append(RenderingServer.viewport_get_measured_render_time_gpu(viewport.get_viewport_rid()))
		draws[enabled] = viewport.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE, Viewport.RENDER_INFO_DRAW_CALLS_IN_FRAME)
	for enabled in [false, true]:
		samples[enabled].sort()
	var off: float = samples[false][90]
	var on: float = samples[true][90]
	check(off > 0 and on > 0, "real GPU viewport timestamps available")
	check(int(draws[true]) == int(draws[false]) + 1, "three regions cost exactly one extra draw")
	var cost := on - off
	# At tens of microseconds clock/query noise can exceed the signed median
	# delta. Retain it verbatim and gate a conservative percentile spread too;
	# a negative delta must never be presented as a negative physical cost.
	var upper_spread: float = maxf(0, samples[true][171] - samples[false][9])
	result["performance"] = {"resolution": "1920x1080", "triangles": 20000,
		"samples_per_state": 180, "warmup_per_block": 30, "blocks": 6,
		"base_gpu_median_ms": off, "decal_gpu_median_ms": on, "second_pass_gpu_ms": cost,
		"second_pass_upper_spread_ms": upper_spread,
		"base_gpu_p95_ms": samples[false][171], "decal_gpu_p95_ms": samples[true][171],
		"base_draws": draws[false], "decal_draws": draws[true], "budget_ms": .7,
		"within_budget": maxf(cost, upper_spread) < .7,
		"adapter": RenderingServer.get_video_adapter_name(), "api": RenderingServer.get_video_adapter_api_version()}
	check(maxf(cost, upper_spread) < .7, "second pass median delta and percentile spread under 0.7 ms")
	decal.clear()
	m.free()

func _run() -> void:
	setup()
	if performance:
		await benchmark()
	else:
		contract()
		if rendering:
			await golden_suite()
	result["rendered"] = rendering
	result["renderer"] = RenderingServer.get_current_rendering_method()
	result["failures"] = failures
	result["passed"] = failures.is_empty()
	var suffix := "-perf" if performance else ("-render" if rendering else "")
	var file := FileAccess.open(ProjectSettings.globalize_path("res://../.tools/T21-surface%s.json" % suffix), FileAccess.WRITE)
	file.store_string(JSON.stringify(result, "  "))
	print("T21 ", JSON.stringify(result))
	quit(0 if failures.is_empty() else 1)
