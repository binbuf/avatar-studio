extends SceneTree

# T22 presentation and framing test.
#
# Run headless:
#   <godot> --headless --path app --script res://tests/framing.gd
# Render the golden cases in a hidden real window:
#   <godot> --path app --rendering-method gl_compatibility --resolution 320x320 \
#       --script res://tests/framing.gd -- --render
#
# The headless phase checks the framing contract, clamps, extreme containment,
# head fit, the bounds overlay and mirroring. The render phase adds an
# independent projection oracle: it recomputes the documented swept bound, fit,
# distance and view matrix in GDScript, projects a model-space marker and the
# model AABB, and compares them to the live SubViewport image. Committed PNGs
# under tests/golden/framing are regression baselines; the oracle is what proves
# the framing math independently of the native result.

const SIZE := Vector2i(320, 320)
const FOV_DEGREES := 35.0
const GOLDEN_ERROR := 0.005
const MARKER_PIXEL_ERROR := 2.5
const CASES := [
	{"name": "model-default", "fit": "model", "zoom": 1.0, "margin": 0.10, "offset": Vector2.ZERO},
	{"name": "model-tight", "fit": "model", "zoom": 1.0, "margin": 0.0, "offset": Vector2.ZERO},
	{"name": "model-loose", "fit": "model", "zoom": 1.0, "margin": 0.5, "offset": Vector2.ZERO},
	{"name": "model-zoom-out", "fit": "model", "zoom": 0.5, "margin": 0.10, "offset": Vector2.ZERO},
	{"name": "model-zoom-in", "fit": "model", "zoom": 2.0, "margin": 0.10, "offset": Vector2.ZERO},
	{"name": "model-offset", "fit": "model", "zoom": 1.0, "margin": 0.10, "offset": Vector2(0.25, -0.15)},
	{"name": "head-fit", "fit": "head", "zoom": 1.0, "margin": 0.10, "offset": Vector2.ZERO},
	{"name": "mirror-output", "fit": "model", "zoom": 1.0, "margin": 0.10, "offset": Vector2.ZERO},
]

var failures: Array[String] = []
var result: Dictionary = {"schema": "avatar_framing.result.v1", "cases": []}
var rendering := false
var update_goldens := false
var scene: Node
var runtime: Object
var viewport: SubViewport
var preview: TextureRect
var model: Node3D
var marker: MeshInstance3D
var marker_position := Vector3.ZERO
var model_aabb := AABB()
var head_aabb := AABB()
var head_pivot := Vector3.ZERO
var face_origin := Vector3.ZERO
var face_scale := 0.0


func _initialize() -> void:
	rendering = "--render" in OS.get_cmdline_user_args()
	update_goldens = "--update-goldens" in OS.get_cmdline_user_args()
	_run.call_deferred()


func _check(condition: bool, message: String) -> void:
	if condition:
		print("  ok   ", message)
	else:
		failures.append(message)
		printerr("  FAIL ", message)


func _call(object: Object, method: String, argument: Variant = null) -> Variant:
	if argument == null:
		return object.call(method)
	return object.call(method, argument)


func _aabb_corners(aabb: AABB) -> Array[Vector3]:
	var out: Array[Vector3] = []
	for z in [aabb.position.z, aabb.position.z + aabb.size.z]:
		for y in [aabb.position.y, aabb.position.y + aabb.size.y]:
			for x in [aabb.position.x, aabb.position.x + aabb.size.x]:
				out.append(Vector3(x, y, z))
	return out


func _oracle(settings: Dictionary) -> Dictionary:
	# Independent re-implementation of the documented T22 formulas. It reads
	# raw model/head/face data, never the native framing result.
	var theta_v := deg_to_rad(FOV_DEGREES * 0.5)
	var theta_h := atan(tan(theta_v) * (float(SIZE.x) / float(SIZE.y)))
	var theta := minf(theta_v, theta_h)
	var swept := model_aabb
	if head_aabb.has_volume():
		var sweep_radius := 0.0
		for corner in _aabb_corners(head_aabb):
			sweep_radius = maxf(sweep_radius, corner.distance_to(head_pivot))
		swept = swept.merge(AABB(head_pivot - Vector3.ONE * sweep_radius,
			Vector3.ONE * (2.0 * sweep_radius)))
	var fit := String(settings.get("fit", "model"))
	var centre := swept.get_center()
	var radius := swept.size.length() * 0.5
	if fit == "head":
		centre = face_origin
		radius = 1.5 * face_scale
	var zoom := clampf(float(settings.get("zoom", 1.0)), 0.5, 2.0)
	var margin := clampf(float(settings.get("margin", 0.10)), 0.0, 0.5)
	var offset := Vector2(settings.get("offset", Vector2.ZERO))
	var distance := (radius / sin(theta)) * (1.0 + margin) / zoom
	var half_height := distance * tan(theta_v)
	var half_width := half_height * (float(SIZE.x) / float(SIZE.y))
	var offset_world := Vector3(offset.x * half_width, offset.y * half_height, 0.0)
	var basis := Basis(Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, -1))
	var view_from_model := Transform3D(basis, offset_world) * Transform3D(Basis(), -centre)
	var camera := Transform3D(Basis(), Vector3(0, 0, distance))
	var near := maxf(0.001, distance - radius * 1.5)
	var far := maxf(near + 0.001, distance + radius * 1.5 + 0.5)
	var projection := Projection.create_perspective(FOV_DEGREES,
		float(SIZE.x) / float(SIZE.y), near, far)
	var view := camera.affine_inverse() * view_from_model
	return {
		"centre": centre,
		"radius": radius,
		"distance": distance,
		"swept": swept,
		"theta": theta,
		"view_from_model": view_from_model,
		"clip": projection * Projection(view),
		"half": Vector2(half_width, half_height),
	}


func _project(clip: Projection, point: Vector3) -> Vector2:
	var v: Vector4 = clip * Vector4(point.x, point.y, point.z, 1.0)
	if absf(v.w) < 1e-9:
		return Vector2(-1e6, -1e6)
	return Vector2((v.x / v.w * 0.5 + 0.5) * SIZE.x, (0.5 - v.y / v.w * 0.5) * SIZE.y)


func _projected_bbox(clip: Projection, aabb: AABB) -> Rect2:
	var minimum := Vector2(INF, INF)
	var maximum := Vector2(-INF, -INF)
	for corner in _aabb_corners(aabb):
		var p := _project(clip, corner)
		minimum = Vector2(minf(minimum.x, p.x), minf(minimum.y, p.y))
		maximum = Vector2(maxf(maximum.x, p.x), maxf(maximum.y, p.y))
	return Rect2(minimum, maximum - minimum)


func _capture() -> Image:
	await process_frame
	await RenderingServer.frame_post_draw
	return viewport.get_texture().get_image()


func _difference(a: Image, b: Image) -> float:
	if a.get_size() != b.get_size():
		return 1.0
	var total := 0.0
	for y in range(a.get_height()):
		for x in range(a.get_width()):
			var av := a.get_pixel(x, y)
			var bv := b.get_pixel(x, y)
			total += absf(av.r - bv.r) + absf(av.g - bv.g) + absf(av.b - bv.b)
	return total / (a.get_width() * a.get_height() * 3.0)


func _marker_centroid(image: Image) -> Dictionary:
	var total := Vector2.ZERO
	var count := 0
	for y in range(image.get_height()):
		for x in range(image.get_width()):
			var c := image.get_pixel(x, y)
			if c.r > 0.55 and c.b > 0.55 and c.g < 0.35:
				total += Vector2(x, y)
				count += 1
	if count == 0:
		return {"centre": Vector2(-1, -1), "count": 0}
	return {"centre": total / count, "count": count}


func _silhouette_bbox(image: Image) -> Rect2i:
	var minimum := Vector2i(image.get_width(), image.get_height())
	var maximum := Vector2i(-1, -1)
	for y in range(image.get_height()):
		for x in range(image.get_width()):
			var c := image.get_pixel(x, y)
			if c.r + c.g + c.b <= 0.02:
				continue
			if c.r > 0.55 and c.b > 0.55 and c.g < 0.35:
				continue
			minimum = Vector2i(mini(minimum.x, x), mini(minimum.y, y))
			maximum = Vector2i(maxi(maximum.x, x), maxi(maximum.y, y))
	if maximum.x < 0:
		return Rect2i()
	return Rect2i(minimum, maximum - minimum + Vector2i.ONE)


func _setup_scene() -> void:
	var packed: PackedScene = load("res://scenes/main.tscn")
	scene = packed.instantiate()
	root.add_child(scene)
	await process_frame
	await process_frame
	runtime = scene.get_node("AvatarRuntime")
	viewport = scene.get_node("SubViewport") as SubViewport
	preview = scene.get_node("Preview") as TextureRect
	model = runtime.call("get_avatar_model") as Node3D
	_check(runtime != null, "main scene has AvatarRuntime")
	_check(viewport != null, "main scene has SubViewport")
	_check(preview != null, "main scene has Preview")
	_check(model != null, "runtime owns an AvatarModel")
	if runtime == null or viewport == null or preview == null or model == null:
		return
	viewport.size = SIZE
	viewport.transparent_bg = false
	var world := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0, 0, 0)
	world.environment = environment
	viewport.add_child(world)
	runtime.call("refresh_framing")
	model_aabb = model.call("get_model_aabb")
	head_aabb = model.call("get_head_aabb")
	var region: Dictionary = model.call("get_head_region")
	head_pivot = region.get("pivot", model_aabb.get_center())
	var face: Dictionary = model.call("get_face_surface")
	face_origin = face.get("origin", model_aabb.get_center())
	face_scale = float(face.get("scale", model_aabb.size.y * 0.2))
	marker_position = model_aabb.position + Vector3(model_aabb.size.x * 0.72,
		model_aabb.size.y * 0.62, model_aabb.size.z * 0.02)
	_add_marker()
	await process_frame
	await process_frame


func _add_marker() -> void:
	marker = MeshInstance3D.new()
	marker.name = "FramingMarker"
	var sphere := SphereMesh.new()
	var radius := maxf(model_aabb.size.x, model_aabb.size.y) * 0.03
	sphere.radius = radius
	sphere.height = radius * 2.0
	sphere.radial_segments = 8
	sphere.rings = 4
	marker.mesh = sphere
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.albedo_color = Color(1, 0, 1)
	material.set_flag(BaseMaterial3D.FLAG_DISABLE_DEPTH_TEST, true)
	material.render_priority = 100
	marker.material_override = material
	marker.position = marker_position
	model.add_child(marker)


func _configure(fit: String, zoom: float, margin: float, offset: Vector2) -> bool:
	return bool(runtime.call("configure_framing", {
		"fit": fit, "zoom": zoom, "margin": margin, "offset": offset,
	}))


func _test_default_and_overlay() -> void:
	print("framing: default fit and bounds overlay")
	var framing: Dictionary = runtime.call("get_framing")
	_check(framing.get("fit", "") == "model", "default fit is model")
	_check(is_equal_approx(float(framing.get("zoom", 0.0)), 1.0), "default zoom is 1.0")
	_check(is_equal_approx(float(framing.get("margin", 0.0)), 0.10), "default margin is 0.10")
	_check(Vector2(framing.get("offset", Vector2.ONE)) == Vector2.ZERO, "default offset is zero")
	var oracle := _oracle({"fit": "model", "zoom": 1.0, "margin": 0.10})
	_check(absf(float(framing.get("distance", 0.0)) - float(oracle.distance)) < 1e-6,
		"distance matches the documented formula (%.9f vs %.9f)" % [
			float(framing.get("distance", 0.0)), float(oracle.distance)])
	_check(absf(float(framing.get("fitted_radius", 0.0)) - float(oracle.radius)) < 1e-6,
		"fitted radius matches the independent swept sphere")
	_check(Vector3(framing.get("fitted_centre", Vector3.ZERO)).distance_to(oracle.centre) < 1e-6,
		"fitted centre matches the independent swept sphere")
	_check(bool(framing.get("valid", false)), "framing is valid")

	var children := viewport.get_child_count()
	var overlay: Dictionary = runtime.call("get_bounds_overlay")
	_check(overlay.get("overlay_is_output", true) == false,
		"bounds overlay is not part of the output")
	_check(overlay.get("schema", "") == "avatar_framing.overlay.v1", "overlay schema")
	var overlay_aabb: AABB = overlay.get("model_aabb", AABB())
	_check(overlay_aabb.position.is_equal_approx(model_aabb.position) and
		overlay_aabb.size.is_equal_approx(model_aabb.size), "overlay carries the model AABB")
	_check(overlay.get("swept_aabb", AABB()).has_volume(), "overlay carries the swept AABB")
	_check(float(overlay.get("fitted_sphere", {}).get("radius", 0.0)) > 0.0,
		"overlay carries the fitted sphere")
	var sweep: Dictionary = overlay.get("head_sweep", {})
	_check(float(sweep.get("radius", 0.0)) > 0.0, "overlay carries the head sweep radius")
	_check(is_equal_approx(float(sweep.get("sweep_degrees", 0.0)), 45.0),
		"overlay records the +/-45 degree sweep")
	_check(Array(overlay.get("frustum_planes", [])).size() == 6,
		"overlay carries six frustum planes")
	_check(PackedVector3Array(overlay.get("frustum_corners", PackedVector3Array())).size() == 8,
		"overlay carries eight frustum corners")
	_check(overlay.get("fitted_volume_inside", false) == true,
		"fitted volume is inside at the default fit")
	_check(overlay.get("avatar_inside", false) == true,
		"avatar is inside at the default fit")
	_check(viewport.get_child_count() == children,
		"reading the overlay adds nothing to the output viewport")
	_check(viewport.get_node_or_null("BoundsOverlay") == null,
		"the output viewport has no bounds overlay node")


func _test_clamps_and_rejection() -> void:
	print("framing: clamps and atomic rejection")
	_check(_configure("model", 10.0, -1.0, Vector2.ZERO), "finite out-of-range values configure")
	var framing: Dictionary = runtime.call("get_framing")
	_check(is_equal_approx(float(framing.zoom), 2.0), "zoom clamps to 2.0")
	_check(is_equal_approx(float(framing.margin), 0.0), "margin clamps to 0.0")
	_check(_configure("model", 0.1, 3.0, Vector2.ZERO), "second out-of-range pair configures")
	framing = runtime.call("get_framing")
	_check(is_equal_approx(float(framing.zoom), 0.5), "zoom clamps to 0.5")
	_check(is_equal_approx(float(framing.margin), 0.5), "margin clamps to 0.5")
	_check(bool(framing.valid), "clamped framing stays valid")

	var before: Dictionary = runtime.call("get_framing")
	for bad in [
		{"fit": "body"},
		{"zoom": NAN},
		{"margin": INF},
		{"offset": "left"},
		{"offset": [1.0]},
	]:
		_check(not bool(runtime.call("configure_framing", bad)),
			"invalid framing rejected: %s" % [bad])
		_check(String(runtime.call("get_last_error_code")) == "AVT-113",
			"invalid framing reports AVT-113")
		var after: Dictionary = runtime.call("get_framing")
		_check(after.zoom == before.zoom and after.margin == before.margin and
			after.fit == before.fit, "rejected framing preserves the prior values")
	_check(_configure("model", 1.0, 0.10, Vector2.ZERO), "default framing restored")


func _test_extremes_and_head_fit() -> void:
	print("framing: extreme zoom/margin and head fit")
	for zoom in [0.5, 1.0]:
		for margin in [0.0, 0.10, 0.5]:
			_check(_configure("model", zoom, margin, Vector2.ZERO),
				"configure zoom %.1f margin %.2f" % [zoom, margin])
			_check(bool(runtime.call("is_avatar_inside_viewport")),
				"avatar inside at zoom %.1f margin %.2f" % [zoom, margin])
			_check(bool(runtime.call("is_fitted_volume_inside_viewport")),
				"fitted volume inside at zoom %.1f margin %.2f" % [zoom, margin])
			var framing: Dictionary = runtime.call("get_framing")
			var oracle := _oracle({"fit": "model", "zoom": zoom, "margin": margin})
			_check(absf(float(framing.distance) - float(oracle.distance)) < 1e-6,
				"distance matches at zoom %.1f margin %.2f" % [zoom, margin])
	# Zoom above 1 is a deliberate crop; the documented containment guarantee is
	# for zoom <= 1.
	_check(_configure("model", 2.0, 0.10, Vector2.ZERO), "zoom-in configures")
	_check(not bool(runtime.call("is_avatar_inside_viewport")),
		"zoom 2.0 deliberately crops the avatar")
	_check(_configure("model", 1.0, 0.10, Vector2.ZERO), "default restored after zoom-in")

	_check(_configure("head", 1.0, 0.10, Vector2.ZERO), "head fit configures")
	var head: Dictionary = runtime.call("get_framing")
	_check(Vector3(head.fitted_centre).distance_to(face_origin) < 1e-6,
		"head fit centres on the face plane origin")
	_check(absf(float(head.fitted_radius) - 1.5 * face_scale) < 1e-6,
		"head fit radius is 1.5 x inter-ocular distance")
	_check(bool(runtime.call("is_fitted_volume_inside_viewport")),
		"head fit keeps the fitted head sphere inside")
	_check(_configure("model", 1.0, 0.10, Vector2.ZERO), "default restored after head fit")


func _test_mirroring() -> void:
	print("framing: preview and output mirroring")
	_check(bool(runtime.call("is_mirror_preview")), "preview mirroring is on by default")
	_check(not bool(runtime.call("is_mirror_output")), "output mirroring is off by default")
	_check(preview.flip_h, "preview TextureRect is flipped, not the viewport")
	runtime.call("set_mirror_output", true)
	_check(bool(runtime.call("is_mirror_output")), "output mirroring can be enabled")
	runtime.call("set_mirror_output", false)

	var image := Image.create(4, 2, false, Image.FORMAT_RGBA8)
	image.fill(Color(0, 0, 0, 1))
	image.set_pixel(1, 0, Color(1, 0, 1, 1))
	var flipped: Image = runtime.call("mirror_frame", image) as Image
	_check(flipped != null, "mirror_frame returns an image")
	if flipped == null:
		return
	_check(flipped.get_pixel(2, 0).r > 0.9 and flipped.get_pixel(1, 0).r < 0.1,
		"mirror_frame flips the final frame horizontally")
	_check(flipped.get_pixel(2, 0).b > 0.9, "mirror_frame preserves the marker colour")


func _test_offset_direction() -> void:
	print("framing: offset direction")
	_check(_configure("model", 1.0, 0.10, Vector2.ZERO), "offset baseline configures")
	var baseline := _project(_oracle({"fit": "model", "zoom": 1.0, "margin": 0.10}).clip,
		marker_position)
	_check(_configure("model", 1.0, 0.10, Vector2(0.25, 0.0)), "positive offset configures")
	var right := _project(_oracle({"fit": "model", "zoom": 1.0, "margin": 0.10,
		"offset": Vector2(0.25, 0.0)}).clip, marker_position)
	_check(_configure("model", 1.0, 0.10, Vector2(-0.25, 0.0)), "negative offset configures")
	var left := _project(_oracle({"fit": "model", "zoom": 1.0, "margin": 0.10,
		"offset": Vector2(-0.25, 0.0)}).clip, marker_position)
	_check(right.x > baseline.x and left.x < baseline.x,
		"positive offset x moves the avatar to the viewer's right")
	_check(_configure("model", 1.0, 0.10, Vector2(0.0, 0.25)), "vertical offset configures")
	var up := _project(_oracle({"fit": "model", "zoom": 1.0, "margin": 0.10,
		"offset": Vector2(0.0, 0.25)}).clip, marker_position)
	_check(up.y < baseline.y, "positive offset y moves the avatar up")
	_check(_configure("model", 1.0, 0.10, Vector2.ZERO), "default restored after offsets")


func _render_case(case: Dictionary) -> void:
	var name := String(case.name)
	_check(_configure(String(case.fit), float(case.zoom), float(case.margin),
		Vector2(case.offset)), "render configure %s" % name)
	await process_frame
	var actual := await _capture()
	var oracle := _oracle(case)
	var image: Image = actual
	var mirror := name == "mirror-output"
	if mirror:
		image = runtime.call("mirror_frame", actual) as Image
		_check(image != null and not image.is_empty(), "mirror-output produced a frame")

	var observed := _marker_centroid(image)
	var expected_marker := _project(oracle.clip, marker_position)
	if mirror:
		expected_marker.x = SIZE.x - 1.0 - expected_marker.x
	_check(int(observed.count) > 4, "%s marker is visible (%d px)" % [name, observed.count])
	_check(Vector2(observed.centre).distance_to(expected_marker) <= MARKER_PIXEL_ERROR,
		"%s marker matches the independent projection (%.2f px)" % [
			name, Vector2(observed.centre).distance_to(expected_marker)])

	var silhouette := _silhouette_bbox(image)
	_check(silhouette.size.x > 0 and silhouette.size.y > 0, "%s model silhouette is visible" % name)
	var observed_rect := Rect2(Vector2(silhouette.position), Vector2(silhouette.size))
	var projected := _projected_bbox(oracle.clip, model_aabb)
	if mirror:
		projected = Rect2(Vector2(SIZE.x - (projected.position.x + projected.size.x),
			projected.position.y), projected.size)
	var fully_inside := projected.position.x >= 0.0 and projected.position.y >= 0.0 and \
		projected.end.x <= float(SIZE.x) and projected.end.y <= float(SIZE.y)
	if fully_inside:
		_check(observed_rect.position.x >= projected.position.x - 3.0 and
			observed_rect.position.y >= projected.position.y - 3.0 and
			observed_rect.end.x <= projected.end.x + 3.0 and
			observed_rect.end.y <= projected.end.y + 3.0,
			"%s silhouette is inside the projected model AABB" % name)
		_check(absf(observed_rect.size.y - projected.size.y) <=
			projected.size.y * 0.25 + 4.0,
			"%s silhouette height matches the projected AABB (%.1f vs %.1f)" % [
				name, observed_rect.size.y, projected.size.y])

	var path := ProjectSettings.globalize_path(
		"res://../tests/golden/framing/%s.png" % name)
	if update_goldens:
		DirAccess.make_dir_recursive_absolute(path.get_base_dir())
		_check(image.save_png(path) == OK, "write golden %s" % name)
	else:
		var stored := Image.load_from_file(path)
		_check(stored != null and not stored.is_empty(), "golden exists %s" % name)
		if stored != null and not stored.is_empty():
			var error := _difference(image, stored)
			_check(error < GOLDEN_ERROR, "%s matches committed golden (%.6f)" % [name, error])
	result.cases.append({
		"name": name,
		"fit": case.fit,
		"zoom": case.zoom,
		"margin": case.margin,
		"offset": [case.offset.x, case.offset.y],
		"marker_error": Vector2(observed.centre).distance_to(expected_marker),
		"marker_pixels": observed.count,
		"silhouette": [silhouette.position.x, silhouette.position.y,
			silhouette.size.x, silhouette.size.y],
		"inside": bool(runtime.call("is_avatar_inside_viewport")),
		"mirrored": mirror,
	})


func _test_preview_is_display_only() -> void:
	print("framing: preview mirroring is display-only")
	_check(_configure("model", 1.0, 0.10, Vector2.ZERO), "preview baseline configures")
	runtime.call("set_mirror_preview", false)
	_check(not preview.flip_h, "preview flip is off")
	var plain := await _capture()
	runtime.call("set_mirror_preview", true)
	_check(preview.flip_h, "preview flip is on")
	var flipped := await _capture()
	_check(_difference(plain, flipped) < 1e-9,
		"preview mirroring never changes the rendered output viewport")


func _render_cases() -> void:
	print("framing: rendered golden cases")
	for case in CASES:
		await _render_case(case)
	# The final-frame flip must move the model and the attached marker together.
	_check(_configure("model", 1.0, 0.10, Vector2.ZERO), "mirror attachment baseline")
	var plain := await _capture()
	var mirrored: Image = runtime.call("mirror_frame", plain) as Image
	_check(mirrored != null, "mirror_frame returns the mirrored frame")
	if mirrored == null:
		return
	var plain_marker := _marker_centroid(plain)
	var mirrored_marker := _marker_centroid(mirrored)
	_check(int(plain_marker.count) > 4 and int(mirrored_marker.count) > 4,
		"mirror attachment marker is visible")
	_check(absf(Vector2(mirrored_marker.centre).x -
		(float(SIZE.x) - 1.0 - Vector2(plain_marker.centre).x)) <= 1.0,
		"output mirror flips the marker with the model")
	var plain_raw := _silhouette_bbox(plain)
	var mirrored_raw := _silhouette_bbox(mirrored)
	var plain_bbox := Rect2(Vector2(plain_raw.position), Vector2(plain_raw.size))
	var mirrored_bbox := Rect2(Vector2(mirrored_raw.position), Vector2(mirrored_raw.size))
	_check(absf(mirrored_bbox.position.x - (float(SIZE.x) - plain_bbox.end.x)) <= 1.0,
		"output mirror flips the model silhouette")
	await _test_preview_is_display_only()


func _run() -> void:
	await _setup_scene()
	if runtime == null or viewport == null or model == null:
		_finish()
		return
	_test_default_and_overlay()
	_test_clamps_and_rejection()
	_test_extremes_and_head_fit()
	_test_mirroring()
	_test_offset_direction()
	if rendering:
		await _render_cases()
	_finish()


func _finish() -> void:
	result["failures"] = failures
	result["passed"] = failures.is_empty()
	result["rendered"] = rendering
	result["viewport"] = [SIZE.x, SIZE.y]
	result["model_aabb"] = [model_aabb.position, model_aabb.size]
	result["head_aabb"] = [head_aabb.position, head_aabb.size]
	result["head_pivot"] = head_pivot
	result["face_origin"] = face_origin
	result["face_scale"] = face_scale
	var path := ProjectSettings.globalize_path(
		"res://../.tools/T22-framing%s.json" % ("-render" if rendering else ""))
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		printerr("framing: cannot write ", path)
	else:
		file.store_string(JSON.stringify(result, "  "))
		file.close()
	print("T22 ", JSON.stringify(result))
	if failures.is_empty():
		print("framing: PASS")
		quit(0)
	else:
		printerr("framing: FAIL (%d failed checks)" % failures.size())
		quit(1)
