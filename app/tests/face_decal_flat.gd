extends SceneTree

## T20 flat-decal tests.
##
## Headless: component contract, quad placement, uniform binding, validation.
## Windowed (`-- --render`): the avocado + synthetic camera texture, an
## independent analytic reference, four committed goldens, the black-camera
## mask diff, edge clamp, mirroring, and the linear colour check.
##
## Run with:
##   <godot> --headless --path app --script res://tests/face_decal_flat.gd
##   <godot> --path app --rendering-method gl_compatibility --resolution 320x320 \
##       --script res://tests/face_decal_flat.gd -- --render
##
## A JSON summary is written to .tools/T20-face-decal.json (or -render.json).

const STAGE_SCENE := "res://tests/scenes/face_decal_flat.tscn"
const SHADER_PATH := "res://shaders/face_decal_flat.gdshader"
const Fixtures = preload("res://tests/face_decal_flat_fixtures.gd")
const FaceDecalFlatScript = preload("res://scenes/face_decal_flat.gd")

const VIEWPORT_SIZE := Vector2i(320, 320)
const GOLDEN_DIR := "res://../tests/golden/face-decal-flat"
const CHANGED_THRESHOLD := 2.0 / 255.0
const GOLDEN_MEAN_ERROR := 0.01

var failures: Array[String] = []
var result := {
	"schema": "avatar_face_decal_flat.result.v1",
	"rendered": false,
	"scenarios": 0,
	"goldens": 0,
	"mask_diff": false,
	"edge_clamp": false,
	"mirroring": false,
	"colour_space": false,
	"opacity": false,
	"avocado_pixels": 0,
	"changed_pixels": 0,
}
var rendering := false
var update_goldens := false
var viewport: SubViewport = null
var stage = null
var camera: Camera3D = null


func _initialize() -> void:
	rendering = "--render" in OS.get_cmdline_user_args()
	update_goldens = "--update-goldens" in OS.get_cmdline_user_args()
	_run.call_deferred()


func check(ok: bool, message: String) -> void:
	if not ok:
		failures.append(message)
		printerr("FAIL: ", message)


func _run() -> void:
	if rendering:
		_setup_viewport()
	else:
		stage = load(STAGE_SCENE).instantiate()
		root.add_child(stage)
		camera = stage.camera
	await process_frame
	await process_frame
	_check_component()
	if rendering:
		await _check_render()
	result["rendered"] = rendering
	result["failures"] = failures
	result["passed"] = failures.is_empty()
	_write_result()
	quit(0 if failures.is_empty() else 1)


func _setup_viewport() -> void:
	viewport = SubViewport.new()
	viewport.size = VIEWPORT_SIZE
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	root.add_child(viewport)
	stage = load(STAGE_SCENE).instantiate()
	viewport.add_child(stage)
	camera = stage.camera


func _check_component() -> void:
	check(stage != null, "test scene instantiated")
	if stage == null:
		return
	check(stage.avatar != null, "avocado node exists")
	if stage.avatar != null:
		check(bool(stage.avatar.call("is_loaded")), "avocado imported")
	check(stage.model_mesh_count() >= 1, "avocado has model meshes")
	check(stage.decal != null, "flat decal manager exists")
	if stage.decal == null:
		return
	check(stage.decal.is_built(), "flat decal built")
	var ids: Array = stage.decal.get_quad_ids()
	check(ids.size() == 3, "exactly three region quads")
	for expected in ["eye_l", "eye_r", "mouth"]:
		check(expected in ids, "quad for " + expected)

	var shader_source := FileAccess.get_file_as_string(SHADER_PATH)
	check(shader_source.contains("depth_draw_never"), "shader disables depth write")
	check(shader_source.contains("blend_mix"), "shader uses the transparent pass")
	check(shader_source.contains("repeat_disable"), "camera sampler never wraps")
	check(shader_source.contains("srgb_to_linear"), "shader converts sRGB to linear")
	check(shader_source.contains("facing * facing * opacity"),
			"shader applies the facing fade and opacity")
	check(shader_source.contains("discard"), "shader discards masked/frame pixels")
	check(not shader_source.contains("repeat_enable"), "shader never enables wrapping")

	var world: Transform3D = stage.avatar.global_transform
	var face_origin: Vector3 = world * stage.face.origin
	var face_right: Vector3 = (world.basis * stage.face.right).normalized()
	var face_up: Vector3 = (world.basis * stage.face.up).normalized()
	var face_normal: Vector3 = (world.basis * stage.face.normal).normalized()
	var face_scale: float = float(stage.face.scale) * _uniform_scale(world.basis)
	for region in stage.regions:
		var id: String = region.id
		var quad: MeshInstance3D = stage.decal.get_quad(id)
		check(quad != null, "quad exists for " + id)
		if quad == null:
			continue
		check(quad.get_parent() == stage.avatar, id + " quad is parented to the head")
		check(quad.cast_shadow == GeometryInstance3D.SHADOW_CASTING_SETTING_OFF,
				id + " quad casts no shadow")
		var mesh := quad.mesh as QuadMesh
		check(mesh != null, id + " uses a quad mesh")
		if mesh == null:
			continue
		check(mesh.size.is_equal_approx(Vector2(region.size.x, region.size.y) * face_scale),
				id + " quad size matches the region")
		var offset := maxf(face_scale * 0.01, 0.0001)
		var expected_centre: Vector3 = face_origin \
				+ (face_right * region.center.x + face_up * region.center.y) * face_scale \
				+ face_normal * offset
		check(quad.global_transform.origin.distance_to(expected_centre) < 1e-4,
				id + " quad sits on the face plane")
		var basis := quad.global_transform.basis
		check(basis.z.normalized().dot(face_normal) > 0.9999, id + " quad faces outward")
		var rotation := float(region.rotation)
		var expected_x := (face_right * cos(rotation) + face_up * sin(rotation)).normalized()
		check(basis.x.normalized().dot(expected_x) > 0.9999, id + " quad follows region_rot")

		var material: ShaderMaterial = stage.decal.get_material(id)
		check(material != null, id + " has a shader material")
		if material == null:
			continue
		check(material.shader == load(SHADER_PATH), id + " uses the flat shader")
		check(material.render_priority >= 1, id + " draws after the opaque model")
		check(material.get_shader_parameter("camera_tex") != null, id + " binds the camera texture")
		check(_basis_equal(material.get_shader_parameter("canonical_to_image"),
				stage.decal.canonical_to_image), id + " binds canonical_to_image")
		check(_vec4_equal(material.get_shader_parameter("region_uv"),
				Vector4(region.center.x, region.center.y, region.size.x, region.size.y)),
				id + " binds region_uv")
		check(_vec2_equal(material.get_shader_parameter("region_anchor"), region.anchor),
				id + " binds region_anchor")
		check(is_equal_approx(float(material.get_shader_parameter("region_rot")),
				float(region.rotation)), id + " binds region_rot")
		check(is_equal_approx(float(material.get_shader_parameter("feather")),
				float(region.feather)), id + " binds feather")
		check(is_equal_approx(float(material.get_shader_parameter("opacity")),
				float(region.opacity)), id + " binds opacity")
		check(_vec3_equal(material.get_shader_parameter("colour_gain"), region.gain),
				id + " binds colour_gain")
		check(_vec3_equal(material.get_shader_parameter("colour_lift"), region.lift),
				id + " binds colour_lift")
		check(int(material.get_shader_parameter("region_shape")) == int(region.shape),
				id + " binds the region shape")
		check(_vec3_equal(material.get_shader_parameter("face_origin"), face_origin),
				id + " binds the world face origin")
		check(_vec3_equal(material.get_shader_parameter("face_right"), face_right),
				id + " binds the world face right")
		check(_vec3_equal(material.get_shader_parameter("face_up"), face_up),
				id + " binds the world face up")
		check(is_equal_approx(float(material.get_shader_parameter("face_scale")), face_scale),
				id + " binds the world face scale")

	var probe := ImageTexture.create_from_image(Fixtures.make_solid_image(0.25, 8))
	stage.decal.set_camera_texture(probe)
	for id in ids:
		check(stage.decal.get_material(id).get_shader_parameter("camera_tex") == probe,
				"camera texture propagates to " + id)
	var homography := Fixtures.make_homography(0.5, 0.1, 0.05)
	check(stage.decal.set_canonical_to_image(homography), "homography accepted")
	for id in ids:
		check(_basis_equal(stage.decal.get_material(id).get_shader_parameter("canonical_to_image"),
				homography), "homography propagates to " + id)
	check(stage.decal.set_region_anchor("eye_l", Vector2(0.1, -0.05)), "anchor accepted")
	check(_vec2_equal(stage.decal.get_material("eye_l").get_shader_parameter("region_anchor"),
			Vector2(0.1, -0.05)), "anchor propagates")
	check(stage.decal.set_region_anchor("eye_l", Vector2.ZERO), "anchor reset")
	check(not stage.decal.set_region_anchor("nope", Vector2.ZERO), "unknown anchor rejected")
	check(not stage.decal.set_region_opacity("mouth", 2.0), "opacity outside [0,1] rejected")
	check(stage.decal.set_region_enabled("mouth", false), "region can be hidden")
	check(not stage.decal.get_quad("mouth").visible, "hidden region is not drawn")
	check(stage.decal.set_region_enabled("mouth", true), "region can be shown")
	check(not stage.decal.set_canonical_to_image(Basis(Vector3.ZERO, Vector3.ZERO, Vector3.ZERO)),
			"degenerate homography rejected")

	_validate_rejections()
	stage.set_camera_image(Fixtures.make_camera_image())
	stage.set_homography(Fixtures.make_homography())


func _validate_rejections() -> void:
	var temp := Node3D.new()
	root.add_child(temp)
	var manager = FaceDecalFlatScript.new()
	check(not manager.build(null, stage.face, stage.regions), "null head rejected")
	check(not manager.build(temp, {}, stage.regions), "empty face rejected")
	var bad_face: Dictionary = stage.face.duplicate(true)
	bad_face.scale = 0.0
	check(not manager.build(temp, bad_face, stage.regions), "zero face scale rejected")
	bad_face = stage.face.duplicate(true)
	bad_face.up = stage.face.normal
	check(not manager.build(temp, bad_face, stage.regions), "parallel face up rejected")
	bad_face = stage.face.duplicate(true)
	bad_face.origin = Vector3(NAN, 0.0, 0.0)
	check(not manager.build(temp, bad_face, stage.regions), "non-finite face rejected")
	var bad_regions: Array = stage.regions.duplicate(true)
	bad_regions[0].id = "nose"
	check(not manager.build(temp, stage.face, bad_regions), "unknown region rejected")
	bad_regions = stage.regions.duplicate(true)
	bad_regions[0].size = Vector2.ZERO
	check(not manager.build(temp, stage.face, bad_regions), "zero region size rejected")
	bad_regions = stage.regions.duplicate(true)
	bad_regions[0].feather = 2.0
	check(not manager.build(temp, stage.face, bad_regions), "invalid feather rejected")
	bad_regions = stage.regions.duplicate(true)
	bad_regions.append(stage.regions[0].duplicate(true))
	check(not manager.build(temp, stage.face, bad_regions), "duplicate region rejected")
	check(not manager.get_last_error().is_empty(), "validation exposes an error")
	check(manager.build(temp, stage.face, stage.regions), "temporary valid build")
	check(manager.get_quad_ids().size() == 3, "temporary build has three quads")
	check(temp.get_child_count() == 3, "quads are children of the temporary head")
	var invalid_rebuild: Array = stage.regions.duplicate(true)
	invalid_rebuild[0].size = Vector2.ZERO
	check(not manager.build(temp, stage.face, invalid_rebuild), "invalid rebuild rejected")
	check(manager.is_built() and manager.get_quad_ids().size() == 3,
			"invalid rebuild preserves the previous pass")
	var rotated_regions: Array = stage.regions.duplicate(true)
	rotated_regions[0].rotation = deg_to_rad(30.0)
	check(manager.build(temp, stage.face, rotated_regions), "rotated region build")
	var rotated_quad: MeshInstance3D = manager.get_quad("eye_l")
	var rotation := deg_to_rad(30.0)
	var expected_x := ((stage.face.right as Vector3) * cos(rotation)
			+ (stage.face.up as Vector3) * sin(rotation)).normalized()
	check(rotated_quad.global_transform.basis.x.normalized().dot(expected_x) > 0.9999,
			"rotated quad follows region_rot")
	check(is_equal_approx(float(manager.get_material("eye_l").get_shader_parameter("region_rot")),
			rotation), "rotated region binds region_rot")
	manager.clear()
	check(manager.get_quad_ids().is_empty(), "clear removes the quad list")
	check(temp.get_child_count() == 0, "clear removes the quad nodes")
	manager = null
	temp.free()


func _check_render() -> void:
	result["rendered"] = true
	var coverage_h := Fixtures.make_homography()
	var coverage_image := Fixtures.make_camera_image()
	var black_image := Fixtures.make_black_image()

	var pair := await _render_pair(coverage_image, coverage_h, true)
	var actual: Image = pair[0]
	var base: Image = pair[1]
	var reference := _reference_image(base, coverage_image, coverage_h)
	_check_golden("coverage", actual, reference)
	result["avocado_pixels"] = _non_background_pixels(actual)
	check(int(result["avocado_pixels"]) > 1000, "avocado renders with the synthetic decal")
	_save_actual("avocado", actual)

	var black_pair := await _render_pair(black_image, coverage_h, true)
	var black_actual: Image = black_pair[0]
	var changed := _changed_pixels(actual, black_actual)
	result["changed_pixels"] = changed.size()
	check(changed.size() > 200, "black-camera diff changes a meaningful area")
	var maps := _screen_quads()
	var outside := 0
	var outside_samples: Array = []
	var per_region := {"eye_l": 0, "eye_r": 0, "mouth": 0}
	for pixel in changed:
		var screen := Vector2(pixel.x + 0.5, pixel.y + 0.5)
		var best := _best_region(maps, screen)
		if float(best.mask) <= 0.0:
			outside += 1
			if outside_samples.size() < 8:
				outside_samples.append([pixel, best.mask])
		elif per_region.has(str(best.region.id)):
			per_region[str(best.region.id)] += 1
	check(outside == 0, "every changed pixel lies inside a region mask")
	for id in per_region:
		check(int(per_region[id]) > 20, "changed pixels reach region " + str(id))
	result["mask_diff"] = outside == 0 and changed.size() > 200
	result["mask_outside"] = outside
	print("T20 mask diff: changed=", changed.size(), " outside=", outside,
			" per_region=", per_region, " samples=", outside_samples)

	check(stage.decal.set_region_opacity("mouth", 0.5), "mouth opacity can be lowered")
	var opacity_pair := await _render_pair(coverage_image, coverage_h, true)
	var opacity_reference := _reference_image(opacity_pair[1], coverage_image, coverage_h)
	var opacity_error := _mean_error(opacity_pair[0], opacity_reference)
	check(opacity_error < GOLDEN_MEAN_ERROR, "opacity is applied: %f" % opacity_error)
	check(stage.decal.set_region_opacity("mouth", 1.0), "mouth opacity restored")
	result["opacity"] = opacity_error < GOLDEN_MEAN_ERROR

	var edge_h := Fixtures.make_homography(0.45, 0.6, -0.15)
	var edge_pair := await _render_pair(coverage_image, edge_h, true)
	var edge_actual: Image = edge_pair[0]
	var edge_base: Image = edge_pair[1]
	var edge_reference := _reference_image(edge_base, coverage_image, edge_h)
	_check_golden("edge-clamp", edge_actual, edge_reference)
	var edge_black := await _render_single(black_image, edge_h, true, true)
	var edge_changed := _changed_pixels(edge_actual, edge_black)
	var edge_outside_uv := 0
	for pixel in edge_changed:
		var best := _best_region(maps, Vector2(pixel.x + 0.5, pixel.y + 0.5))
		if float(best.mask) <= 0.0:
			continue
		var uv := _image_uv(best.canonical, edge_h)
		if uv.x < 0.0 or uv.x > 1.0 or uv.y < 0.0 or uv.y > 1.0:
			edge_outside_uv += 1
	var mask_outside_uv := 0
	for y in range(VIEWPORT_SIZE.y):
		for x in range(VIEWPORT_SIZE.x):
			var best := _best_region(maps, Vector2(x + 0.5, y + 0.5))
			if float(best.mask) <= 0.0:
				continue
			var uv := _image_uv(best.canonical, edge_h)
			if uv.x < 0.0 or uv.x > 1.0 or uv.y < 0.0 or uv.y > 1.0:
				mask_outside_uv += 1
	check(edge_outside_uv == 0, "no changed pixel samples outside the camera frame")
	check(mask_outside_uv > 20, "edge-clamp scenario exercises out-of-frame mask area")
	result["edge_clamp"] = edge_outside_uv == 0 and mask_outside_uv > 20

	var marker_image := Fixtures.make_marker_image()
	var mirror_pair := await _render_pair(marker_image, coverage_h, true)
	var mirror_actual: Image = mirror_pair[0]
	var mirror_base: Image = mirror_pair[1]
	var mirror_reference := _reference_image(mirror_base, marker_image, coverage_h)
	_check_golden("mirroring", mirror_actual, mirror_reference)
	var marker_canonical := Vector2(
			(0.5 - Fixtures.MARKER_IMAGE.x) / Fixtures.COVERAGE_K + Fixtures.COVERAGE_U0,
			Fixtures.COVERAGE_V0 + (0.5 - Fixtures.MARKER_IMAGE.y) / Fixtures.COVERAGE_K)
	var marker_screen := _screen_of_canonical("eye_r", marker_canonical)
	var mirrored_screen := _screen_of_canonical("eye_l", Vector2(-marker_canonical.x,
			marker_canonical.y))
	var marker_mask := float(_best_region(maps, marker_screen).mask)
	var marker_colour := _sample_rgb(mirror_actual, marker_screen)
	var mirrored_colour := _sample_rgb(mirror_actual, mirrored_screen)
	check(marker_mask > 0.5, "mirror marker is inside the eye_r mask")
	check(marker_colour.x > 0.5 and marker_colour.z > 0.5 and marker_colour.y < 0.35,
			"unmirrored marker appears on the subject's right eye")
	check(not (mirrored_colour.x > 0.5 and mirrored_colour.z > 0.5 and mirrored_colour.y < 0.35),
			"marker does not appear on the mirrored eye")
	check(marker_screen.x < VIEWPORT_SIZE.x * 0.5,
			"subject's right eye is on the viewer's left")
	result["mirroring"] = marker_mask > 0.5 \
			and marker_colour.x > 0.5 and marker_colour.z > 0.5 and marker_colour.y < 0.35 \
			and marker_screen.x < VIEWPORT_SIZE.x * 0.5

	stage.set_model_visible(false)
	var solid_image := Fixtures.make_solid_image(0.5)
	var colour_pair := await _render_pair(solid_image, coverage_h, false)
	var colour_actual: Image = colour_pair[0]
	var colour_base: Image = colour_pair[1]
	var colour_reference := _reference_image(colour_base, solid_image, coverage_h)
	_check_golden("colour-space", colour_actual, colour_reference)
	var expected := Fixtures.srgb_to_linear(0.5)
	var colour_ok := true
	for region in stage.regions:
		var centre := _screen_of_canonical(region.id, region.center)
		var measured := _sample_rgb(colour_actual, centre).x
		check(absf(measured - expected) < 0.03,
				"%s centre is linear %.3f (expected %.3f)" % [region.id, measured, expected])
		check(absf(measured - 0.5) > 0.15, "%s does not pass raw sRGB" % region.id)
		if absf(measured - expected) >= 0.03 or absf(measured - 0.5) <= 0.15:
			colour_ok = false
	result["colour_space"] = colour_ok
	result["colour_space_linear"] = expected
	stage.set_model_visible(true)


func _render_pair(image: Image, homography: Basis, model_visible: bool) -> Array:
	var actual := await _render_single(image, homography, model_visible, true)
	var base := await _render_single(image, homography, model_visible, false)
	stage.set_decal_visible(true)
	return [actual, base]


func _render_single(image: Image, homography: Basis, model_visible: bool,
		decal_visible: bool) -> Image:
	stage.set_camera_image(image)
	stage.set_homography(homography)
	stage.set_model_visible(model_visible)
	stage.set_decal_visible(decal_visible)
	await process_frame
	await process_frame
	await RenderingServer.frame_post_draw
	return viewport.get_texture().get_image()


func _reference_image(base: Image, image: Image, homography: Basis) -> Image:
	var width := base.get_width()
	var height := base.get_height()
	var output := Image.create(width, height, false, Image.FORMAT_RGBA8)
	var maps := _screen_quads()
	for y in range(height):
		for x in range(width):
			var background := base.get_pixel(x, y)
			var colour := Vector3(background.r, background.g, background.b)
			var screen := Vector2(x + 0.5, y + 0.5)
			for region in stage.regions:
				var map: Dictionary = maps[region.id]
				var d := _region_d(map, screen)
				var mask := _mask_from_d(d, region)
				if mask <= 0.0:
					continue
				var canonical := _canonical(map, screen)
				var uv := _image_uv(canonical, homography)
				if uv.x < 0.0 or uv.x > 1.0 or uv.y < 0.0 or uv.y > 1.0:
					continue
				var sample := Fixtures.sample_bilinear(image, uv)
				var linear := Fixtures.srgb_to_linear_rgb(sample)
				var decal: Vector3 = linear * (region.gain as Vector3) + (region.lift as Vector3)
				var alpha := mask * float(region.opacity)
				colour = decal * alpha + colour * (1.0 - alpha)
			output.set_pixel(x, y, Color(clampf(colour.x, 0.0, 1.0),
					clampf(colour.y, 0.0, 1.0), clampf(colour.z, 0.0, 1.0), 1.0))
	return output


func _screen_quads() -> Dictionary:
	var maps: Dictionary = {}
	for region in stage.regions:
		var quad: MeshInstance3D = stage.decal.get_quad(region.id)
		var mesh := quad.mesh as QuadMesh
		var half := mesh.size * 0.5
		var transform := quad.global_transform
		var centre := camera.unproject_position(transform * Vector3.ZERO)
		var ex := camera.unproject_position(transform * Vector3(half.x, 0.0, 0.0)) - centre
		var ey := camera.unproject_position(transform * Vector3(0.0, half.y, 0.0)) - centre
		var det := ex.x * ey.y - ex.y * ey.x
		maps[region.id] = {
			"region": region,
			"centre": centre,
			"ex": ex,
			"ey": ey,
			"det": det,
		}
	return maps


func _region_ab(map: Dictionary, screen: Vector2) -> Vector2:
	var p: Vector2 = screen - map.centre
	var ex: Vector2 = map.ex
	var ey: Vector2 = map.ey
	var det: float = map.det
	if absf(det) < 1e-9:
		return Vector2(INF, INF)
	return Vector2((p.x * ey.y - p.y * ey.x) / det, (ex.x * p.y - ex.y * p.x) / det)


func _region_d(map: Dictionary, screen: Vector2) -> Vector2:
	var ab := _region_ab(map, screen)
	var region: Dictionary = map.region
	var c := cos(float(region.rotation))
	var s := sin(float(region.rotation))
	var anchor: Vector2 = region.anchor
	var rotated := Vector2(c * anchor.x + s * anchor.y, -s * anchor.x + c * anchor.y)
	return ab - Vector2(rotated.x / (region.size.x * 0.5), rotated.y / (region.size.y * 0.5))


func _canonical(map: Dictionary, screen: Vector2) -> Vector2:
	var ab := _region_ab(map, screen)
	var region: Dictionary = map.region
	var local := ab * Vector2(region.size.x, region.size.y) * 0.5
	var c := cos(float(region.rotation))
	var s := sin(float(region.rotation))
	return Vector2(region.center.x, region.center.y) \
			+ Vector2(c * local.x - s * local.y, s * local.x + c * local.y)


func _mask_from_d(d: Vector2, region: Dictionary) -> float:
	var sd := 0.0
	if int(region.shape) == 1:
		var radius := float(region.corner_radius)
		var q := Vector2(absf(d.x) - (1.0 - radius), absf(d.y) - (1.0 - radius))
		sd = Vector2(maxf(q.x, 0.0), maxf(q.y, 0.0)).length() \
				+ minf(maxf(q.x, q.y), 0.0) - radius
	else:
		sd = d.length() - 1.0
	var f := maxf(float(region.feather), 0.00001)
	return 1.0 - smoothstep(-f, 0.0, sd)


func _best_region(maps: Dictionary, screen: Vector2) -> Dictionary:
	var best := {"mask": 0.0, "region": null, "canonical": Vector2.ZERO}
	for region in stage.regions:
		var map: Dictionary = maps[region.id]
		var mask := _mask_from_d(_region_d(map, screen), region)
		if mask > float(best.mask):
			best = {"mask": mask, "region": region, "canonical": _canonical(map, screen)}
	return best


func _image_uv(canonical: Vector2, homography: Basis) -> Vector2:
	var h := homography * Vector3(canonical.x, canonical.y, 1.0)
	if h.z == 0.0:
		return Vector2(-1.0, -1.0)
	return Vector2(h.x / h.z, h.y / h.z)


func _screen_of_canonical(id: String, canonical: Vector2) -> Vector2:
	var region: Dictionary = stage.decal.get_region(id)
	var quad: MeshInstance3D = stage.decal.get_quad(id)
	var mesh := quad.mesh as QuadMesh
	var half := mesh.size * 0.5
	var a: float = (canonical.x - region.center.x) / (region.size.x * 0.5)
	var b: float = (canonical.y - region.center.y) / (region.size.y * 0.5)
	return camera.unproject_position(quad.global_transform * Vector3(a * half.x, b * half.y, 0.0))


func _sample_rgb(image: Image, screen: Vector2) -> Vector3:
	var x := clampi(roundi(screen.x), 0, image.get_width() - 1)
	var y := clampi(roundi(screen.y), 0, image.get_height() - 1)
	var total := Vector3.ZERO
	var count := 0
	for dy in range(-1, 2):
		for dx in range(-1, 2):
			var px := clampi(x + dx, 0, image.get_width() - 1)
			var py := clampi(y + dy, 0, image.get_height() - 1)
			var colour := image.get_pixel(px, py)
			total += Vector3(colour.r, colour.g, colour.b)
			count += 1
	return total / float(count)


func _changed_pixels(a: Image, b: Image) -> Array[Vector2i]:
	var changed: Array[Vector2i] = []
	if a.get_size() != b.get_size():
		return changed
	for y in range(a.get_height()):
		for x in range(a.get_width()):
			var pa := a.get_pixel(x, y)
			var pb := b.get_pixel(x, y)
			var delta := maxf(absf(pa.r - pb.r), maxf(absf(pa.g - pb.g), absf(pa.b - pb.b)))
			if delta > CHANGED_THRESHOLD:
				changed.append(Vector2i(x, y))
	return changed


func _non_background_pixels(image: Image) -> int:
	var count := 0
	var background := Color(0.03, 0.03, 0.03)
	for y in range(image.get_height()):
		for x in range(image.get_width()):
			var colour := image.get_pixel(x, y)
			if absf(colour.r - background.r) + absf(colour.g - background.g) \
					+ absf(colour.b - background.b) > 0.12:
				count += 1
	return count


func _mean_error(a: Image, b: Image) -> float:
	if a.get_size() != b.get_size():
		return 1.0
	var total := 0.0
	for y in range(a.get_height()):
		for x in range(a.get_width()):
			var pa := a.get_pixel(x, y)
			var pb := b.get_pixel(x, y)
			total += absf(pa.r - pb.r) + absf(pa.g - pb.g) + absf(pa.b - pb.b)
	return total / float(a.get_width() * a.get_height() * 3)


func _check_golden(name: String, actual: Image, reference: Image) -> void:
	var error := _mean_error(actual, reference)
	check(error < GOLDEN_MEAN_ERROR, "%s matches the analytic reference: %f" % [name, error])
	_save_actual(name + "-actual", actual)
	_save_actual(name + "-reference", reference)
	result["scenarios"] = int(result["scenarios"]) + 1
	var path := ProjectSettings.globalize_path(GOLDEN_DIR + "/" + name + ".png")
	if update_goldens:
		DirAccess.make_dir_recursive_absolute(path.get_base_dir())
		check(reference.save_png(path) == OK, "write golden " + name)
		result["goldens"] = int(result["goldens"]) + 1
	else:
		var stored := Image.load_from_file(path)
		check(stored != null and not stored.is_empty(), "golden exists " + name)
		if stored != null and not stored.is_empty():
			check(_mean_error(actual, stored) < GOLDEN_MEAN_ERROR, "matches golden " + name)
			result["goldens"] = int(result["goldens"]) + 1


func _save_actual(name: String, image: Image) -> void:
	var path := ProjectSettings.globalize_path("res://../.tools/T20-face-decal-" + name + ".png")
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	image.save_png(path)


func _write_result() -> void:
	var suffix := "-render" if rendering else ""
	var path := ProjectSettings.globalize_path("res://../.tools/T20-face-decal%s.json" % suffix)
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(JSON.stringify(result, "  "))
	print("T20 ", JSON.stringify(result))


func _uniform_scale(basis: Basis) -> float:
	var scale := basis.get_scale()
	return (absf(scale.x) + absf(scale.y) + absf(scale.z)) / 3.0


func _basis_equal(a: Variant, b: Basis) -> bool:
	if not (a is Basis):
		return false
	var basis: Basis = a
	return basis.x.is_equal_approx(b.x) and basis.y.is_equal_approx(b.y) \
			and basis.z.is_equal_approx(b.z)


func _vec2_equal(a: Variant, b: Vector2) -> bool:
	if not (a is Vector2):
		return false
	return (a as Vector2).is_equal_approx(b)


func _vec3_equal(a: Variant, b: Vector3) -> bool:
	if not (a is Vector3):
		return false
	return (a as Vector3).is_equal_approx(b)


func _vec4_equal(a: Variant, b: Vector4) -> bool:
	if not (a is Vector4):
		return false
	var v: Vector4 = a
	return is_equal_approx(v.x, b.x) and is_equal_approx(v.y, b.y) \
			and is_equal_approx(v.z, b.z) and is_equal_approx(v.w, b.w)
