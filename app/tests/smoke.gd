extends SceneTree

# Headless shell smoke test for T02.
#
# Run with:
#   <godot> --headless --path app --script res://tests/smoke.gd
#
# Godot 4.7.2 only scans res://*.gdextension into .godot/extension_list.cfg during
# an editor import; scripts/verify.* seeds that one-line file first. A plain
# --script run on a tree without it will fail the AvatarRuntime check.
#
# It asserts the project is Forward+, the main scene owns a 1920x1080
# transparent SubViewport with UPDATE_ALWAYS, the pinned GDExtension loaded and
# registered AvatarRuntime, and the C++ frame counter advances. Any failed check
# exits non-zero so scripts/verify.* can gate on it.

var failures: Array[String] = []


func _initialize() -> void:
	_run.call_deferred()


func _check(condition: bool, message: String) -> void:
	if condition:
		print("  ok   ", message)
	else:
		failures.append(message)
		printerr("  FAIL ", message)


func _run() -> void:
	print("smoke: Avatar Studio shell")

	_check(
		ProjectSettings.get_setting("rendering/renderer/rendering_method") == "forward_plus",
		"renderer is Forward+"
	)
	_check(
		int(ProjectSettings.get_setting("application/run/max_fps")) == 60,
		"Engine.max_fps is pinned to 60"
	)
	_check(
		ProjectSettings.get_setting("application/run/main_scene") == "res://scenes/main.tscn",
		"main scene is res://scenes/main.tscn"
	)

	var extension_loaded := ClassDB.class_exists("AvatarRuntime")
	_check(extension_loaded, "GDExtension registers AvatarRuntime")
	if extension_loaded:
		var avatar := ClassDB.instantiate("AvatarRuntime") as Node
		_check(avatar != null, "AvatarRuntime instantiates")
		if avatar != null:
			_check(avatar.has_method("get_version"), "AvatarRuntime exposes get_version()")
			var version: Variant = avatar.call("get_version")
			_check(
				typeof(version) == TYPE_STRING and not String(version).is_empty(),
				"get_version() returns a non-empty string (%s)" % String(version)
			)
			root.add_child(avatar)
			avatar.call("reset_frame_count")
			for _i in range(3):
				await process_frame
			var frames := int(avatar.call("get_frame_count"))
			_check(frames > 0, "C++ frame counter advances (got %d)" % frames)
			avatar.queue_free()
			await process_frame

	var packed := load("res://scenes/main.tscn")
	_check(packed != null, "main scene loads")
	if packed != null:
		var scene := packed.instantiate() as Node
		_check(scene != null, "main scene instantiates")
		if scene != null:
			root.add_child(scene)
			await process_frame
			var viewport := scene.get_node_or_null("SubViewport") as SubViewport
			_check(viewport != null, "main scene has a SubViewport")
			if viewport != null:
				_check(viewport.size == Vector2i(1920, 1080), "SubViewport is 1920x1080")
				_check(viewport.transparent_bg, "SubViewport has a transparent background")
				_check(
					viewport.render_target_update_mode == SubViewport.UPDATE_ALWAYS,
					"SubViewport render_target_update_mode is ALWAYS"
				)
			scene.queue_free()
			await process_frame

	if failures.is_empty():
		print("smoke: PASS")
		quit(0)
	else:
		printerr("smoke: FAIL (%d failed checks)" % failures.size())
		quit(1)
