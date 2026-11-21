extends SceneTree

# Headless runtime configuration and pacing test for T16.
#
# Run with:
#   <godot> --headless --path app --script res://tests/runtime.gd
#
# It asserts the one-viewport configuration, the preview copy, the engine
# knobs, the hidden C++ HUD, consumer/paused processing, and a 10-second paced
# run. The pacing gate follows docs/design/testing-and-quality.md: frame-time
# p99 and deadline-jitter p99 must stay under 16.6 ms, and no catch-up burst
# may exceed two frames. The nominal interval is 16.667 ms, so the absolute
# interval p99 is reported but not gated against 16.6 ms.
#
# A JSON summary is written to AVATAR_RUNTIME_RESULT when set, otherwise to
# user://runtime-result.json. tests/gates/T16.ps1 reads the file written by
# scripts/smoke.ps1.

const PACED_SECONDS := 10.0
const MAX_P99_MS := 16.6
const MAX_BURST_FRAMES := 2

var failures: Array[String] = []
var result: Dictionary = {}


func _initialize() -> void:
	_run.call_deferred()


func _check(condition: bool, message: String) -> void:
	if condition:
		print("  ok   ", message)
	else:
		failures.append(message)
		printerr("  FAIL ", message)


func _call(runtime: Object, method: String, arg: Variant = null) -> Variant:
	if arg == null:
		return runtime.call(method)
	return runtime.call(method, arg)


func _run() -> void:
	print("runtime: Avatar Studio one-viewport pacing test")

	var packed := load("res://scenes/main.tscn")
	_check(packed != null, "main scene loads")
	if packed == null:
		_finish(0.0)
		return

	var scene := packed.instantiate() as Node
	_check(scene != null, "main scene instantiates")
	if scene == null:
		_finish(0.0)
		return
	root.add_child(scene)
	await process_frame
	await process_frame

	var runtime := scene.get_node_or_null("AvatarRuntime")
	var viewport := scene.get_node_or_null("SubViewport") as SubViewport
	var preview := scene.get_node_or_null("Preview") as TextureRect
	var presentation: Node3D = null
	if viewport != null:
		presentation = viewport.get_node_or_null("Presentation") as Node3D

	_check(runtime != null, "main scene has an AvatarRuntime")
	_check(viewport != null, "main scene has a SubViewport")
	_check(preview != null, "main scene has a Preview TextureRect")
	_check(presentation != null, "SubViewport has a fixed Presentation node")
	if runtime == null or viewport == null or preview == null:
		_finish(0.0)
		return

	_check(int(Engine.max_fps) == 60, "Engine.max_fps is pinned to 60")
	_check(OS.low_processor_usage_mode == false, "OS low-processor mode is disabled")
	_check(_call(runtime, "is_low_processor_mode_disabled") == true,
		"runtime reports low-processor mode disabled")
	_check(String(_call(runtime, "get_renderer_name")) == "forward_plus",
		"renderer is Forward+ (%s)" % String(_call(runtime, "get_renderer_name")))

	_check(viewport.size == Vector2i(1920, 1080), "SubViewport is 1920x1080")
	_check(viewport.transparent_bg, "SubViewport has a transparent background")
	_check(viewport.render_target_update_mode == SubViewport.UPDATE_ALWAYS,
		"SubViewport render_target_update_mode is ALWAYS")
	_check(_call(runtime, "get_avatar_viewport_size") == Vector2i(1920, 1080),
		"runtime reports the 1920x1080 viewport")
	_check(_call(runtime, "is_avatar_viewport_transparent") == true,
		"runtime reports a transparent viewport")
	_check(int(_call(runtime, "get_avatar_viewport_update_mode")) == SubViewport.UPDATE_ALWAYS,
		"runtime reports UPDATE_ALWAYS")

	var subviewports := scene.find_children("*", "SubViewport", true, false)
	_check(subviewports.size() == 1, "the scene owns exactly one SubViewport")

	_check(preview.texture != null, "preview has a texture")
	_check(preview.texture is ViewportTexture, "preview texture is a ViewportTexture")
	if preview.texture != null and viewport.get_texture() != null:
		_check(preview.texture.get_rid() == viewport.get_texture().get_rid(),
			"preview samples the avatar viewport texture")
	_check(_call(runtime, "is_preview_a_copy") == true, "runtime reports the preview is a copy")

	var transform: Transform3D = _call(runtime, "get_presentation_transform")
	_check(transform.basis.x.is_equal_approx(Vector3(-1, 0, 0)),
		"presentation transform is a 180 degree yaw (x)")
	_check(transform.basis.z.is_equal_approx(Vector3(0, 0, -1)),
		"presentation transform is a 180 degree yaw (z)")

	_check(_call(runtime, "is_debug_hud_visible") == false, "debug HUD is hidden by default")
	var hud := runtime.get_node_or_null("DebugHudLayer/DebugHud") as Label
	_check(hud != null, "debug HUD is created by C++")
	if hud != null:
		_check(hud.visible == false, "debug HUD label starts hidden")
		_call(runtime, "set_debug_hud_visible", true)
		await process_frame
		_check(_call(runtime, "is_debug_hud_visible") == true, "debug HUD toggles visible")
		var hud_text := String(_call(runtime, "get_debug_hud_text"))
		_check(hud_text.contains("frame") and hud_text.contains("interval") and
			hud_text.contains("drops"), "debug HUD carries frame/interval/drop counters")
		_check(hud_text.contains("1920x1080"), "debug HUD carries the viewport size")
		_check(hud_text.contains(String(_call(runtime, "get_renderer_name"))),
			"debug HUD carries the renderer")
		_check(hud_text.contains(String(_call(runtime, "get_pacing_mode"))),
			"debug HUD carries the pacing mode")
		_call(runtime, "set_debug_hud_visible", false)
		await process_frame
		_check(_call(runtime, "is_debug_hud_visible") == false, "debug HUD hides again")

	var frames_before_pause := int(_call(runtime, "get_frame_count"))
	_call(runtime, "set_consumer_connected", true)
	_check(_call(runtime, "is_consumer_connected") == true, "consumer connection is recorded")
	_check(_call(runtime, "is_app_nap_disabled") == true, "App Nap is disabled while live")
	paused = true
	for _i in range(5):
		await process_frame
	paused = false
	_check(int(_call(runtime, "get_frame_count")) > frames_before_pause,
		"runtime keeps processing while the tree is paused (hidden-window proxy)")
	_call(runtime, "set_consumer_connected", false)
	_check(_call(runtime, "is_app_nap_disabled") == false, "App Nap resumes without a consumer")

	var pacing_mode := String(_call(runtime, "get_pacing_mode"))
	_check(pacing_mode == "vsync" or pacing_mode == "deadline",
		"pacing mode is explicit (%s)" % pacing_mode)
	_check(absf(float(_call(runtime, "get_target_interval_ms")) - 16.666667) < 0.001,
		"target interval is 16.667 ms")

	_call(runtime, "reset_pacing_stats")
	var start_usec := Time.get_ticks_usec()
	while Time.get_ticks_usec() - start_usec < int(PACED_SECONDS * 1_000_000.0):
		await process_frame
	var duration_s := float(Time.get_ticks_usec() - start_usec) / 1_000_000.0

	var samples := int(_call(runtime, "get_pacing_sample_count"))
	var frame_time_p99 := float(_call(runtime, "get_frame_time_p99_ms"))
	var interval_p99 := float(_call(runtime, "get_interval_p99_ms"))
	var jitter_p99 := float(_call(runtime, "get_jitter_p99_ms"))
	var max_interval := float(_call(runtime, "get_max_interval_ms"))
	var max_burst := int(_call(runtime, "get_max_burst_frames"))
	var dropped := int(_call(runtime, "get_dropped_frames"))
	var target_interval := float(_call(runtime, "get_target_interval_ms"))

	print(("runtime: paced %.2f s, %d samples, frame_time_p99=%.3f ms, interval_p99=%.3f ms, " +
		"interval_jitter_p99=%.3f ms, max_interval=%.3f ms, max_burst=%d, dropped=%d") % [
			duration_s, samples, frame_time_p99, interval_p99, jitter_p99,
			max_interval, max_burst, dropped])

	_check(samples >= 500, "10-second run collected at least 500 interval samples (%d)" % samples)
	_check(frame_time_p99 <= MAX_P99_MS,
		"frame-time p99 <= 16.6 ms (%.3f ms)" % frame_time_p99)
	_check(jitter_p99 <= MAX_P99_MS,
		"interval-jitter p99 <= 16.6 ms (%.3f ms)" % jitter_p99)
	_check(interval_p99 <= target_interval * 1.1,
		"interval p99 is within 10%% of the target (%.3f ms)" % interval_p99)
	_check(max_interval <= target_interval * 3.0,
		"no interval longer than two missed frames (%.3f ms)" % max_interval)
	_check(max_burst <= MAX_BURST_FRAMES,
		"no catch-up burst longer than two frames (%d)" % max_burst)

	var viewport_render_count := int(_call(runtime, "get_viewport_render_count"))
	var preview_render_count := int(_call(runtime, "get_preview_render_count"))
	_check(viewport_render_count > 0, "avatar viewport rendered frames (%d)" % viewport_render_count)
	_check(preview_render_count == 0,
		"preview performed no scene render of its own (%d)" % preview_render_count)

	result = {
		"schema": "avatar_runtime.result.v1",
		"engine": {
			"max_fps": int(Engine.max_fps),
			"low_processor_mode": OS.low_processor_usage_mode,
			"renderer": String(_call(runtime, "get_renderer_name")),
			"display": DisplayServer.get_name(),
			"refresh_hz": float(_call(runtime, "get_display_refresh_hz")),
		},
		"viewport": {
			"size": [viewport.size.x, viewport.size.y],
			"transparent": viewport.transparent_bg,
			"update_mode": int(viewport.render_target_update_mode),
			"count": subviewports.size(),
		},
		"preview": {
			"is_copy": bool(_call(runtime, "is_preview_a_copy")),
			"viewport_render_count": viewport_render_count,
			"preview_render_count": preview_render_count,
		},
		"presentation": {
			"yaw_180": transform.basis.x.is_equal_approx(Vector3(-1, 0, 0)) and
				transform.basis.z.is_equal_approx(Vector3(0, 0, -1)),
		},
		"hud": {
			"hidden_by_default": true,
			"toggles": true,
		},
		"consumer": {
			"paused_processing": true,
			"app_nap_disabled": bool(_call(runtime, "is_app_nap_disabled")),
		},
		"pacing": {
			"mode": pacing_mode,
			"target_interval_ms": target_interval,
			"duration_s": duration_s,
			"samples": samples,
			"frame_time_p99_ms": frame_time_p99,
			"interval_p99_ms": interval_p99,
			"interval_jitter_p99_ms": jitter_p99,
			"max_interval_ms": max_interval,
			"max_burst_frames": max_burst,
			"dropped": dropped,
		},
		"passed": failures.is_empty(),
	}

	_finish(duration_s)


func _finish(_duration_s: float) -> void:
	result["passed"] = failures.is_empty()
	var path := OS.get_environment("AVATAR_RUNTIME_RESULT")
	if path.is_empty():
		path = "user://runtime-result.json"
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		printerr("runtime: cannot write result to ", path)
	else:
		file.store_string(JSON.stringify(result, "  "))
		file.close()

	if failures.is_empty():
		print("runtime: PASS")
		quit(0)
	else:
		printerr("runtime: FAIL (%d failed checks)" % failures.size())
		quit(1)
