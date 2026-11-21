extends SceneTree

# Windowed host-evidence capture for T17.
#
# Run with a real renderer (not --headless):
#   <godot> --path app --script res://tests/avatar_shot.gd
#
# The verify path is headless and cannot read the avatar viewport image, so this
# script is manual evidence that the imported avocado actually rasterises in
# bind pose with its materials. It writes the viewport PNG to
# AVATAR_SHOT_PATH (default user://T17-avatar.png) and fails if fewer than 1000
# pixels are opaque.

const MIN_OPAQUE_PIXELS := 1000


func _initialize() -> void:
	_run.call_deferred()


func _run() -> void:
	var packed := load("res://scenes/main.tscn")
	if packed == null:
		printerr("avatar_shot: main scene did not load")
		quit(1)
		return
	var scene := packed.instantiate() as Node
	root.add_child(scene)
	for _i in range(10):
		await process_frame
	await RenderingServer.frame_post_draw

	var viewport := scene.get_node_or_null("SubViewport") as SubViewport
	if viewport == null:
		printerr("avatar_shot: no SubViewport")
		quit(1)
		return
	var image := viewport.get_texture().get_image()
	if image == null or image.is_empty():
		printerr("avatar_shot: viewport image is empty")
		quit(1)
		return

	var opaque := 0
	var non_black := 0
	for y in range(image.get_height()):
		for x in range(image.get_width()):
			var color := image.get_pixel(x, y)
			if color.a > 0.5:
				opaque += 1
				if color.r + color.g + color.b > 0.02:
					non_black += 1

	var path := OS.get_environment("AVATAR_SHOT_PATH")
	if path.is_empty():
		path = "user://T17-avatar.png"
	image.save_png(path)
	print("avatar_shot: %dx%d, opaque=%d, non_black=%d, saved %s" % [
		image.get_width(), image.get_height(), opaque, non_black, path])
	quit(0 if opaque >= MIN_OPAQUE_PIXELS else 1)
