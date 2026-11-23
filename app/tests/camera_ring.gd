extends SceneTree

## T24 timestamp-paired camera ring.
##
## Headless: the GDExtension ring adapter publishes NV12/BGRA frames, selects an
## older slot by exact timestamp, wraps, drops for a pinned slow consumer and
## invalidates on a format change.
##
## Windowed (`-- --render`): the shared YUV shader is checked against known
## colour bars for BT.601/709 and limited/full range; the flat decal binds the
## selected (older) ring slot and renders it, proving pixels and pose are paired;
## ring upload and GPU downscale costs are measured into the JSON summary.
##
##   <godot> --headless --path app --script res://tests/camera_ring.gd
##   <godot> --path app --rendering-method gl_compatibility --resolution 320x320 \
##       --script res://tests/camera_ring.gd -- --render
##
## JSON: .tools/T24-camera-ring.json (or -render.json).

const STAGE_SCENE := "res://tests/scenes/face_decal_flat.tscn"
const PROBE_SHADER := "res://tests/shaders/yuv_probe.gdshader"
const BARS := [
	Color(0.75, 0.75, 0.75), Color(0.75, 0.75, 0.0), Color(0.0, 0.75, 0.75),
	Color(0.0, 0.75, 0.0), Color(0.75, 0.0, 0.75), Color(0.75, 0.0, 0.0),
	Color(0.0, 0.0, 0.75), Color(0.0, 0.0, 0.0),
]
const ENCODINGS := ["bt601_limited", "bt601_full", "bt709_limited", "bt709_full"]
const TOLERANCE := 0.02
const VIEWPORT_SIZE := Vector2i(320, 320)

var failures: Array[String] = []
var result := {
	"schema": "avatar_camera_ring.result.v1",
	"rendered": false,
	"passed": false,
	"ring": {},
	"pairing": {},
	"yuv_bars": {},
	"decal": {},
	"measurements": {},
}
var rendering := false
var bar_viewport: SubViewport = null
var bar_material: ShaderMaterial = null
var decal_viewport: SubViewport = null
var stage = null


func _initialize() -> void:
	rendering = "--render" in OS.get_cmdline_user_args()
	_run.call_deferred()


func check(ok: bool, message: String) -> void:
	if ok:
		print("  ok   ", message)
	else:
		failures.append(message)
		printerr("  FAIL ", message)


func _run() -> void:
	await process_frame
	_test_headless_ring()
	if rendering:
		await _test_yuv_bars()
		await _test_decal_slot()
		await _test_measurements()
	result["rendered"] = rendering
	result["failures"] = failures
	result["passed"] = failures.is_empty()
	_write_result()
	quit(0 if failures.is_empty() else 1)


func _write_result() -> void:
	var path := "res://../.tools/T24-camera-ring%s.json" % ("-render" if rendering else "")
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		printerr("cannot write ", path)
		return
	file.store_string(JSON.stringify(result, "  "))
	file.close()


func _new_ring(capacity := 4) -> Object:
	var ring: Object = ClassDB.instantiate("AvatarCameraRing")
	check(ring != null, "AvatarCameraRing is registered")
	if ring == null:
		return null
	check(bool(ring.call("initialize", capacity)), "ring initializes: " + str(ring.call("get_last_error")))
	return ring


func _nv12_solid(colour: Color, width: int, height: int, color_space := "bt601_limited") -> Array:
	var encoding := _encoding(color_space)
	var yuv := _yuv_from_rgb(colour, int(encoding.matrix), int(encoding.range))
	var y_plane := PackedByteArray()
	y_plane.resize(width * height)
	y_plane.fill(clampi(roundi(yuv.x * 255.0), 0, 255))
	var uv_plane := PackedByteArray()
	uv_plane.resize(width * height / 2)
	var u := clampi(roundi(yuv.y * 255.0), 0, 255)
	var v := clampi(roundi(yuv.z * 255.0), 0, 255)
	for i in range(0, uv_plane.size(), 2):
		uv_plane[i] = u
		uv_plane[i + 1] = v
	return [y_plane, uv_plane]


func _bgra_solid(colour: Color, width: int, height: int) -> PackedByteArray:
	var data := PackedByteArray()
	data.resize(width * height * 4)
	var b := clampi(roundi(colour.b * 255.0), 0, 255)
	var g := clampi(roundi(colour.g * 255.0), 0, 255)
	var r := clampi(roundi(colour.r * 255.0), 0, 255)
	for i in range(0, data.size(), 4):
		data[i] = b
		data[i + 1] = g
		data[i + 2] = r
		data[i + 3] = 255
	return data


func _encoding(color_space: String) -> Dictionary:
	match color_space:
		"bt601_limited":
			return {"matrix": 0, "range": 0}
		"bt601_full":
			return {"matrix": 0, "range": 1}
		"bt709_limited":
			return {"matrix": 1, "range": 0}
		"bt709_full":
			return {"matrix": 1, "range": 1}
	return {"matrix": 0, "range": 0}


func _yuv_from_rgb(colour: Color, matrix: int, range: int) -> Vector3:
	var r := colour.r
	var g := colour.g
	var b := colour.b
	var y: float
	var u: float
	var v: float
	if matrix == 1:
		y = 0.2126 * r + 0.7152 * g + 0.0722 * b
		u = -0.114572 * r - 0.385428 * g + 0.5 * b + 0.5
		v = 0.5 * r - 0.454153 * g - 0.045847 * b + 0.5
	else:
		y = 0.299 * r + 0.587 * g + 0.114 * b
		u = -0.168736 * r - 0.331264 * g + 0.5 * b + 0.5
		v = 0.5 * r - 0.418688 * g - 0.081312 * b + 0.5
	if range == 0:
		y = 16.0 / 255.0 + y * (219.0 / 255.0)
		u = 16.0 / 255.0 + u * (224.0 / 255.0)
		v = 16.0 / 255.0 + v * (224.0 / 255.0)
	return Vector3(clampf(y, 0.0, 1.0), clampf(u, 0.0, 1.0), clampf(v, 0.0, 1.0))


func _test_headless_ring() -> void:
	var ring := _new_ring(4)
	if ring == null:
		result["ring"] = {"passed": false}
		return
	var times := [1000, 1016, 1033, 1050, 1066, 1083]
	for i in range(times.size()):
		var planes := _nv12_solid(Color(0.2, 0.4, 0.6), 64, 48)
		check(bool(ring.call("publish_nv12", planes[0], planes[1], 64, 48, times[i], i + 1,
				"bt601_limited")), "publish NV12 frame %d" % (i + 1))
	var stats: Dictionary = ring.call("get_stats")
	check(int(stats.published) == 6, "six frames published")
	check(int(stats.wraps) >= 2, "ring wrapped")
	check(int(ring.call("get_slot_info", 0).get("valid", false)) or
			int(ring.call("get_slot_info", 3).get("valid", false)), "ring has valid slots")

	var older: Dictionary = ring.call("select", 1033, 2000)
	check(bool(older.get("valid", false)), "selects an older exact timestamp")
	check(int(older.get("sequence", 0)) == 3, "older selection is sequence 3")
	check(bool(older.get("is_yuv", false)), "older selection is YUV")
	check(int(older.get("matrix", -1)) == 0 and int(older.get("range", -1)) == 0,
			"older selection carries BT.601 limited")
	check(older.get("y_texture") != null and older.get("uv_texture") != null,
			"older selection exposes plane textures")
	var miss: Dictionary = ring.call("select", 1025, 2000)
	check(not bool(miss.get("valid", true)), "select never falls back to different pixels")
	ring.call("release_selected")

	var tracking: Dictionary = ring.call("select_for_tracking", 1070)
	check(bool(tracking.get("valid", false)), "tracking selects the newest frame at or before now")
	check(int(tracking.get("sequence", 0)) == 5, "tracking selection is sequence 5")
	ring.call("release_selected")

	# Pin all four live slots; the next publish is dropped, not queued.
	var pinned := 0
	for i in range(times.size()):
		var slot: Dictionary = ring.call("select", times[i], 2000)
		if bool(slot.get("valid", false)):
			pinned += 1
	check(pinned == 4, "four slots pinned for the slow-consumer case")
	var before: Dictionary = ring.call("get_stats")
	var extra := _nv12_solid(Color(0.2, 0.4, 0.6), 64, 48)
	check(not bool(ring.call("publish_nv12", extra[0], extra[1], 64, 48, 1100, 7,
			"bt601_limited")), "publish drops while every slot is pinned")
	check(int(ring.call("get_stats").dropped) == int(before.dropped) + 1, "drop is counted")
	ring.call("release_selected")

	var generation := int(ring.call("get_generation"))
	var bgra := _bgra_solid(Color(0.9, 0.1, 0.1), 32, 32)
	check(bool(ring.call("publish_bgra", bgra, 32, 32, 1200, 8)), "publish BGRA after NV12")
	check(int(ring.call("get_generation")) > generation, "format change bumps the generation")
	check(not bool(ring.call("select", 1033, 2000).get("valid", true)),
			"old generation timestamp is invalid after a format change")
	var fresh: Dictionary = ring.call("select", 1200, 2000)
	check(bool(fresh.get("valid", false)), "new format selects")
	check(bool(fresh.get("bgra", false)), "BGRA slot is flagged for the shader swizzle")
	result["ring"] = {
		"passed": failures.is_empty(),
		"published": int(stats.published),
		"wraps": int(stats.wraps),
		"format_changes": int(ring.call("get_stats").format_changes),
		"dropped": int(ring.call("get_stats").dropped),
	}
	ring.call("release_selected")


func _setup_bar_viewport() -> void:
	bar_viewport = SubViewport.new()
	# Two pixels per bar so every bar owns a 4:2:0 chroma sample.
	bar_viewport.size = Vector2i(BARS.size() * 2, 1)
	bar_viewport.disable_3d = true
	bar_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	root.add_child(bar_viewport)
	var rect := ColorRect.new()
	bar_material = ShaderMaterial.new()
	bar_material.shader = load(PROBE_SHADER)
	rect.material = bar_material
	bar_viewport.add_child(rect)
	rect.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)


func _test_yuv_bars() -> void:
	_setup_bar_viewport()
	await process_frame
	await process_frame
	var all_passed := true
	for color_space in ENCODINGS:
		var encoding := _encoding(color_space)
		var y_data := PackedByteArray()
		var uv_data := PackedByteArray()
		y_data.resize(BARS.size() * 2)
		uv_data.resize(BARS.size() * 2)
		for i in range(BARS.size()):
			var yuv := _yuv_from_rgb(BARS[i], int(encoding.matrix), int(encoding.range))
			y_data[i * 2] = clampi(roundi(yuv.x * 255.0), 0, 255)
			y_data[i * 2 + 1] = y_data[i * 2]
			uv_data[i * 2] = clampi(roundi(yuv.y * 255.0), 0, 255)
			uv_data[i * 2 + 1] = clampi(roundi(yuv.z * 255.0), 0, 255)
		var y_image := Image.create_from_data(BARS.size() * 2, 1, false, Image.FORMAT_R8, y_data)
		var uv_image := Image.create_from_data(BARS.size(), 1, false,
				Image.FORMAT_RG8, uv_data)
		var y_texture := ImageTexture.create_from_image(y_image)
		var uv_texture := ImageTexture.create_from_image(uv_image)
		bar_material.set_shader_parameter("y_tex", y_texture)
		bar_material.set_shader_parameter("uv_tex", uv_texture)
		bar_material.set_shader_parameter("matrix", int(encoding.matrix))
		bar_material.set_shader_parameter("range", int(encoding.range))
		await process_frame
		await RenderingServer.frame_post_draw
		var image := bar_viewport.get_texture().get_image()
		if image == null or image.is_empty():
			check(false, "colour-bar readback for " + color_space)
			all_passed = false
			continue
		var max_error := 0.0
		var errors: Array = []
		for i in range(BARS.size()):
			var actual := image.get_pixel(i * 2, 0)
			var rgb := Vector3(actual.r, actual.g, actual.b)
			var expected: Color = BARS[i]
			var error := maxf(absf(rgb.x - expected.r),
					maxf(absf(rgb.y - expected.g), absf(rgb.z - expected.b)))
			max_error = maxf(max_error, error)
			errors.append(error)
		var passed := max_error <= TOLERANCE
		check(passed, "colour bars %s (max error %.4f)" % [color_space, max_error])
		all_passed = all_passed and passed
		result["yuv_bars"][color_space] = {
			"passed": passed,
			"max_error": max_error,
			"errors": errors,
		}
	result["yuv_bars"]["passed"] = all_passed


func _test_decal_slot() -> void:
	decal_viewport = SubViewport.new()
	decal_viewport.size = VIEWPORT_SIZE
	decal_viewport.own_world_3d = true
	decal_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	root.add_child(decal_viewport)
	stage = load(STAGE_SCENE).instantiate()
	decal_viewport.add_child(stage)
	await process_frame
	await process_frame

	var ring := _new_ring(4)
	if ring == null:
		result["decal"] = {"passed": false}
		return
	var red := _nv12_solid(Color(1.0, 0.0, 0.0), 128, 128)
	var blue := _nv12_solid(Color(0.0, 0.0, 1.0), 128, 128)
	check(bool(ring.call("publish_nv12", red[0], red[1], 128, 128, 2000, 1, "bt601_limited")),
			"publish red camera frame")
	check(bool(ring.call("publish_nv12", blue[0], blue[1], 128, 128, 2016, 2, "bt601_limited")),
			"publish blue camera frame")

	var older: Dictionary = ring.call("select", 2000, 2100)
	check(bool(older.get("valid", false)) and int(older.get("sequence", 0)) == 1,
			"pairing selects the older red slot, not the newest blue slot")
	check(stage.set_camera_slot(older), "flat decal binds the selected NV12 slot")
	await process_frame
	await process_frame
	await RenderingServer.frame_post_draw
	var red_image := decal_viewport.get_texture().get_image()
	var red_counts := _colour_counts(red_image)
	check(red_counts.red > 100, "decal renders the paired red slot (%d px)" % red_counts.red)
	check(red_counts.blue < 5, "decal does not sample the newer blue slot (%d px)" % red_counts.blue)
	ring.call("release_selected")

	var newest: Dictionary = ring.call("select", 2016, 2100)
	check(bool(newest.get("valid", false)) and int(newest.get("sequence", 0)) == 2,
			"newest slot selects by its own timestamp")
	check(stage.set_camera_slot(newest), "flat decal binds the newest NV12 slot")
	await process_frame
	await process_frame
	await RenderingServer.frame_post_draw
	var blue_image := decal_viewport.get_texture().get_image()
	var blue_counts := _colour_counts(blue_image)
	check(blue_counts.blue > 100, "decal renders the blue slot when selected (%d px)" % blue_counts.blue)
	check(blue_counts.red < 5, "decal no longer samples red (%d px)" % blue_counts.red)
	ring.call("release_selected")
	result["decal"] = {
		"passed": red_counts.red > 100 and red_counts.blue < 5 and
				blue_counts.blue > 100 and blue_counts.red < 5,
		"red_pixels": red_counts.red,
		"red_slot_blue_pixels": red_counts.blue,
		"blue_pixels": blue_counts.blue,
		"blue_slot_red_pixels": blue_counts.red,
	}


func _colour_counts(image: Image) -> Dictionary:
	var red := 0
	var blue := 0
	if image == null or image.is_empty():
		return {"red": red, "blue": blue}
	for y in range(image.get_height()):
		for x in range(image.get_width()):
			var c := image.get_pixel(x, y)
			if c.r > 0.25 and c.g < 0.12 and c.b < 0.12:
				red += 1
			elif c.b > 0.25 and c.r < 0.12 and c.g < 0.12:
				blue += 1
	return {"red": red, "blue": blue}


func _test_measurements() -> void:
	var ring := _new_ring(4)
	if ring == null:
		return
	var frame := _nv12_solid(Color(0.4, 0.5, 0.6), 1280, 720)
	for i in range(40):
		ring.call("publish_nv12", frame[0], frame[1], 1280, 720, 3000 + i * 16, i + 1,
				"bt709_limited")
	var stats: Dictionary = ring.call("get_stats")
	var measurements := {
		"upload_p50_us": float(stats.upload_p50_ns) / 1000.0,
		"upload_p95_us": float(stats.upload_p95_ns) / 1000.0,
		"upload_max_us": float(stats.upload_max_ns) / 1000.0,
		"upload_samples": int(stats.upload_samples),
	}
	var selected: Dictionary = ring.call("select_for_tracking", 4000)
	check(bool(selected.get("valid", false)), "measurement selects a tracking frame")
	if bool(selected.get("valid", false)):
		var downscale := await _measure_downscale(selected.get("y_texture"))
		measurements.merge(downscale, true)
	ring.call("release_selected")
	result["measurements"] = measurements
	check(float(measurements.upload_p50_us) > 0.0, "ring upload cost is measured")
	check(float(measurements.get("downscale_p50_us", 0.0)) > 0.0,
			"GPU downscale cost is measured")


func _measure_downscale(y_texture: Texture2D) -> Dictionary:
	var down := SubViewport.new()
	down.size = Vector2i(320, 180)
	down.disable_3d = true
	down.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	root.add_child(down)
	var rect := TextureRect.new()
	rect.texture = y_texture
	rect.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	rect.stretch_mode = TextureRect.STRETCH_SCALE
	down.add_child(rect)
	rect.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	await process_frame
	await process_frame
	# Warm up shader compilation and the first texture allocation; the measured
	# samples are the steady-state blit + readback.
	for i in range(5):
		RenderingServer.force_draw(false)
		RenderingServer.force_sync()
		down.get_texture().get_image()
	var samples: Array = []
	for i in range(20):
		var start := Time.get_ticks_usec()
		RenderingServer.force_draw(false)
		RenderingServer.force_sync()
		var image := down.get_texture().get_image()
		var elapsed := Time.get_ticks_usec() - start
		if image != null and not image.is_empty():
			samples.append(elapsed)
	samples.sort()
	var p50 := 0.0
	var p95 := 0.0
	if not samples.is_empty():
		p50 = float(samples[samples.size() / 2])
		p95 = float(samples[mini(samples.size() - 1, int(ceil(samples.size() * 0.95)) - 1)])
	return {
		"downscale_p50_us": p50,
		"downscale_p95_us": p95,
		"downscale_samples": samples.size(),
		"downscale_size": "320x180",
	}


