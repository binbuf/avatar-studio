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
const SourceFit = preload("res://scenes/source_region_autofit.gd")

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
	"source": {},
	"freeze": {},
	"direct": {},
}
var rendering := false
var scene: Node = null
var hud: CanvasLayer = null
var setup = null
var controller = null
var test_path := "user://T25c-setup-test.json"
var source_kind := ""
var result_tag := ""


func _initialize() -> void:
	rendering = "--render" in OS.get_cmdline_user_args()
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--result-tag="):
			result_tag = argument.substr("--result-tag=".length())
	_run.call_deferred()


func check(condition: bool, message: String) -> void:
	if condition:
		print("  ok   ", message)
	else:
		failures.append(message)
		printerr("  FAIL ", message)


func _run() -> void:
	await process_frame
	# The headless window defaults to a tiny surface. Use the same 720p layout
	# the operator sees so pane sizes and direct manipulation are meaningful.
	if not rendering:
		root.size = Vector2i(1280, 720)
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--content-scale="):
			root.content_scale_factor = float(argument.substr("--content-scale=".length()))
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
	source_kind = String(controller.get_config().get("source", ""))

	# A deterministic calibration file for this test; never touch the operator's.
	controller.set_calibration_path(test_path)
	_remove(test_path)
	var reloaded: Dictionary = controller.reload_calibration()
	check(bool(reloaded.get("ok", false)), "the controller reloads the provisional mapping")

	hud.call("open_setup")
	for _i in range(24):
		await process_frame

	_check_source()
	_check_panes()
	_check_overlay()
	_check_mapping()
	await _check_direct_manipulation()
	await _check_final_output()
	await _check_edit_and_apply()
	_check_hold()
	await _check_freeze()
	await _check_auto_fit_and_eye_lock()
	if rendering:
		await _check_rendered()
	await _check_selectors()

	_remove(test_path)
	result["live"] = bool(hud.call("is_live"))
	_finish()


func _check_source() -> void:
	var config: Dictionary = controller.get_config()
	var source := String(config.get("source", ""))
	var label := String(setup.call("get_source_label"))
	if source == "still":
		check(label == "still (file)", "the setup status labels the still file source")
		var still_path := String(config.get("still_image", ""))
		check(still_path != "", "the still source reports its path")
		check(still_path.ends_with(".png") or still_path.ends_with(".jpg")
				or still_path.ends_with(".jpeg"), "the still source path is an image")
		check(bool(controller.last_state.get("have_output", false))
				or String(controller.last_state.get("face", "")) == "tracked"
				or String(controller.last_state.get("face", "")) == "degraded",
				"the still source runs the tracker/FaceRig path")
		result["source"] = {"kind": source, "label": label, "still_image": still_path}
	else:
		check(label == "synthetic (dev)" or label == "camera",
				"the setup status labels the live source")
		result["source"] = {"kind": source, "label": label, "still_image": ""}


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
			"status": _rect_text((panes.get("status") as Control).get_global_rect()),
			"controls": _rect_text((panes.get("controls") as Control).get_global_rect()),
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


func _check_direct_manipulation() -> void:
	var image: Rect2 = setup.call("get_camera_image_rect")
	check(image.size.x > 1.0 and image.size.y > 1.0,
			"the camera view exposes a laid-out image rect")
	if image.size.x <= 1.0 or image.size.y <= 1.0:
		return
	var overlay: Dictionary = setup.call("get_camera_overlay")
	var box: Rect2 = overlay.get("face_box", Rect2())
	check(box.size.x > 0.0 and box.size.y > 0.0,
			"the camera draw interaction has a face box to aim at")
	if box.size.x <= 0.0 or box.size.y <= 0.0:
		return
	# Draw the right eye around the right side of the tracked face box.
	var draw_rect := Rect2(
			image.position + Vector2(
					(box.position.x + box.size.x * 0.52) * image.size.x,
					(box.position.y + box.size.y * 0.20) * image.size.y),
			Vector2(box.size.x * 0.22, box.size.y * 0.14) * image.size)
	var before: Dictionary = Calibration.source_region(setup.call("get_calibration"), "eye_r")
	var drawn: Dictionary = setup.call("draw_region_from_screen_rect", "eye_r", draw_rect)
	check(bool(drawn.get("ok", false)), "drawing a box on the camera applies a region")
	var after: Dictionary = Calibration.source_region(setup.call("get_calibration"), "eye_r")
	if not before.is_empty() and not after.is_empty():
		check(Vector2(after.center).distance_to(Vector2(before.center)) > 0.001,
				"the drawn box moves the eye R camera cutout")
		check(absf(float(after.size.x) - float(before.size.x)) > 0.001
				or absf(float(after.size.y) - float(before.size.y)) > 0.001,
				"the drawn box resizes the eye R camera cutout")

	# The model view projects canonical face points onto the avatar pane and
	# unprojects pointer positions back onto the same face plane.
	var model_view = setup.call("get_model_view")
	check(model_view != null, "the final pane has a model view")
	var model_ok := false
	var model_draw_ok := false
	var model_resize_ok := false
	var camera_drag_ok := false
	var camera_resize_ok := false
	var mapping_draw_ok := false
	var mapping_drag_ok := false
	var mapping_resize_ok := false
	var mapping_view = setup.call("get_mapping_view")
	if model_view != null:
		var mouth: Dictionary = Calibration.region(setup.call("get_calibration"), "mouth")
		var centre: Vector2 = mouth.center
		var screen: Variant = model_view.call("screen_from_canonical", centre)
		check(screen != null, "the model view projects the mouth onto the avatar")
		if screen != null:
			var round_trip: Variant = model_view.call("canonical_from_screen", screen)
			check(round_trip != null
					and Vector2(round_trip).distance_to(centre) < 0.05,
					"the model view round-trips canonical face coordinates")
			var target: Vector2 = Vector2(screen) + Vector2(12, -8)
			var dragged: Dictionary = setup.call("drag_region_on_model", "mouth", target)
			check(bool(dragged.get("ok", false)),
					"dragging on the model applies the region move")
			var moved: Dictionary = Calibration.region(setup.call("get_calibration"), "mouth")
			model_ok = Vector2(moved.center).distance_to(centre) > 0.001
			check(model_ok, "the model drag moves the mouth centre")

			# Draw a box on the model: it sets the region centre and size.
			var eye: Dictionary = Calibration.region(setup.call("get_calibration"), "eye_l")
			var eye_screen: Variant = model_view.call("screen_from_canonical",
					Vector2(eye.center))
			if eye_screen != null:
				var model_box := Rect2(Vector2(eye_screen) - Vector2(20, 14),
						Vector2(40, 28))
				var before_size := Vector2(eye.size)
				var model_drawn: Dictionary = setup.call("draw_region_on_model", "eye_l",
						model_box)
				check(bool(model_drawn.get("ok", false)),
						"drawing a box on the model applies a region")
				var after_eye: Dictionary = Calibration.region(
						setup.call("get_calibration"), "eye_l")
				check(Vector2(after_eye.size).distance_to(before_size) > 0.001,
						"the model box resizes the eye L shape")
				# The handle path scales about the centre.
				var scaled: Dictionary = setup.call("resize_region_on_model", "eye_l",
						Vector2(0.30, 0.20))
				check(bool(scaled.get("ok", false)),
						"the model handle path applies a new size")
				var scaled_region: Dictionary = Calibration.region(
						setup.call("get_calibration"), "eye_l")
				check(absf(float(scaled_region.size.x) - 0.30) < 0.02,
						"the model resize updates the eye L width")
				model_draw_ok = bool(model_drawn.get("ok", false))
				model_resize_ok = bool(scaled.get("ok", false))

	# Camera pane: move and scale an existing cutout. The synthetic/scripted
	# homography is identity, so aim at the face box centre rather than the
	# authored canonical point (which can project outside the image).
	var camera_view = setup.get("camera_view")
	if camera_view != null:
		var face_centre := image.position + Vector2(
				(box.position.x + box.size.x * 0.5) * image.size.x,
				(box.position.y + box.size.y * 0.5) * image.size.y)
		var before_cam: Vector2 = Vector2(Calibration.source_region(
				setup.call("get_calibration"), "eye_l").center)
		var camera_drag: Dictionary = setup.call("drag_region_on_camera", "eye_l",
				face_centre + Vector2(8, -6))
		camera_drag_ok = bool(camera_drag.get("ok", false)) \
				and Vector2(Calibration.source_region(setup.call("get_calibration"),
						"eye_l").center).distance_to(before_cam) > 0.001
		check(camera_drag_ok, "dragging on the camera moves a region")
		var camera_resize: Dictionary = setup.call("resize_region_on_camera", "eye_l",
				Vector2(0.34, 0.22))
		camera_resize_ok = bool(camera_resize.get("ok", false))
		check(camera_resize_ok, "the camera handle path applies a new size")

	# Mapping pane: draw, move and scale on the canonical grid.
	check(mapping_view != null, "the mapping pane has a mapping view")
	if mapping_view != null:
		var mouth_centre: Vector2 = Vector2(Calibration.source_region(
				setup.call("get_calibration"), "mouth").center)
		var mapping_screen: Vector2 = mapping_view.call("screen_from_canonical",
				mouth_centre)
		var mapping_box := Rect2(mapping_screen - Vector2(16, 12), Vector2(32, 24))
		var mapping_drawn: Dictionary = setup.call("draw_region_on_mapping", "mouth",
				mapping_box)
		mapping_draw_ok = bool(mapping_drawn.get("ok", false))
		check(mapping_draw_ok, "drawing a box on the mapping view applies a region")
		var before_map: Vector2 = Vector2(Calibration.source_region(
				setup.call("get_calibration"), "eye_r").center)
		var eye_r_screen: Vector2 = mapping_view.call("screen_from_canonical", before_map)
		var mapping_drag: Dictionary = setup.call("drag_region_on_mapping", "eye_r",
				eye_r_screen + Vector2(10, -8))
		mapping_drag_ok = bool(mapping_drag.get("ok", false)) \
				and Vector2(Calibration.source_region(setup.call("get_calibration"),
						"eye_r").center).distance_to(before_map) > 0.001
		check(mapping_drag_ok, "dragging on the mapping view moves a region")
		var mapping_resize: Dictionary = setup.call("resize_region_on_mapping", "eye_r",
				Vector2(0.36, 0.24))
		mapping_resize_ok = bool(mapping_resize.get("ok", false))
		check(mapping_resize_ok, "the mapping handle path applies a new size")

	# Regression: clicking a cutout emits `region_selected`, and the Setup
	# window used to clear the draw modes on that signal, which reset the pane's
	# drag state and made the cutout impossible to position. Drive the real
	# press/motion/release path on all three panes.
	var mapping_input_ok := false
	if mapping_view != null:
		var mouth_now: Vector2 = Vector2(Calibration.source_region(
				setup.call("get_calibration"), "mouth").center)
		var start: Vector2 = mapping_view.call("screen_from_canonical", mouth_now)
		_press(mapping_view, start)
		_motion(mapping_view, start + Vector2(14, -10))
		_release(mapping_view, start + Vector2(14, -10))
		var after_input: Dictionary = Calibration.source_region(
				setup.call("get_calibration"), "mouth")
		mapping_input_ok = Vector2(after_input.center).distance_to(mouth_now) > 0.001
		check(mapping_input_ok, "selecting then dragging on the mapping view moves the cutout")

	var model_input_ok := false
	if model_view != null:
		var eye_now: Vector2 = Vector2(Calibration.region(
				setup.call("get_calibration"), "eye_l").center)
		var eye_point: Variant = model_view.call("screen_from_canonical", eye_now)
		if eye_point != null:
			_press(model_view, Vector2(eye_point))
			_motion(model_view, Vector2(eye_point) + Vector2(12, -8))
			_release(model_view, Vector2(eye_point) + Vector2(12, -8))
			var after_model: Dictionary = Calibration.region(
					setup.call("get_calibration"), "eye_l")
			model_input_ok = Vector2(after_model.center).distance_to(eye_now) > 0.001
			check(model_input_ok, "selecting then dragging on the model moves the cutout")

	var camera_input_ok := false
	var camera_track_ok := false
	if camera_view != null:
		var values: Dictionary = setup.call("get_calibration")
		var regions: Array = values.get("source_regions", values.get("regions", []))
		var h1 := PackedFloat32Array([0.12, 0.0, 0.5, 0.0, 0.12, 0.5, 0.0, 0.0, 1.0])
		camera_view.call("set_overlay", PackedVector2Array(), Rect2(), h1, [], regions, "")
		var first_poly: Dictionary = camera_view.call("get_region_polygons")
		var first_centroid := _polygon_centroid(first_poly.get("eye_l",
				PackedVector2Array()))
		var eye_before: Vector2 = Vector2(Calibration.source_region(
				setup.call("get_calibration"), "eye_l").center)
		if first_centroid != Vector2(-1, -1):
			_press(camera_view, first_centroid)
			_motion(camera_view, first_centroid + Vector2(12, -8))
			_release(camera_view, first_centroid + Vector2(12, -8))
			var after_camera: Dictionary = Calibration.source_region(
					setup.call("get_calibration"), "eye_l")
			camera_input_ok = Vector2(after_camera.center).distance_to(eye_before) > 0.001
			check(camera_input_ok, "selecting then dragging on the camera moves the cutout")

		# A moving face changes the homography and the anchors; the camera
		# cutout must follow both, not stay at its authored place.
		var h2 := PackedFloat32Array([0.13, 0.0, 0.58, 0.0, 0.13, 0.56, 0.0, 0.0, 1.0])
		camera_view.call("set_overlay", PackedVector2Array(), Rect2(), h2, [], regions, "")
		var moved_poly: Dictionary = camera_view.call("get_region_polygons")
		var moved_centroid := _polygon_centroid(moved_poly.get("eye_l",
				PackedVector2Array()))
		var anchored: Array = regions.duplicate(true)
		if not anchored.is_empty():
			anchored[0].center = Vector2(anchored[0].center) + Vector2(0.06, -0.03)
		camera_view.call("set_overlay", PackedVector2Array(), Rect2(), h2, [], anchored, "")
		var anchored_poly: Dictionary = camera_view.call("get_region_polygons")
		var anchored_centroid := _polygon_centroid(anchored_poly.get("eye_l",
				PackedVector2Array()))
		camera_track_ok = first_centroid != Vector2(-1, -1) \
				and moved_centroid.distance_to(first_centroid) > 1.0 \
				and anchored_centroid.distance_to(moved_centroid) > 1.0
		check(camera_track_ok,
				"the camera cutout follows the live homography and region anchor")

	result["direct"] = {
		"camera_image": _rect_text(image),
		"drawn": bool(drawn.get("ok", false)),
		"eye_r_center": _vector_text(Vector2(after.get("center", Vector2.ZERO))),
		"model_view": model_view != null,
		"model_drag": model_ok,
		"model_draw": model_draw_ok,
		"model_resize": model_resize_ok,
		"model_input_drag": model_input_ok,
		"camera_drag": camera_drag_ok,
		"camera_resize": camera_resize_ok,
		"camera_input_drag": camera_input_ok,
		"camera_track": camera_track_ok,
		"mapping_draw": mapping_draw_ok,
		"mapping_drag": mapping_drag_ok,
		"mapping_resize": mapping_resize_ok,
		"mapping_input_drag": mapping_input_ok,
	}
	await process_frame


func _press(view: Control, point: Vector2) -> void:
	var event := InputEventMouseButton.new()
	event.button_index = MOUSE_BUTTON_LEFT
	event.pressed = true
	event.position = point
	view._gui_input(event)


func _motion(view: Control, point: Vector2) -> void:
	var event := InputEventMouseMotion.new()
	event.position = point
	view._gui_input(event)


func _release(view: Control, point: Vector2) -> void:
	var event := InputEventMouseButton.new()
	event.button_index = MOUSE_BUTTON_LEFT
	event.pressed = false
	event.position = point
	view._gui_input(event)


func _polygon_centroid(points: PackedVector2Array) -> Vector2:
	if points.is_empty():
		return Vector2(-1, -1)
	var sum := Vector2.ZERO
	for point in points:
		sum += point
	return sum / float(points.size())


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

	setup.call("set_region_center", "eye_l", Vector2(-0.78, 0.18))
	setup.call("set_region_size", "eye_l", Vector2(0.62, 0.40))
	await process_frame
	var after: Dictionary = setup.call("get_calibration")
	var after_region: Dictionary = Calibration.region(after, "eye_l")
	var after_center: Vector2 = after_region.center
	check(absf(after_center.x - -0.78) < 0.02 and absf(after_center.y - 0.18) < 0.02,
			"moving the eye L model placement updates the live calibration")
	check(absf(float(after_region.size.x) - 0.62) < 0.02,
			"resizing the eye L model placement updates the live calibration")
	var decal_region: Dictionary = controller.decal.get_region("eye_l")
	check(absf(float(decal_region.center.x) - -0.78) < 0.02,
			"the decal pass receives the moved model placement")

	# The mapping pane is bound to the camera view, so it follows the source
	# selection, not the model placement.
	var source_before: Array = setup.call("get_mapping_values").get("effective", [])
	setup.call("set_source_region_center", "eye_l", Vector2(-0.62, 0.30))
	setup.call("set_source_region_size", "eye_l", Vector2(0.50, 0.36))
	var source_after: Array = setup.call("get_mapping_values").get("effective", [])
	check(source_after.size() == 3 and source_before.size() == 3 \
			and Vector2(source_after[0]).distance_to(Vector2(source_before[0])) > 0.001,
			"the mapping pane updates with the camera selection")
	var after_source: Dictionary = Calibration.source_region(
			setup.call("get_calibration"), "eye_l")
	check(absf(float(after_source.center.x) - -0.62) < 0.02,
			"the camera selection is independent of the model placement")
	var target_after_source: Dictionary = Calibration.region(
			setup.call("get_calibration"), "eye_l")
	check(absf(float(target_after_source.center.x) - -0.78) < 0.02,
			"moving the camera selection leaves the model placement alone")

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
				"the saved file carries the moved eye L model centre")
	var saved_source: Dictionary = Calibration.source_region(loaded_values, "eye_l")
	if not saved_source.is_empty():
		var saved_source_center: Array = saved_source.center
		check(absf(float(saved_source_center[0]) - -0.62) < 0.02,
				"the saved file carries the separate camera selection")

	# Restart path: re-read the persisted file through the controller.
	var reload: Dictionary = controller.reload_calibration()
	check(bool(reload.get("ok", false)), "the controller reloads the persisted mapping")
	var restarted: Dictionary = controller.get_calibration()
	var restarted_region: Dictionary = Calibration.region(restarted, "eye_l")
	check(not restarted_region.is_empty() \
			and absf(float(restarted_region.center.x) - -0.78) < 0.02,
			"the applied mapping survives a reload")

	# Best defaults restores the model-aware mapping, re-arms the automatic fit,
	# and does not save by itself.
	setup.call("reset_calibration")
	var reset: Dictionary = setup.call("get_calibration")
	var reset_region: Dictionary = Calibration.region(reset, "eye_l")
	var reset_mouth: Dictionary = Calibration.region(reset, "mouth")
	var reset_source: Dictionary = Calibration.source_region(reset, "eye_l")
	check(not reset_region.is_empty() and absf(float(reset_region.center.x) - -0.5) < 0.02,
			"Best defaults restores the eye L centre")
	check(not reset_region.is_empty() and absf(float(reset_region.center.y)) < 0.02,
			"Best defaults puts the eye line on the canonical origin")
	check(not reset_mouth.is_empty() and absf(float(reset_mouth.center.y) - -0.58) < 0.02,
			"Best defaults places the model mouth below the eye line")
	check(not reset_source.is_empty() and absf(float(reset_source.size.x) - 0.44) < 0.02,
			"Best defaults gives the camera a stable eye sampling window")
	check(bool(setup.call("get_auto_fit")),
			"Best defaults re-arms the automatic camera fit")
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


func _wait_seconds(seconds: float) -> void:
	var start := Time.get_ticks_usec()
	var deadline := int(seconds * 1_000_000.0)
	while Time.get_ticks_usec() - start < deadline:
		await process_frame


func _check_freeze() -> void:
	await process_frame
	var before: Dictionary = controller.last_state.duplicate()
	var face_before := String(before.get("face", ""))
	var confidence_before := float(before.get("confidence", 0.0))
	var timestamp_before := int(before.get("source_timestamp_ns", 0))
	check(face_before == "tracked" or face_before == "degraded",
			"a face is tracked before the snapshot")
	check(confidence_before > 0.0, "tracking confidence is nonzero before the snapshot")
	var snapshot_path := ProjectSettings.globalize_path("user://T25c-snapshot.png")
	var tools_path := ProjectSettings.globalize_path("res://../.tools/T25c-snapshot.png")
	_remove("user://T25c-snapshot.png")
	if FileAccess.file_exists(tools_path):
		DirAccess.remove_absolute(tools_path)
	setup.call("_on_snapshot_pressed")
	check(FileAccess.file_exists(snapshot_path), "Snapshot writes a PNG to user://")
	check(FileAccess.file_exists(tools_path), "Snapshot copies the PNG to .tools")
	check(bool(setup.call("is_frozen")), "the Setup window reports frozen")
	check(String(setup.call("get_source_label")) == "still (dev)",
			"the frozen source is labelled still (dev)")

	# Longer than both the 500 ms result timeout and the 150 ms loss hold.
	await _wait_seconds(1.2)
	var held: Dictionary = controller.last_state
	check(bool(held.get("frozen", false)), "the pipeline state reports frozen")
	check(String(held.get("face", "")) == face_before,
			"the frozen face state does not time out")
	check(absf(float(held.get("confidence", 0.0)) - confidence_before) < 0.02,
			"the frozen confidence is held")
	check(int(held.get("source_timestamp_ns", 0)) == timestamp_before,
			"the frozen pose timestamp is held")
	var overlay: Dictionary = setup.call("get_camera_overlay")
	check(bool(overlay.get("has_frame", false)), "the camera pane holds the frozen frame")

	var image := Image.create(64, 36, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.2, 0.6, 0.9, 1.0))
	var frozen_decision: Dictionary = hud.call("submit_output_image", image,
			_now_ns() + 250_000_000)
	check(String(frozen_decision.get("state", "")) == "blank",
			"the OBS output blanks while the diagnostic surface is frozen")

	# Use camera (or a direct unfreeze for a still run, which must not need a
	# camera) resumes the live feed.
	var resumed := false
	if source_kind == "still":
		resumed = bool(controller.set_frozen(false).get("ok", false))
	else:
		resumed = bool(setup.call("use_camera"))
	check(resumed, "Use camera unfreezes the pipeline")
	check(not bool(setup.call("is_frozen")), "the Setup window reports live again")
	await _wait_seconds(0.5)
	var live_state: Dictionary = controller.last_state
	check(not bool(live_state.get("frozen", true)), "the pipeline resumes after unfreeze")
	check(String(live_state.get("face", "")) == "tracked"
			or String(live_state.get("face", "")) == "degraded",
			"the resumed pipeline tracks again")
	var live_decision: Dictionary = hud.call("submit_output_image", image, _now_ns())
	check(String(live_decision.get("state", "")) == "avatar",
			"the OBS output resumes after unfreeze")
	_remove("user://T25c-freeze-test.png")
	result["freeze"] = {
		"snapshot": tools_path,
		"face": face_before,
		"confidence": confidence_before,
		"timestamp": timestamp_before,
		"held_face": String(held.get("face", "")),
		"held_confidence": float(held.get("confidence", 0.0)),
		"held_timestamp": int(held.get("source_timestamp_ns", 0)),
		"held_features": bool(held.get("features_valid", false)),
		"obs_frozen": String(frozen_decision.get("state", "")),
		"obs_resumed": String(live_decision.get("state", "")),
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
	var sample_point := Vector2i(
			int((final.get_global_rect().position.x + 6) * root.content_scale_factor),
			int((final.get_global_rect().position.y + 30) * root.content_scale_factor))
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
	check(camera.get_global_rect().size.y >= 120.0,
			"the camera pane is readable at 200%")
	check(mapping.get_global_rect().size.y >= 120.0,
			"the mapping pane is readable at 200%")
	check(final.get_global_rect().size.y >= 120.0,
			"the final pane is readable at 200%")
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


## Auto-fit the camera cutouts from the tracker contours, and the eye symmetry
## lock. The fit is exercised against fabricated 478-point landmarks so the
## result is deterministic without a camera.
func _check_auto_fit_and_eye_lock() -> void:
	# H: canonical (u, v) -> normalised image. 0.2 image units per canonical
	# unit, origin at the image centre, v pointing up.
	var h := PackedFloat32Array([0.2, 0.0, 0.5, 0.0, -0.2, 0.5, 0.0, 0.0, 1.0])
	var points: Array = []
	points.resize(478)
	for index in range(points.size()):
		points[index] = Vector2(0.5, 0.5)
	_fill_contour(points, SourceFit.EYE_R_CONTOUR, Vector2(-0.5, 0.25),
			Vector2(0.30, 0.12), h)
	_fill_contour(points, SourceFit.EYE_L_CONTOUR, Vector2(0.5, 0.25),
			Vector2(0.30, 0.12), h)
	_fill_contour(points, SourceFit.MOUTH_CONTOUR, Vector2(0.0, -0.22),
			Vector2(1.18, 0.20), h)
	# A 0.70-wide nose: the mouth prior caps a wide smile instead of sampling
	# the cheeks. The synthetic test face uses a tiny nose, so this is the only
	# place the proportion path is exercised.
	points[129] = _apply_homography(h, Vector2(0.35, -0.27))
	points[358] = _apply_homography(h, Vector2(-0.35, -0.27))
	var landmarks := PackedVector2Array(points)

	var values: Dictionary = setup.call("get_calibration")
	var targets: Array = values.get("regions", [])
	var measured: Dictionary = SourceFit.measure(landmarks, h, targets)
	check(bool(measured.get("ok", false)), "auto-fit measures a complete face")
	var fitted: Array = measured.get("regions", [])
	check(fitted.size() == 3, "auto-fit returns one camera cutout per region")
	var eye_r: Dictionary = _entry(fitted, "eye_r")
	var mouth: Dictionary = _entry(fitted, "mouth")
	var target_eye: Dictionary = _entry(targets, "eye_r")
	check(not eye_r.is_empty()
			and Vector2(eye_r.center).distance_to(Vector2(-0.5, 0.25)) < 0.01,
			"auto-fit centres the eye on the tracked corners")
	if not eye_r.is_empty() and not target_eye.is_empty():
		var aspect := float(target_eye.size.x) / float(target_eye.size.y)
		check(absf(float(eye_r.size.x) / float(eye_r.size.y) - aspect) < 0.02,
				"auto-fit matches the painted aspect so the patch is not stretched")
		check(absf(float(eye_r.size.x) - 0.30 * SourceFit.EYE_MARGIN) < 0.02,
				"auto-fit sizes the eye from the tracked corner width")
	check(not mouth.is_empty() and Vector2(mouth.center).y < 0.0,
			"auto-fit places the mouth below the eyes")
	var quality: Dictionary = measured.get("quality", {})
	check(absf(float(quality.get("nose_width", 0.0)) - 0.70) < 0.02,
			"auto-fit measures the nose proportion")
	check(not mouth.is_empty() and float(mouth.size.x) < 1.05,
			"the mouth prior caps a wide smile instead of sampling the cheeks")
	check(not mouth.is_empty() and float(mouth.size.x) < 1.18,
			"the capped mouth window is narrower than the raw smile")
	var fallback: Array = SourceFit.fit(PackedVector2Array(), h, targets)
	check(fallback.is_empty(), "auto-fit declines without dense landmarks")
	var degenerate: Array = SourceFit.fit(landmarks,
			PackedFloat32Array([0, 0, 0, 0, 0, 0, 0, 0, 0]), targets)
	check(degenerate.is_empty(), "a singular homography declines instead of failing")
	var settled: Dictionary = SourceFit.settle({
		"eye_r": {"centers": [Vector2(-0.5, 0.25), Vector2(-0.5, 0.25),
				Vector2(-0.5, 0.25)], "widths": [0.40, 0.50, 0.45]},
		"mouth": {"centers": [Vector2(0.0, -0.22), Vector2(0.0, -0.22)],
				"widths": [0.55, 1.18]},
	})
	check(absf(float(settled.get("eye_r", {}).get("width", 0.0)) - 0.45) < 0.001,
			"the settled eye width is the median")
	check(absf(float(settled.get("mouth", {}).get("width", 0.0)) - 0.865) < 0.001,
			"the settled mouth width is the median")
	var box_points: Array = []
	box_points.resize(478)
	for index in range(box_points.size()):
		box_points[index] = Vector2(0.5, 0.5)
	for entry in [[10, 0.50, 0.30], [152, 0.50, 0.70], [127, 0.30, 0.50],
			[356, 0.70, 0.50], [234, 0.31, 0.60], [454, 0.69, 0.60],
			[50, 0.35, 0.55], [280, 0.65, 0.55], [33, 0.38, 0.45],
			[263, 0.62, 0.45], [61, 0.40, 0.58], [291, 0.60, 0.58]]:
		box_points[int(entry[0])] = Vector2(float(entry[1]), float(entry[2]))
	var frontal_box := SourceFit.face_box(PackedVector2Array(box_points), 0.0)
	var turned_box := SourceFit.face_box(PackedVector2Array(box_points), 40.0)
	check(frontal_box.size.x > 0.4, "the face box covers the landmark outline")
	check(turned_box.size.x > frontal_box.size.x * 1.2,
			"the face box widens when the head turns")
	var fallback_h := SourceFit.face_box_homography(Rect2(0.2, 0.2, 0.4, 0.5))
	check(absf(fallback_h[0] - 0.4 / 2.09) < 0.001
			and absf(fallback_h[2] - 0.4) < 0.001
			and absf(fallback_h[5] - 0.36) < 0.001,
			"the face-box fallback puts the eye line at 32% down the box")

	# A manual camera edit is the operator's decision: it turns the auto-fit off
	# so the next tick cannot overwrite it.
	setup.call("set_auto_fit", true)
	check(bool(setup.call("get_auto_fit")), "auto-fit can be switched on")
	# With the real tracker (still source) the live path must actually fit the
	# cutouts, not just pass the pure-function check above.
	if controller.get_landmarks().size() >= 468 \
			and bool(controller.last_state.get("features_valid", false)):
		for _i in range(90):
			await process_frame
		var live_state: Dictionary = setup.call("get_auto_fit_state")
		check(live_state.size() == 3, "the live auto-fit applies to all three cutouts")
		var live_quality: Dictionary = setup.call("get_auto_fit_quality")
		check(live_quality.has("yaw_deg") and live_quality.has("mouth_width"),
				"the live auto-fit reports head angles and measured widths")
		check(float(live_quality.get("mouth_width", 2.0)) <= 1.05,
				"the live mouth window is capped by the face proportion")
		var live_box: Rect2 = setup.call("get_measured_face_box")
		check(live_box.size.x > 0.0 and live_box.size.y > 0.0,
				"the live measurement supplies a face box")
		var fit_text: Array = []
		for fit_id in ["eye_l", "eye_r", "mouth"]:
			var fit_entry: Dictionary = live_state.get(fit_id, {})
			if not fit_entry.is_empty():
				fit_text.append("%s %s %s" % [fit_id,
						_vector_text(Vector2(fit_entry.center)),
						_vector_text(Vector2(fit_entry.size))])
		result["direct"]["auto_fit_state"] = fit_text
		var live_source: Dictionary = Calibration.source_region(
				setup.call("get_calibration"), "eye_r")
		var live_target: Dictionary = Calibration.region(
				setup.call("get_calibration"), "eye_r")
		if not live_source.is_empty() and not live_target.is_empty():
			check(absf(float(live_source.size.x) / float(live_source.size.y)
					- float(live_target.size.x) / float(live_target.size.y)) < 0.05,
					"the live camera cutout matches the painted aspect")
	setup.call("resize_region_on_camera", "eye_l", Vector2(0.40, 0.30))
	check(not bool(setup.call("get_auto_fit")),
			"a manual camera edit turns the auto-fit off")
	var manual: Dictionary = Calibration.source_region(setup.call("get_calibration"), "eye_l")
	check(absf(float(manual.size.x) - 0.40) < 0.02,
			"the hand-placed camera size is kept")

	# Eye lock: the model pane edits one eye, the partner mirrors it.
	setup.call("set_lock_eyes", true)
	check(bool(setup.call("get_lock_eyes")), "the eye lock can be switched on")
	var model_view = setup.call("get_model_view")
	var eye_l: Dictionary = Calibration.region(setup.call("get_calibration"), "eye_l")
	var screen: Variant = model_view.call("screen_from_canonical", Vector2(eye_l.center))
	if screen != null:
		var moved: Dictionary = setup.call("drag_region_on_model", "eye_l",
				Vector2(screen) + Vector2(10, -6))
		check(bool(moved.get("ok", false)), "a locked eye drag applies")
		var left: Dictionary = Calibration.region(setup.call("get_calibration"), "eye_l")
		var right: Dictionary = Calibration.region(setup.call("get_calibration"), "eye_r")
		check(absf(float(left.center.x) + float(right.center.x)) < 0.02,
				"locking the eyes mirrors their u position")
		check(absf(float(left.center.y) - float(right.center.y)) < 0.02,
				"locking the eyes keeps their v position equal")
		var resized: Dictionary = setup.call("resize_region_on_model", "eye_l",
				Vector2(0.50, 0.36))
		check(bool(resized.get("ok", false)), "a locked eye resize applies")
		left = Calibration.region(setup.call("get_calibration"), "eye_l")
		right = Calibration.region(setup.call("get_calibration"), "eye_r")
		check(Vector2(left.size).distance_to(Vector2(right.size)) < 0.02,
				"locking the eyes gives both the same size")
		var decal_right: Dictionary = controller.decal.get_region("eye_r")
		check(not decal_right.is_empty()
				and absf(float(decal_right.center.x) + float(left.center.x)) < 0.02,
				"the decal receives the mirrored eye placement")
		var right_screen: Variant = model_view.call("screen_from_canonical",
				Vector2(right.center))
		if right_screen != null:
			var draw_box := Rect2(Vector2(right_screen) - Vector2(18, 12), Vector2(36, 24))
			var drawn_pair: Dictionary = setup.call("draw_region_on_model", "eye_r", draw_box)
			check(bool(drawn_pair.get("ok", false)), "a locked eye draw applies")
			var drawn_l: Dictionary = Calibration.region(
					setup.call("get_calibration"), "eye_l")
			var drawn_r: Dictionary = Calibration.region(
					setup.call("get_calibration"), "eye_r")
			check(absf(float(drawn_l.center.x) + float(drawn_r.center.x)) < 0.02
					and Vector2(drawn_l.size).distance_to(Vector2(drawn_r.size)) < 0.02,
					"drawing one locked eye mirrors the partner")

	# Unlocking restores independent edits.
	setup.call("set_lock_eyes", false)
	var right_before: Dictionary = Calibration.region(setup.call("get_calibration"), "eye_r")
	var left_now: Dictionary = Calibration.region(setup.call("get_calibration"), "eye_l")
	var left_screen: Variant = model_view.call("screen_from_canonical",
			Vector2(left_now.center))
	if left_screen != null:
		setup.call("drag_region_on_model", "eye_l",
				Vector2(left_screen) + Vector2(-9, 5))
		var right_after: Dictionary = Calibration.region(
				setup.call("get_calibration"), "eye_r")
		check(Vector2(right_after.center).distance_to(Vector2(right_before.center)) < 0.005,
				"unlocking the eyes leaves the partner alone")
	setup.call("set_auto_fit", false)
	result["direct"]["auto_fit"] = bool(setup.call("get_auto_fit"))
	result["direct"]["lock_eyes"] = bool(setup.call("get_lock_eyes"))
	# The manual-edit path persists the toggle; do not leave the developer's
	# Setup window with the auto-fit switched off after a test run.
	_remove("user://setup_prefs.json")


func _fill_contour(points: Array, indices: Array, centre: Vector2, size: Vector2,
		h: PackedFloat32Array) -> void:
	var count := maxi(indices.size(), 3)
	for step in range(count):
		var angle := TAU * float(step) / float(count)
		var canonical := centre + Vector2(cos(angle) * size.x * 0.5,
				sin(angle) * size.y * 0.5)
		points[int(indices[step % indices.size()])] = _apply_homography(h, canonical)


func _apply_homography(h: PackedFloat32Array, point: Vector2) -> Vector2:
	var w := h[6] * point.x + h[7] * point.y + h[8]
	return Vector2((h[0] * point.x + h[1] * point.y + h[2]) / w,
			(h[3] * point.x + h[4] * point.y + h[5]) / w)


func _entry(entries: Array, id: String) -> Dictionary:
	for entry in entries:
		if entry is Dictionary and str(entry.get("id", "")) == id:
			return entry
	return {}


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
	if not rendering and result_tag != "":
		suffix = "-" + result_tag
	elif not rendering and source_kind == "still":
		suffix = "-still"
	var path := ProjectSettings.globalize_path("res://../.tools/T25c-setup%s.json" % suffix)
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(JSON.stringify(result, "  "))
	file.close()
	print("T25c ", JSON.stringify(result))
	quit(0 if failures.is_empty() else 1)
