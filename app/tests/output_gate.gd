extends SceneTree

# T23 output gate and window output.
#
# Headless:
#   <godot> --headless --path app --script res://tests/output_gate.gd
# Real window:
#   <godot> --path app --rendering-method gl_compatibility --resolution 320x320 \
#       --script res://tests/output_gate.gd -- --render
#
# The headless phase exercises the native gate through the GDExtension adapter
# with NullOutput and FileOutput: hold, blank deadline, causes, diagnostics,
# PNG round-trip, alpha and colour conversion. The rendered phase repeats the
# same gate sequence on the window transport and adds the no-passthrough diff:
# a high-contrast camera texture versus a black one, with every changed pixel
# required to lie inside the region masks (one-pixel footprint tolerance).
#
# A JSON summary is written to .tools/T23-output.json (or -render.json).

const SIZE := Vector2i(320, 320)
const STAGE_SCENE := "res://tests/scenes/face_decal_flat.tscn"
const Fixtures = preload("res://tests/face_decal_flat_fixtures.gd")
const CHANGED_THRESHOLD := 2.0 / 255.0
const HOLD_MS := 150.0
const FILE_DIR := "res://../.tools/T23-file"

var failures: Array[String] = []
var result := {
	"schema": "avatar_output.result.v1",
	"rendered": false,
	"passed": false,
	"transports": {},
	"gate": {},
	"diagnostics": {},
	"no_passthrough": false,
	"changed_pixels": 0,
	"mask_outside": 0,
	"per_region": {},
	"blank_latency_ms": HOLD_MS,
	"blank_submit_us": 0.0,
	"dropped_mouth": false,
	"tracker_timeout": false,
	"forced_invalid": false,
	"window_alive": false,
	"file_frames": 0,
	"colour_space": false,
	"alpha": false,
}
var rendering := false
var viewport: SubViewport = null
var stage = null
var camera: Camera3D = null
var target: TextureRect = null
var window_manager: Object = null
var capture_space := "linear"


func _initialize() -> void:
	rendering = "--render" in OS.get_cmdline_user_args()
	_run.call_deferred()


func check(condition: bool, message: String) -> void:
	if condition:
		print("  ok   ", message)
	else:
		failures.append(message)
		printerr("  FAIL ", message)


func _run() -> void:
	await process_frame
	_test_headless_transports()
	if rendering:
		await _test_rendered()
	result["rendered"] = rendering
	result["failures"] = failures
	result["passed"] = failures.is_empty()
	_write_result()
	quit(0 if failures.is_empty() else 1)


func _new_manager(config: Dictionary) -> Object:
	var manager: Object = ClassDB.instantiate("AvatarOutputManager")
	check(manager != null, "AvatarOutputManager is registered")
	if manager == null:
		return null
	var ok: bool = manager.call("initialize", config)
	check(ok, "manager initializes: " + str(manager.call("get_last_error")))
	return manager if ok else null


func _image(value: Color = Color(0.1, 0.2, 0.3, 1.0), size: Vector2i = Vector2i(4, 4)) -> Image:
	var image := Image.create(size.x, size.y, false, Image.FORMAT_RGBA8)
	image.fill(value)
	return image


func _signal(t: int, result_valid := true, features_valid := true,
		frame_available := true) -> Dictionary:
	return {
		"result_valid": result_valid,
		"features_valid": features_valid,
		"frame_available": frame_available,
		"source_timestamp_ns": maxi(t - 16_000_000, 0),
		"render_time_ns": t,
	}


func _states(manager: Object, image: Image, times: Array, valid: Array, features: Array,
		frame_available: Array) -> Array:
	var states: Array = []
	for i in range(times.size()):
		var decision: Dictionary = manager.call("submit_frame", image,
				_signal(int(times[i]), bool(valid[i]), bool(features[i]),
						bool(frame_available[i])))
		states.append(str(decision.get("state", "")))
	return states


func _diagnostic_codes(manager: Object) -> Array:
	var codes: Array = []
	for entry in manager.call("get_diagnostics"):
		codes.append(str(entry.code))
	return codes


func _diagnostic_causes(manager: Object) -> Array:
	var causes: Array = []
	for entry in manager.call("get_diagnostics"):
		causes.append(str(entry.cause))
	return causes


func _test_headless_transports() -> void:
	var times := [1_000_000_000, 1_100_000_000, 1_149_000_000, 1_150_000_000, 1_200_000_000]
	var valid := [true, true, true, true, true]
	var features := [true, false, false, false, false]
	var frames := [true, true, true, true, true]
	var image := _image()
	var expected := ["avatar", "held", "held", "blank", "blank"]

	var null_manager := _new_manager({
		"kind": "null", "width": 4, "height": 4, "alpha": "straight",
		"color_space": "srgb", "source_alpha": "straight",
		"source_color_space": "srgb",
	})
	if null_manager != null:
		var states := _states(null_manager, image, times, valid, features, frames)
		check(states == expected, "NullOutput follows the gate: " + str(states))
		var stats: Dictionary = null_manager.call("get_stats")
		check(int(stats.avatar_frames) == 3, "NullOutput accepted three avatar frames")
		check(int(stats.blank_frames) == 2, "NullOutput accepted two blank frames")
		check(bool(null_manager.call("is_initialized")), "NullOutput stays alive while blank")
		var codes := _diagnostic_codes(null_manager)
		check(codes == ["TRK-008", "OUT-007"], "NullOutput emits TRK-008 and OUT-007 once: " + str(codes))
		var causes := _diagnostic_causes(null_manager)
		check(causes == ["missing_features", "missing_features"],
				"NullOutput records the blank cause: " + str(causes))
		result["transports"]["null"] = states == expected
		result["gate"] = null_manager.call("get_gate_state")

	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(FILE_DIR))
	var file_manager := _new_manager({
		"kind": "file", "width": 4, "height": 4, "alpha": "straight",
		"color_space": "srgb", "source_alpha": "straight",
		"source_color_space": "srgb", "directory": ProjectSettings.globalize_path(FILE_DIR),
		"prefix": "headless",
	})
	if file_manager != null:
		var states := _states(file_manager, image, times, valid, features, frames)
		check(states == expected, "FileOutput follows the gate: " + str(states))
		var first := ProjectSettings.globalize_path(FILE_DIR + "/headless-000001.png")
		var blank_path := ProjectSettings.globalize_path(FILE_DIR + "/headless-000004.png")
		check(FileAccess.file_exists(first), "FileOutput wrote the first PNG")
		check(FileAccess.file_exists(blank_path), "FileOutput wrote the blank PNG")
		var loaded := Image.load_from_file(first)
		var blank := Image.load_from_file(blank_path)
		check(loaded != null and not loaded.is_empty(), "FileOutput PNG loads")
		check(blank != null and not blank.is_empty(), "blank PNG loads")
		if loaded != null and blank != null:
			check(_pixel_is(loaded, 0, 0, Color(0.1, 0.2, 0.3, 1.0), 0.01),
					"FileOutput preserves avatar pixels")
			check(_is_fully_blank(blank), "FileOutput blank is transparent")
			result["alpha"] = _is_fully_blank(blank)
			result["file_frames"] = int(file_manager.call("get_stats").submitted)
		result["transports"]["file"] = states == expected
		file_manager.call("shutdown")

	# Opaque blank uses the configured background, not transparency.
	var opaque_manager := _new_manager({
		"kind": "file", "width": 2, "height": 2, "alpha": "opaque",
		"color_space": "srgb", "source_alpha": "straight",
		"source_color_space": "srgb", "directory": ProjectSettings.globalize_path(FILE_DIR),
		"prefix": "opaque", "background": Color(0.2, 0.4, 0.6, 1.0),
	})
	if opaque_manager != null:
		opaque_manager.call("submit_frame", _image(Color(0.1, 0.2, 0.3, 1.0), Vector2i(2, 2)),
				_signal(1_000_000_000))
		var blank: Dictionary = opaque_manager.call("submit_frame",
				_image(Color(0.1, 0.2, 0.3, 1.0), Vector2i(2, 2)),
				_signal(1_200_000_000, true, false))
		check(str(blank.state) == "blank", "opaque output blanks after the hold")
		var path := ProjectSettings.globalize_path(FILE_DIR + "/opaque-000002.png")
		var loaded := Image.load_from_file(path)
		check(loaded != null and not loaded.is_empty(), "opaque blank PNG loads")
		if loaded != null and not loaded.is_empty():
			var opaque_ok := _pixel_is(loaded, 0, 0, Color(0.2, 0.4, 0.6, 1.0), 0.01)
			check(opaque_ok, "opaque blank is the configured background")
			result["alpha"] = bool(result["alpha"]) and opaque_ok
		opaque_manager.call("shutdown")

	# Linear -> sRGB conversion at the output boundary.
	var colour_manager := _new_manager({
		"kind": "file", "width": 1, "height": 1, "alpha": "opaque",
		"color_space": "srgb", "source_alpha": "straight",
		"source_color_space": "linear", "directory": ProjectSettings.globalize_path(FILE_DIR),
		"prefix": "colour", "background": Color(0.0, 0.0, 0.0, 1.0),
	})
	if colour_manager != null:
		var linear := _image(Color(0.5, 0.0, 0.0, 0.5), Vector2i(1, 1))
		colour_manager.call("submit_frame", linear, _signal(1_000_000_000))
		var path := ProjectSettings.globalize_path(FILE_DIR + "/colour-000001.png")
		var loaded := Image.load_from_file(path)
		check(loaded != null and not loaded.is_empty(), "colour PNG loads")
		if loaded != null and not loaded.is_empty():
			var red := loaded.get_pixel(0, 0).r
			check(absf(red - 0.537) < 0.02, "linear 0.5 at alpha 0.5 encodes to sRGB 0.537: %f" % red)
			check(loaded.get_pixel(0, 0).a > 0.99, "opaque output writes alpha 1")
			result["colour_space"] = absf(red - 0.537) < 0.02
		colour_manager.call("shutdown")

	result["diagnostics"]["null"] = _diagnostic_codes(null_manager) if null_manager != null else []


func _test_rendered() -> void:
	await _setup_render()
	if stage == null:
		return
	capture_space = "linear" if RenderingServer.get_current_rendering_method() == "gl_compatibility" else "srgb"

	target = TextureRect.new()
	target.name = "OutputWindow"
	target.position = Vector2.ZERO
	target.size = Vector2(SIZE)
	target.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	target.stretch_mode = TextureRect.STRETCH_SCALE
	root.add_child(target)

	window_manager = _new_manager({
		"kind": "window", "width": SIZE.x, "height": SIZE.y, "alpha": "straight",
		"color_space": capture_space, "source_alpha": "straight",
		"source_color_space": capture_space, "target": target,
	})
	if window_manager == null:
		return

	var coverage_h := Fixtures.make_homography()
	var coverage_image := Fixtures.make_camera_image()
	var black_image := Fixtures.make_black_image()
	var pattern := await _capture(coverage_image, coverage_h)
	var black := await _capture(black_image, coverage_h)
	window_manager.call("submit_frame", pattern, _signal(1_000_000_000))
	var presented_pattern: Image = window_manager.call("get_last_frame")
	window_manager.call("submit_frame", black, _signal(1_016_000_000))
	var presented_black: Image = window_manager.call("get_last_frame")
	check(presented_pattern != null and presented_black != null, "window output presents both frames")
	if presented_pattern == null or presented_black == null:
		return
	check(target.texture != null, "window target has a texture")
	if target.texture != null:
		check(target.texture.get_width() == SIZE.x, "window texture is the output size")
	var changed := _changed_pixels(presented_pattern, presented_black)
	result["changed_pixels"] = changed.size()
	check(changed.size() > 200, "black-camera diff changes a meaningful area")
	var maps := _screen_quads()
	var outside := 0
	var outside_samples: Array = []
	var per_region := {"eye_l": 0, "eye_r": 0, "mouth": 0}
	for pixel in changed:
		var screen := Vector2(pixel.x + 0.5, pixel.y + 0.5)
		var best := _best_region_with_footprint(maps, screen)
		if float(best.mask) <= 0.0:
			outside += 1
			if outside_samples.size() < 8:
				outside_samples.append([pixel, best.mask])
		elif per_region.has(str(best.region.id)):
			per_region[str(best.region.id)] += 1
	check(outside == 0, "every changed pixel lies inside a region mask within one pixel")
	for id in per_region:
		check(int(per_region[id]) > 20, "changed pixels reach region " + str(id))
	result["no_passthrough"] = outside == 0 and changed.size() > 200
	result["mask_outside"] = outside
	result["per_region"] = per_region
	result["transports"]["window"] = outside == 0 and changed.size() > 200
	print("T23 mask diff: changed=", changed.size(), " outside=", outside,
			" per_region=", per_region, " samples=", outside_samples)
	_save("pattern", presented_pattern)
	_save("black", presented_black)

	# The same two frames through NullOutput and FileOutput. Null cannot expose
	# pixels by definition; it must accept both avatar frames and never blank.
	var null_manager := _new_manager({
		"kind": "null", "width": SIZE.x, "height": SIZE.y, "alpha": "straight",
		"color_space": capture_space, "source_alpha": "straight",
		"source_color_space": capture_space,
	})
	if null_manager != null:
		var a: Dictionary = null_manager.call("submit_frame", pattern, _signal(3_000_000_000))
		var b: Dictionary = null_manager.call("submit_frame", black, _signal(3_016_000_000))
		check(str(a.state) == "avatar" and str(b.state) == "avatar",
				"NullOutput accepts both camera patterns as avatar frames")
		result["transports"]["null_render"] = str(a.state) == "avatar" and str(b.state) == "avatar"
		null_manager.call("shutdown")

	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(FILE_DIR))
	var file_manager := _new_manager({
		"kind": "file", "width": SIZE.x, "height": SIZE.y, "alpha": "straight",
		"color_space": capture_space, "source_alpha": "straight",
		"source_color_space": capture_space,
		"directory": ProjectSettings.globalize_path(FILE_DIR), "prefix": "render",
	})
	if file_manager != null:
		file_manager.call("submit_frame", pattern, _signal(4_000_000_000))
		file_manager.call("submit_frame", black, _signal(4_016_000_000))
		var file_pattern := Image.load_from_file(
				ProjectSettings.globalize_path(FILE_DIR + "/render-000001.png"))
		var file_black := Image.load_from_file(
				ProjectSettings.globalize_path(FILE_DIR + "/render-000002.png"))
		check(file_pattern != null and file_black != null, "FileOutput wrote the render pair")
		if file_pattern != null and file_black != null:
			var file_changed := _changed_pixels(file_pattern, file_black)
			var file_outside := 0
			for pixel in file_changed:
				if float(_best_region_with_footprint(maps,
						Vector2(pixel.x + 0.5, pixel.y + 0.5)).mask) <= 0.0:
					file_outside += 1
			check(file_outside == 0, "FileOutput passes the no-passthrough mask invariant")
			result["transports"]["file_render"] = file_outside == 0 and file_changed.size() > 200
		file_manager.call("shutdown")

	# Missing mouth, tracker timeout and a forced invalid result on the window.
	var t := 10_000_000_000
	window_manager.call("submit_frame", pattern, _signal(t))
	var held_start := Time.get_ticks_usec()
	var held: Dictionary = window_manager.call("submit_frame", pattern,
			_signal(t + 100_000_000, true, false))
	var held_us := float(Time.get_ticks_usec() - held_start)
	var held_frame: Image = window_manager.call("get_last_frame")
	check(str(held.state) == "held", "dropped mouth holds the current avatar frame")
	check(held_frame != null and not _is_fully_blank(held_frame), "held frame is the avatar, not blank")
	var near: Dictionary = window_manager.call("submit_frame", pattern,
			_signal(t + 149_000_000, true, false))
	check(str(near.state) == "held", "hold lasts until the 150 ms deadline")
	var blank_start := Time.get_ticks_usec()
	var blank: Dictionary = window_manager.call("submit_frame", pattern,
			_signal(t + 150_000_000, true, false))
	var blank_us := float(Time.get_ticks_usec() - blank_start)
	var blank_frame: Image = window_manager.call("get_last_frame")
	check(str(blank.state) == "blank", "dropped mouth blanks at 150 ms")
	check(blank_frame != null and _is_fully_blank(blank_frame),
			"blank frame contains neither avatar nor camera")
	result["blank_submit_us"] = blank_us
	result["dropped_mouth"] = str(blank.state) == "blank" and _is_fully_blank(blank_frame)
	result["window_alive"] = bool(window_manager.call("is_initialized")) and target.texture != null
	check(bool(window_manager.call("is_initialized")), "window output stays alive while blank")
	check(target.texture != null, "window target keeps a blank texture")
	var codes := _diagnostic_codes(window_manager)
	check(codes == ["TRK-008", "OUT-007"], "window emits TRK-008 and OUT-007 once: " + str(codes))
	check(_diagnostic_causes(window_manager) == ["missing_features", "missing_features"],
			"window records the blank cause")

	# A tracker timeout is a different cause and code.
	window_manager.call("clear_diagnostics")
	window_manager.call("submit_frame", pattern, _signal(t + 1_000_000_000))
	var timeout: Dictionary = window_manager.call("submit_frame", pattern,
			_signal(t + 1_200_000_000, false, false))
	check(str(timeout.cause) == "face_lost", "tracker timeout reports face_lost")
	check(_diagnostic_codes(window_manager) == ["TRK-002", "OUT-007"],
			"tracker timeout emits TRK-002 and OUT-007")
	result["tracker_timeout"] = str(timeout.cause) == "face_lost"

	# A forced invalid result with no frame is OUT-007 only.
	window_manager.call("clear_diagnostics")
	var no_frame: Dictionary = window_manager.call("submit_frame", pattern,
			_signal(t + 2_000_000_000, false, false, false))
	check(str(no_frame.cause) == "no_frame", "missing frame reports no_frame")
	check(_diagnostic_codes(window_manager) == ["OUT-007"],
			"missing frame emits OUT-007 without a TRK code")
	result["forced_invalid"] = str(no_frame.state) == "blank"

	# Opaque window blank uses the configured background.
	var opaque_target := TextureRect.new()
	opaque_target.size = Vector2(8, 8)
	root.add_child(opaque_target)
	var opaque_manager := _new_manager({
		"kind": "window", "width": 8, "height": 8, "alpha": "opaque",
		"color_space": "srgb", "source_alpha": "straight",
		"source_color_space": "srgb", "target": opaque_target,
		"background": Color(0.2, 0.4, 0.6, 1.0),
	})
	if opaque_manager != null:
		opaque_manager.call("submit_frame", _image(Color(0.1, 0.2, 0.3, 1.0), Vector2i(8, 8)),
				_signal(20_000_000_000))
		var opaque_blank: Dictionary = opaque_manager.call("submit_frame",
				_image(Color(0.1, 0.2, 0.3, 1.0), Vector2i(8, 8)),
				_signal(20_200_000_000, true, false))
		var opaque_frame: Image = opaque_manager.call("get_last_frame")
		check(str(opaque_blank.state) == "blank", "opaque window blanks")
		check(opaque_frame != null and _pixel_is(opaque_frame, 0, 0,
				Color(0.2, 0.4, 0.6, 1.0), 0.01), "opaque blank is the configured background")
		check(bool(opaque_manager.call("is_initialized")), "opaque window stays alive")
		opaque_manager.call("shutdown")
		opaque_target.queue_free()

	# Colour conversion through the window adapter.
	var colour_target := TextureRect.new()
	colour_target.size = Vector2(1, 1)
	root.add_child(colour_target)
	var colour_manager := _new_manager({
		"kind": "window", "width": 1, "height": 1, "alpha": "opaque",
		"color_space": "srgb", "source_alpha": "straight",
		"source_color_space": "linear", "target": colour_target,
		"background": Color(0.0, 0.0, 0.0, 1.0),
	})
	if colour_manager != null:
		colour_manager.call("submit_frame", _image(Color(0.5, 0.0, 0.0, 0.5), Vector2i(1, 1)),
				_signal(21_000_000_000))
		var colour_frame: Image = colour_manager.call("get_last_frame")
		check(colour_frame != null and absf(colour_frame.get_pixel(0, 0).r - 0.537) < 0.02,
				"window output encodes linear to sRGB")
		colour_manager.call("shutdown")
		colour_target.queue_free()

	# The root window must actually contain the presented frame.
	await process_frame
	await RenderingServer.frame_post_draw
	var window_image := root.get_texture().get_image()
	check(window_image != null and not window_image.is_empty(), "real window is drawable")
	result["blank_latency_ms"] = HOLD_MS
	result["blank_deadline_ms"] = HOLD_MS
	result["held_submit_us"] = held_us
	window_manager.call("shutdown")
	target.queue_free()


func _setup_render() -> void:
	viewport = SubViewport.new()
	viewport.size = SIZE
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	root.add_child(viewport)
	stage = load(STAGE_SCENE).instantiate()
	viewport.add_child(stage)
	camera = stage.camera
	await process_frame
	await process_frame


func _capture(image: Image, homography: Basis) -> Image:
	stage.set_camera_image(image)
	stage.set_homography(homography)
	await process_frame
	await process_frame
	await RenderingServer.frame_post_draw
	return viewport.get_texture().get_image()


func _pixel_is(image: Image, x: int, y: int, colour: Color, tolerance: float) -> bool:
	if image == null or image.is_empty():
		return false
	var actual := image.get_pixel(clampi(x, 0, image.get_width() - 1),
			clampi(y, 0, image.get_height() - 1))
	return absf(actual.r - colour.r) <= tolerance and absf(actual.g - colour.g) <= tolerance \
			and absf(actual.b - colour.b) <= tolerance and absf(actual.a - colour.a) <= tolerance


func _is_fully_blank(image: Image) -> bool:
	if image == null or image.is_empty():
		return false
	for y in range(image.get_height()):
		for x in range(image.get_width()):
			var colour := image.get_pixel(x, y)
			if colour.a > 0.0 or colour.r > 0.0 or colour.g > 0.0 or colour.b > 0.0:
				return false
	return true


func _changed_pixels(a: Image, b: Image) -> Array[Vector2i]:
	var changed: Array[Vector2i] = []
	if a == null or b == null or a.get_size() != b.get_size():
		return changed
	for y in range(a.get_height()):
		for x in range(a.get_width()):
			var pa := a.get_pixel(x, y)
			var pb := b.get_pixel(x, y)
			var delta := maxf(absf(pa.r - pb.r), maxf(absf(pa.g - pb.g), absf(pa.b - pb.b)))
			if delta > CHANGED_THRESHOLD:
				changed.append(Vector2i(x, y))
	return changed


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
	var best := {"mask": 0.0, "region": null}
	for region in stage.regions:
		var map: Dictionary = maps[region.id]
		var mask := _mask_from_d(_region_d(map, screen), region)
		if mask > float(best.mask):
			best = {"mask": mask, "region": region}
	return best


func _best_region_with_footprint(maps: Dictionary, screen: Vector2) -> Dictionary:
	var best := _best_region(maps, screen)
	if float(best.mask) > 0.0:
		return best
	for dy in range(-1, 2):
		for dx in range(-1, 2):
			if dx == 0 and dy == 0:
				continue
			var candidate := _best_region(maps, screen + Vector2(dx, dy))
			if float(candidate.mask) > float(best.mask):
				best = candidate
	return best


func _save(name: String, image: Image) -> void:
	if image == null or image.is_empty():
		return
	var path := ProjectSettings.globalize_path("res://../.tools/T23-output-%s.png" % name)
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	image.save_png(path)


func _write_result() -> void:
	var suffix := "-render" if rendering else ""
	var path := ProjectSettings.globalize_path("res://../.tools/T23-output%s.json" % suffix)
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(JSON.stringify(result, "  "))
	print("T23 ", JSON.stringify(result))
