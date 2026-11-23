extends SceneTree

# T25c Setup window: three panes, overlay data, live mapping, apply/persist,
# background toggle, capture/tracker selectors and the feature-loss hold.
#
# Headless:
#   <godot> --headless --path app --script res://tests/setup_window.gd \
#       -- --live-source synthetic --live-tracker scripted
# Rendered:
#   <godot> --path app --resolution 1280x720 --script res://tests/setup_window.gd \
#       --quit-after 60000 -- --render --live-source synthetic --live-tracker scripted
#
# The synthetic camera + scripted tracker drive the real main scene, the native
# live pipeline, the decal and the output gate. The camera/tracker measurement
# with a person is the task's HITL review, not this test.

const Calibration = preload("res://scenes/live_calibration.gd")

var failures: Array[String] = []
var result := {
	"schema": "avatar_setup.result.v1",
	"rendered": false,
	"passed": false,
	"panes": {},
	"overlay": {},
	"mapping": {},
	"calibration": {},
	"persist": {},
	"hold": {},
	"background": {},
	"selectors": {},
	"layout": {},
}
var rendering := false
var scene: Node = null
var hud: CanvasLayer = null
var setup = null
var controller = null
var test_path := "user://T25c-setup-test.json"


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
	for _i in range(30):
		await process_frame

	hud = scene.get_node_or_null("LiveHud") as CanvasLayer
	check(hud != null, "main scene has a LiveHud")
	if hud == null:
		_finish()
		return
	setup = hud.get("setup_window")
	controller = hud.get("pipeline_controller")
	check(setup != null, "LiveHud owns the Setup window")
	check(controller != null, "LiveHud owns the live pipeline controller")
	if setup == null or controller == null:
		_finish()
		return

	# A deterministic calibration file for this test; never touch the operator's.
	controller.set_calibration_path(test_path)
	_remove(test_path)
	var reloaded: Dictionary = controller.reload_calibration()
	check(bool(reloaded.get("ok", false)), "the controller reloads the provisional mapping")

	hud.call("open_setup")
	for _i in range(24):
		await process_frame

	_check_panes()
	_check_overlay()
	_check_mapping()
	await _check_final_output()
	await _check_edit_and_apply()
	_check_hold()
	if rendering:
		await _check_rendered()
	await _check_selectors()

	_remove(test_path)
	result["live"] = bool(hud.call("is_live"))
	_finish()


func _check_panes() -> void:
	check(bool(setup.call("is_open")), "the Setup window is open")
	check(bool(setup.visible), "the Setup window is visible")
	var panes: Dictionary = setup.call("get_panes")
	for key in ["camera", "mapping", "final", "status", "controls"]:
		check(panes.get(key) != null, "Setup pane exists: " + key)
	var camera: Control = panes.get("camera")
	var mapping: Control = panes.get("mapping")
	var final: Control = panes.get("final")
	if camera != null and mapping != null and final != null:
		check(not camera.get_global_rect().intersects(mapping.get_global_rect()),
				"camera and mapping panes do not overlap")
		check(not mapping.get_global_rect().intersects(final.get_global_rect()),
				"mapping and final panes do not overlap")
		result["panes"] = {
			"camera": _rect_text(camera.get_global_rect()),
			"mapping": _rect_text(mapping.get_global_rect()),
			"final": _rect_text(final.get_global_rect()),
		}
	check(setup.get("camera_view") != null, "the camera pane has a camera view")
	check(setup.get("mapping_view") != null, "the mapping pane has a mapping view")
	check(setup.get("final_avatar") != null, "the final pane has an avatar texture rect")


func _check_overlay() -> void:
	var overlay: Dictionary = setup.call("get_camera_overlay")
	check(bool(overlay.get("has_frame", false)), "the camera pane has a raw ring frame")
	check(int(overlay.get("landmark_count", 0)) > 0, "the camera pane has tracker landmarks")
	var box: Rect2 = overlay.get("face_box", Rect2())
	check(box.size.x > 0.0 and box.size.y > 0.0, "the camera pane has a face box")
	var quad: PackedVector2Array = overlay.get("quad_points", PackedVector2Array())
	check(quad.size() == 4, "the camera pane has the homography quad")
	check(int(overlay.get("anchor_count", 0)) == 3, "the camera pane has three region anchors")
	result["overlay"] = {
		"landmark_count": int(overlay.get("landmark_count", 0)),
		"face_box": _box_text(box),
		"quad_points": quad.size(),
		"anchor_count": int(overlay.get("anchor_count", 0)),
		"has_frame": bool(overlay.get("has_frame", false)),
	}


func _check_mapping() -> void:
	var mapping: Dictionary = setup.call("get_mapping_values")
	check(int(mapping.get("region_count", 0)) == 3, "the mapping pane draws three regions")
	check(float(mapping.get("head_distance", 0.0)) > 0.0,
			"the mapping pane uses the actual head cut distance")
	check(float(mapping.get("head_falloff", 0.0)) > 0.0,
			"the mapping pane uses the actual head falloff")
	check(float(mapping.get("face_scale", 0.0)) > 0.0,
			"the mapping pane uses the actual face scale")
	var effective: Array = mapping.get("effective", [])
	check(effective.size() == 3, "the mapping pane has three effective region centres")
	result["mapping"] = {
		"region_count": int(mapping.get("region_count", 0)),
		"head_distance": float(mapping.get("head_distance", 0.0)),
		"head_falloff": float(mapping.get("head_falloff", 0.0)),
		"face_scale": float(mapping.get("face_scale", 0.0)),
		"effective": _vector_texts(effective),
	}


func _check_final_output() -> void:
	var avatar: TextureRect = setup.get("final_avatar")
	check(avatar != null and avatar.texture != null, "the final pane samples the avatar viewport")
	setup.call("set_background", "white")
	await process_frame
	var white: ColorRect = setup.get("final_background")
	check(white.color == Color.WHITE, "the final pane background toggles white")
	setup.call("set_background", "black")
	await process_frame
	var black: ColorRect = setup.get("final_background")
	check(black.color == Color.BLACK, "the final pane background toggles black")
	check(String(setup.call("get_background")) == "black", "the background state is black")
	result["background"] = {"white": true, "black": true}


func _check_edit_and_apply() -> void:
	var before: Dictionary = setup.call("get_calibration")
	check(not before.is_empty(), "the controller exposes the working calibration")
	if before.is_empty():
		return
	var before_region: Dictionary = Calibration.region(before, "eye_l")
	var before_center: Vector2 = before_region.center
	var before_mapping: Dictionary = setup.call("get_mapping_values")
	var before_effective: Array = before_mapping.get("effective", [])

	setup.call("set_region_center", "eye_l", Vector2(-0.78, 0.18))
	setup.call("set_region_size", "eye_l", Vector2(0.62, 0.40))
	await process_frame
	var after: Dictionary = setup.call("get_calibration")
	var after_region: Dictionary = Calibration.region(after, "eye_l")
	var after_center: Vector2 = after_region.center
	check(absf(after_center.x - -0.78) < 0.02 and absf(after_center.y - 0.18) < 0.02,
			"moving the eye L region updates the live calibration")
	check(absf(float(after_region.size.x) - 0.62) < 0.02,
			"resizing the eye L region updates the live calibration")
	var decal_region: Dictionary = controller.decal.get_region("eye_l")
	check(absf(float(decal_region.center.x) - -0.78) < 0.02,
			"the decal pass receives the moved region")
	var after_mapping: Dictionary = setup.call("get_mapping_values")
	var after_effective: Array = after_mapping.get("effective", [])
	check(after_effective.size() == 3 and before_effective.size() == 3 \
			and Vector2(after_effective[0]) != Vector2(before_effective[0]),
			"the mapping pane updates with the moved region")

	var applied: Dictionary = setup.call("apply_to_model")
	check(bool(applied.get("ok", false)), "Apply to model reports success")
	var path := ProjectSettings.globalize_path(test_path)
	check(FileAccess.file_exists(test_path), "Apply writes the calibration file")
	var loaded: Dictionary = Calibration.load_file(test_path)
	check(bool(loaded.get("ok", false)), "the saved calibration file parses")
	var loaded_values: Dictionary = loaded.get("values", {})
	var saved_region: Dictionary = Calibration.region(loaded_values, "eye_l")
	if not saved_region.is_empty():
		var saved_center: Array = saved_region.center
		check(absf(float(saved_center[0]) - -0.78) < 0.02,
				"the saved file carries the moved eye L centre")

	# Restart path: re-read the persisted file through the controller.
	var reload: Dictionary = controller.reload_calibration()
	check(bool(reload.get("ok", false)), "the controller reloads the persisted mapping")
	var restarted: Dictionary = controller.get_calibration()
	var restarted_region: Dictionary = Calibration.region(restarted, "eye_l")
	check(not restarted_region.is_empty() \
			and absf(float(restarted_region.center.x) - -0.78) < 0.02,
			"the applied mapping survives a reload")

	# Reset restores the provisional AABB mapping and does not save by itself.
	setup.call("reset_calibration")
	var reset: Dictionary = setup.call("get_calibration")
	var reset_region: Dictionary = Calibration.region(reset, "eye_l")
	check(not reset_region.is_empty() and absf(float(reset_region.center.x) - -0.5) < 0.02,
			"Reset restores the provisional eye L centre")
	result["calibration"] = {
		"before": _vector_text(before_center),
		"after": _vector_text(after_center),
		"decal": _vector_text(decal_region.center),
		"path": path,
	}
	result["persist"] = {
		"file": FileAccess.file_exists(test_path),
		"loaded": bool(loaded.get("ok", false)),
		"reloaded": bool(reload.get("ok", false)),
		"restarted": _vector_text(restarted_region.center) if not restarted_region.is_empty() else "",
	}


func _check_hold() -> void:
	var held := {
		"tracker_available": true,
		"features_valid": true,
		"have_output": true,
		"face": "not_visible",
	}
	check(bool(controller.should_show_decal(held)),
			"the Setup decal holds the FaceRig pose during the 150 ms loss window")
	var expired := held.duplicate()
	expired["features_valid"] = false
	check(not bool(controller.should_show_decal(expired)),
			"the Setup decal stops when the hold expires")
	var unavailable := held.duplicate()
	unavailable["tracker_available"] = false
	check(not bool(controller.should_show_decal(unavailable)),
			"a missing tracker never holds the Setup decal")

	# The separate OBS gate still blanks on the same loss while the setup decal
	# would still be showing the held pose.
	hud.call("set_debug_state", "face_not_visible")
	var image := Image.create(64, 36, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.2, 0.6, 0.9, 1.0))
	# The T23 output gate holds a recent complete sample for its own 150 ms
	# window; past that deadline an invalid face must blank, never freeze.
	var decision: Dictionary = hud.call("submit_output_image", image,
			_now_ns() + 250_000_000)
	check(String(decision.get("state", "")) == "blank",
			"the OBS output gate still blanks on invalid features")
	check(bool(controller.should_show_decal(held)),
			"the Setup hold and the OBS blank are independent")
	hud.call("set_debug_state", "healthy")
	result["hold"] = {
		"setup_holds": bool(controller.should_show_decal(held)),
		"expires": not bool(controller.should_show_decal(expired)),
		"obs_blank": String(decision.get("state", "")),
	}


func _check_selectors() -> void:
	var capture: OptionButton = setup.find_child("CaptureMode", true, false)
	var tracker: OptionButton = setup.find_child("TrackerInput", true, false)
	check(capture != null and capture.item_count == 4, "the capture-mode selector is present")
	check(tracker != null and tracker.item_count == 2, "the tracker-input selector is present")
	# The scripted/synthetic path restarts cleanly and reports the requested input.
	setup.call("_on_tracker_selected", 1)
	await process_frame
	var config: Dictionary = controller.get_config()
	check(int(config.get("tracker_input", 0)) == 256,
			"choosing 256 restarts the tracker with the requested input")
	setup.call("_on_tracker_selected", 0)
	await process_frame
	config = controller.get_config()
	check(int(config.get("tracker_input", 0)) == 192,
			"choosing 192 restarts the tracker with the requested input")
	result["selectors"] = {
		"capture_items": capture.item_count if capture != null else 0,
		"tracker_items": tracker.item_count if tracker != null else 0,
		"tracker_input": int(config.get("tracker_input", 0)),
	}


func _check_rendered() -> void:
	hud.call("set_debug_state", "healthy")
	for _i in range(6):
		await process_frame
	await RenderingServer.frame_post_draw
	var viewport_rect := root.get_visible_rect()
	var panes: Dictionary = setup.call("get_panes")
	var camera: Control = panes.get("camera")
	var mapping: Control = panes.get("mapping")
	var final: Control = panes.get("final")
	check(_rect_inside(camera.get_global_rect(), viewport_rect),
			"the camera pane is inside the window at 100%")
	check(_rect_inside(mapping.get_global_rect(), viewport_rect),
			"the mapping pane is inside the window at 100%")
	check(_rect_inside(final.get_global_rect(), viewport_rect),
			"the final pane is inside the window at 100%")
	var shot := root.get_texture().get_image()
	_save("T25c-setup-100", shot)
	check(shot != null and not shot.is_empty(), "the Setup window is drawable at 100%")
	if shot != null and not shot.is_empty():
		check(shot.get_pixel(2, 2).a > 0.9, "the Setup window background is opaque")
	result["layout"]["100"] = {
		"camera": _rect_text(camera.get_global_rect()),
		"mapping": _rect_text(mapping.get_global_rect()),
		"final": _rect_text(final.get_global_rect()),
		"viewport": _rect_text(viewport_rect),
	}

	# Background readability evidence: sample a background corner of the final
	# pane with the avatar centred.
	setup.call("set_background", "white")
	for _i in range(4):
		await process_frame
	await RenderingServer.frame_post_draw
	var white_shot := root.get_texture().get_image()
	_save("T25c-setup-white", white_shot)
	setup.call("set_background", "black")
	for _i in range(4):
		await process_frame
	await RenderingServer.frame_post_draw
	var black_shot := root.get_texture().get_image()
	_save("T25c-setup-black", black_shot)
	var sample_point := Vector2i(int(final.get_global_rect().position.x + 6),
			int(final.get_global_rect().position.y + 30))
	if white_shot != null and black_shot != null:
		var white_pixel := white_shot.get_pixelv(sample_point)
		var black_pixel := black_shot.get_pixelv(sample_point)
		check(white_pixel.r > 0.75 and white_pixel.g > 0.75,
				"the final pane reads on a white background")
		check(black_pixel.r < 0.20 and black_pixel.g < 0.20,
				"the final pane reads on a black background")
		result["background"]["white_pixel"] = _colour_text(white_pixel)
		result["background"]["black_pixel"] = _colour_text(black_pixel)

	hud.call("cycle_ui_scale")
	hud.call("cycle_ui_scale")
	for _i in range(6):
		await process_frame
	await RenderingServer.frame_post_draw
	var scaled_rect := root.get_visible_rect()
	check(_rect_inside(camera.get_global_rect(), scaled_rect),
			"the camera pane is inside the window at 200%")
	check(_rect_inside(mapping.get_global_rect(), scaled_rect),
			"the mapping pane is inside the window at 200%")
	check(_rect_inside(final.get_global_rect(), scaled_rect),
			"the final pane is inside the window at 200%")
	var scaled := root.get_texture().get_image()
	_save("T25c-setup-200", scaled)
	check(scaled != null and not scaled.is_empty(), "the Setup window is drawable at 200%")
	result["layout"]["200"] = {
		"camera": _rect_text(camera.get_global_rect()),
		"mapping": _rect_text(mapping.get_global_rect()),
		"final": _rect_text(final.get_global_rect()),
		"viewport": _rect_text(scaled_rect),
	}
	hud.call("cycle_ui_scale")
	setup.call("set_background", "black")


func _remove(path: String) -> void:
	var absolute := ProjectSettings.globalize_path(path)
	if FileAccess.file_exists(path):
		DirAccess.remove_absolute(absolute)


func _rect_inside(inner: Rect2, outer: Rect2) -> bool:
	return outer.encloses(inner)


func _rect_text(rect: Rect2) -> String:
	return "%d,%d %dx%d" % [int(rect.position.x), int(rect.position.y),
			int(rect.size.x), int(rect.size.y)]


func _box_text(rect: Rect2) -> String:
	return "%.3f,%.3f %.3fx%.3f" % [rect.position.x, rect.position.y,
			rect.size.x, rect.size.y]


func _vector_text(value: Vector2) -> String:
	return "%.3f,%.3f" % [value.x, value.y]


func _vector_texts(values: Array) -> Array:
	var out: Array = []
	for value in values:
		out.append(_vector_text(Vector2(value)))
	return out


func _colour_text(colour: Color) -> String:
	return "%.2f,%.2f,%.2f,%.2f" % [colour.r, colour.g, colour.b, colour.a]


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
	var path := ProjectSettings.globalize_path("res://../.tools/T25c-setup%s.json" % suffix)
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(JSON.stringify(result, "  "))
	file.close()
	print("T25c ", JSON.stringify(result))
	quit(0 if failures.is_empty() else 1)
