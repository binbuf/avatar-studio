extends SceneTree

# T25 Live HUD, panic blank and edit guard.
#
# Headless:
#   <godot> --headless --path app --script res://tests/live_hud.gd
# Real window:
#   <godot> --path app --rendering-method gl_compatibility --resolution 1280x720 \
#       --script res://tests/live_hud.gd -- --render
#
# The headless phase drives the real main scene: the Live HUD, the
# AvatarLiveSession adapter, the T23 window output and the panic gate. The
# rendered phase captures the surface at 100% and 200% UI scale and checks that
# the output monitor is actually blank during panic and coloured after restore.

const OUTPUT_SIZE := Vector2i(640, 360)
const HOLD_MS := 150

var failures: Array[String] = []
var result := {
	"schema": "avatar_live.result.v1",
	"rendered": false,
	"passed": false,
	"live": false,
	"hud": {},
	"hotkey": {},
	"panic": {},
	"edit_guard": {},
	"states": {},
	"output": {},
	"layout": {},
}
var rendering := false
var scene: Node = null
var hud: CanvasLayer = null
var runtime: Node = null
var viewport: SubViewport = null
var output_manager: Object = null


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
	var packed := load("res://scenes/main.tscn")
	check(packed != null, "main scene loads")
	if packed == null:
		_finish()
		return
	scene = packed.instantiate() as Node
	root.add_child(scene)
	await process_frame
	await process_frame
	await process_frame

	runtime = scene.get_node_or_null("AvatarRuntime")
	viewport = scene.get_node_or_null("SubViewport") as SubViewport
	hud = scene.get_node_or_null("LiveHud") as CanvasLayer
	check(runtime != null, "main scene has an AvatarRuntime")
	check(viewport != null, "main scene has a SubViewport")
	check(hud != null, "main scene has a LiveHud")
	if hud == null:
		_finish()
		return
	output_manager = hud.get("output_manager")
	check(output_manager != null, "LiveHud owns a window output manager")

	_check_surface()
	_check_hotkey_binding()
	await _check_hud_numbers()
	await _check_panic()
	await _check_states()
	await _check_edit_guard()
	_check_ui_scale()
	if rendering:
		await _check_rendered()

	result["live"] = bool(hud.call("is_live"))
	_finish()


func _check_surface() -> void:
	var session: Object = hud.get("live_session")
	check(session != null, "AvatarLiveSession is registered")
	if session != null:
		check(bool(session.call("is_live")), "Live starts on launch")
	check(hud.get_node_or_null("LiveRoot") != null, "Live root exists")
	check(hud.find_child("TopBar", true, false) != null, "top status bar exists")
	check(hud.get("banner") != null, "status banner exists")
	check(hud.get("panic_button") != null, "panic button exists")
	check(hud.get("restore_button") != null, "restore button exists")
	check(hud.get("stop_button") != null, "stop button exists")
	check(hud.get("go_live_button") != null, "go live button exists")
	check(hud.get("edit_menu") != null, "edit menu exists")
	check(hud.get("output_target") != null, "window output monitor exists")
	check(hud.get("scale_button") != null, "UI scale control exists")
	var labels: Dictionary = hud.get("labels")
	for key in ["live", "elapsed", "fps", "frame", "confidence", "face", "output", "global"]:
		check(labels.has(key), "HUD label exists: " + key)


func _check_hotkey_binding() -> void:
	check(InputMap.has_action("live_panic"), "live_panic input action exists")
	var has_ctrl := false
	var has_meta := false
	for event in InputMap.action_get_events("live_panic"):
		if event is InputEventKey and event.keycode == KEY_H:
			if event.ctrl_pressed and event.shift_pressed:
				has_ctrl = true
			if event.meta_pressed and event.shift_pressed:
				has_meta = true
	check(has_ctrl, "Ctrl+Shift+H is bound")
	check(has_meta, "Cmd+Shift+H is bound")
	var hotkey: Dictionary = hud.get("live_session").call("get_global_hotkey_status")
	check(String(hotkey.get("detail", "")) != "", "global hotkey status has a sentence")
	if not bool(hotkey.get("registered", false)):
		check(String(hotkey.get("detail", "")).contains("in-app"),
				"unavailable global binding names the in-app fallback")
	result["hotkey"] = hotkey


func _check_hud_numbers() -> void:
	hud.call("set_debug_state", "healthy")
	for _i in range(12):
		await process_frame
	var now := _now_ns()
	var hud_data: Dictionary = hud.call("get_hud_snapshot")
	var text := String(hud.get("live_session").call("get_hud_text", now))
	check(float(hud_data.get("elapsed_s", 0.0)) > 0.0, "elapsed time advances")
	check(float(hud_data.get("fps", 0.0)) > 0.0, "actual fps is reported")
	check(float(hud_data.get("frame_time_ms", 0.0)) >= 0.0, "frame time is reported")
	check(float(hud_data.get("confidence", 0.0)) > 0.0, "tracking confidence is reported")
	check(String(hud_data.get("face_state", "")) == "tracked", "face state is tracked")
	check(String(hud_data.get("output_state", "")) != "", "output state is reported")
	check(text.contains("LIVE"), "HUD text carries LIVE")
	check(text.contains("fps"), "HUD text carries fps")
	check(text.contains("frame"), "HUD text carries frame time")
	check(text.contains("tracking"), "HUD text carries tracking confidence")
	check(text.contains("face"), "HUD text carries face state")
	check(text.contains("output"), "HUD text carries output state")
	result["hud"] = {
		"fps": float(hud_data.get("fps", 0.0)),
		"elapsed_s": float(hud_data.get("elapsed_s", 0.0)),
		"frame_time_ms": float(hud_data.get("frame_time_ms", 0.0)),
		"confidence": float(hud_data.get("confidence", 0.0)),
		"face_state": String(hud_data.get("face_state", "")),
		"output_state": String(hud_data.get("output_state", "")),
		"text": text,
	}


func _check_panic() -> void:
	var session: Object = hud.get("live_session")
	hud.call("set_debug_state", "healthy")
	var image := _image(Color(0.2, 0.6, 0.9, 1.0))
	var decision: Dictionary = hud.call("submit_output_image", image, _now_ns())
	check(String(decision.get("state", "")) == "avatar", "healthy output submits the avatar")
	check(_has_colour(output_manager.call("get_last_frame")), "output monitor shows the avatar")
	var live_status: Dictionary = hud.call("get_status")
	check(String(live_status.get("output_state", "")) == "window (live)",
			"the HUD reports the live output state")

	var event := InputEventKey.new()
	event.keycode = KEY_H
	event.ctrl_pressed = true
	event.shift_pressed = true
	event.pressed = true
	hud.call("_unhandled_input", event)
	check(bool(session.call("is_panic")), "Ctrl+Shift+H blanks the output")
	check(bool(output_manager.call("is_panic")), "the output gate is in panic")
	var panic_status: Dictionary = hud.call("get_status")
	check(String(panic_status.get("output_state", "")) == "hidden (panic)",
			"the HUD reports the hidden output state")
	var panic_frame: Image = output_manager.call("get_last_frame")
	check(_is_blank_sample(panic_frame), "panic output is blank, never a static face")
	check(bool(hud.get("banner").visible), "panic banner is visible")
	var title := String(hud.get("banner_title").text)
	var action := String(hud.get("banner_action").text)
	var code := String(hud.get("banner_code").text)
	check(title.contains("Output is hidden"), "panic banner says why")
	check(action.contains("Restore"), "panic banner offers restore")
	check(code == "OUT-007", "panic banner carries a support code")

	var restore := InputEventKey.new()
	restore.keycode = KEY_H
	restore.ctrl_pressed = true
	restore.shift_pressed = true
	restore.pressed = true
	hud.call("_unhandled_input", restore)
	check(not bool(session.call("is_panic")), "the same binding restores the output")
	check(not bool(output_manager.call("is_panic")), "the gate leaves panic")
	var resumed: Dictionary = hud.call("submit_output_image", image, _now_ns() + 16_000_000)
	check(String(resumed.get("state", "")) == "avatar", "output resumes after restore")
	check(_has_colour(output_manager.call("get_last_frame")), "restored output is the avatar")
	result["panic"] = {
		"blank": _is_blank_sample(panic_frame),
		"restored": String(resumed.get("state", "")) == "avatar",
		"code": code,
		"title": title,
		"action": action,
	}


func _check_states() -> void:
	var expected := {
		"camera_lost": ["CAP-001", "Camera disconnected", "Reconnect"],
		"face_not_visible": ["TRK-001", "Can't see your face", "lighting"],
		"features_hidden": ["TRK-008", "Can't see your eyes and mouth", "sunglasses"],
		"degraded": ["TRK-004", "Face tracking is struggling", "resolution"],
		"tracker_unavailable": ["TRK-007", "Face tracking isn't running", "tracker"],
	}
	var image := _image(Color(0.2, 0.6, 0.9, 1.0))
	var offset := 0
	for name in expected.keys():
		offset += 1_000_000_000
		hud.call("set_debug_state", name)
		var status: Dictionary = hud.call("get_status")
		var want: Array = expected[name]
		check(String(status.get("code", "")) == want[0], name + " has code " + want[0])
		check(String(status.get("sentence", "")) == want[1], name + " has the human sentence")
		check(String(status.get("action", "")).contains(want[2]),
				name + " has the recovery action")
		check(bool(hud.get("banner").visible), name + " shows a banner")
		check(String(hud.get("banner_title").text) == want[1], name + " banner carries the sentence")
		check(String(hud.get("banner_code").text) == want[0], name + " banner carries the code")
		var decision: Dictionary = hud.call("submit_output_image", image, _now_ns() + offset)
		if name == "degraded":
			check(String(decision.get("state", "")) == "avatar",
					"degraded tracking keeps a valid avatar")
		else:
			check(String(decision.get("state", "")) == "blank",
					name + " blanks the output")
		result["states"][name] = {
			"code": String(status.get("code", "")),
			"sentence": String(status.get("sentence", "")),
			"action": String(status.get("action", "")),
			"output": String(decision.get("state", "")),
		}
	hud.call("set_debug_state", "healthy")


func _check_edit_guard() -> void:
	var session: Object = hud.get("live_session")
	hud.call("set_debug_state", "healthy")
	for kind in ["avatar", "calibration", "camera", "resolution", "transport"]:
		var decision: Dictionary = hud.call("request_edit", kind)
		check(not bool(decision.get("allowed", true)), kind + " is refused while live")
		check(bool(decision.get("requires_stop", false)), kind + " requires Stop")
		check(String(decision.get("action", "")) == "Stop and switch",
				kind + " offers Stop and switch")
	check(bool(hud.get("stop_and_switch_button").visible), "Stop and switch is offered")
	for kind in ["outfit", "appearance"]:
		var allowed: Dictionary = hud.call("request_edit", kind)
		check(bool(allowed.get("allowed", false)), kind + " is safe while live")

	hud.call("stop_and_switch", "avatar")
	check(not bool(session.call("is_live")), "Stop and switch stops live")
	check(not bool(output_manager.call("is_initialized")), "Stop and switch shuts the output")
	check(String(hud.get("banner_title").text).contains("Stopped"), "stopped state is explained")
	var after_stop: Dictionary = hud.call("request_edit", "avatar")
	check(bool(after_stop.get("allowed", false)), "the edit is allowed after Stop")
	hud.call("_on_go_live_pressed")
	check(bool(session.call("is_live")), "Go Live restarts the pipeline")
	result["edit_guard"] = {
		"refused": true,
		"stopped": true,
		"allowed_after_stop": bool(after_stop.get("allowed", false)),
		"restarted": bool(session.call("is_live")),
	}


func _check_ui_scale() -> void:
	var first: float = hud.call("cycle_ui_scale")
	check(absf(first - 1.5) < 0.001, "UI scale steps to 150%")
	check(absf(root.content_scale_factor - 1.5) < 0.001, "window scale is applied")
	var second: float = hud.call("cycle_ui_scale")
	check(absf(second - 2.0) < 0.001, "UI scale steps to 200%")
	check(absf(root.content_scale_factor - 2.0) < 0.001, "200% scale is applied")
	var third: float = hud.call("cycle_ui_scale")
	check(absf(third - 1.0) < 0.001, "UI scale returns to 100%")
	check(absf(root.content_scale_factor - 1.0) < 0.001, "100% scale is restored")


func _check_rendered() -> void:
	hud.call("set_debug_state", "healthy")
	var image := _image(Color(0.2, 0.6, 0.9, 1.0))
	hud.call("submit_output_image", image, _now_ns())
	for _i in range(6):
		await process_frame
	await RenderingServer.frame_post_draw
	await process_frame
	var viewport_rect := root.get_visible_rect()
	var top := hud.find_child("TopBar", true, false) as Control
	var bottom := hud.find_child("BottomBar", true, false) as Control
	check(top != null and bottom != null, "top and bottom bars are laid out")
	if top == null or bottom == null:
		return
	check(_rect_inside(top.get_global_rect(), viewport_rect), "top bar is inside the window at 100%")
	check(_rect_inside(bottom.get_global_rect(), viewport_rect),
			"bottom bar is inside the window at 100%")
	var shot := root.get_texture().get_image()
	_save("T25-live-100", shot)
	check(shot != null and not shot.is_empty(), "100% surface is drawable")
	result["layout"]["100"] = {
		"top": _rect_text(top.get_global_rect()),
		"bottom": _rect_text(bottom.get_global_rect()),
		"viewport": _rect_text(viewport_rect),
	}

	hud.call("cycle_ui_scale")
	hud.call("cycle_ui_scale")
	for _i in range(4):
		await process_frame
	await RenderingServer.frame_post_draw
	var scaled_rect := root.get_visible_rect()
	check(_rect_inside(top.get_global_rect(), scaled_rect), "top bar is inside the window at 200%")
	check(_rect_inside(bottom.get_global_rect(), scaled_rect),
			"bottom bar is inside the window at 200%")
	var scaled := root.get_texture().get_image()
	_save("T25-live-200", scaled)
	check(scaled != null and not scaled.is_empty(), "200% surface is drawable")
	result["layout"]["200"] = {
		"top": _rect_text(top.get_global_rect()),
		"bottom": _rect_text(bottom.get_global_rect()),
		"viewport": _rect_text(scaled_rect),
	}

	hud.call("set_debug_state", "features_hidden")
	for _i in range(3):
		await process_frame
	await RenderingServer.frame_post_draw
	check(_rect_inside(top.get_global_rect(), scaled_rect),
			"top bar stays inside the window with a banner at 200%")
	check(_rect_inside(bottom.get_global_rect(), scaled_rect),
			"bottom bar stays inside the window with a banner at 200%")
	check(bool(hud.get("banner").visible), "the 200% banner is visible")
	var banner_shot := root.get_texture().get_image()
	_save("T25-live-200-banner", banner_shot)
	result["layout"]["200_banner"] = {
		"top": _rect_text(top.get_global_rect()),
		"bottom": _rect_text(bottom.get_global_rect()),
		"viewport": _rect_text(scaled_rect),
	}
	hud.call("set_debug_state", "healthy")
	hud.call("cycle_ui_scale")


func _image(colour: Color) -> Image:
	var image := Image.create(320, 180, false, Image.FORMAT_RGBA8)
	image.fill(colour)
	return image


func _is_blank_sample(image: Image) -> bool:
	if image == null or image.is_empty():
		return false
	var points := [
		Vector2i(0, 0),
		Vector2i(image.get_width() / 2, image.get_height() / 2),
		Vector2i(image.get_width() - 1, image.get_height() - 1),
	]
	for point in points:
		var colour := image.get_pixel(point.x, point.y)
		if colour.a > 0.001 or colour.r > 0.001 or colour.g > 0.001 or colour.b > 0.001:
			return false
	return true


func _has_colour(image: Image) -> bool:
	if image == null or image.is_empty():
		return false
	var colour := image.get_pixel(image.get_width() / 2, image.get_height() / 2)
	return colour.a > 0.01 and (colour.r > 0.01 or colour.g > 0.01 or colour.b > 0.01)


func _rect_inside(inner: Rect2, outer: Rect2) -> bool:
	return outer.encloses(inner)


func _rect_text(rect: Rect2) -> String:
	return "%d,%d %dx%d" % [int(rect.position.x), int(rect.position.y),
			int(rect.size.x), int(rect.size.y)]


func _save(name: String, image: Image) -> void:
	if image == null or image.is_empty():
		return
	var path := ProjectSettings.globalize_path("res://../.tools/%s.png" % name)
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	image.save_png(path)


func _now_ns() -> int:
	return int(Time.get_ticks_usec()) * 1000


func _finish() -> void:
	result["rendered"] = rendering
	result["failures"] = failures
	result["passed"] = failures.is_empty()
	var suffix := "-render" if rendering else ""
	var path := ProjectSettings.globalize_path("res://../.tools/T25-live%s.json" % suffix)
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(JSON.stringify(result, "  "))
	file.close()
	print("T25 ", JSON.stringify(result))
	quit(0 if failures.is_empty() else 1)
