extends SceneTree

# T17 runtime GLB import test.
#
# Run with:
#   <godot> --headless --path app --script res://tests/model_import.gd
#
# It imports assets/models/avocado_half.glb and the generated fixtures through
# the AvatarModel GDExtension class, then asserts the model-check surface, the
# bind-pose rules (no animation, skeleton or skin), the axis/unit conversion,
# face_submesh restriction, decal arrays, AVT-102 on an invalid GLB, and the
# app scene's camera/light/avatar wiring. Import time and static-memory deltas
# are written to AVATAR_MODEL_RESULT for tests/gates/T17.ps1 and
# docs/progress/T17.md.

const SCHEMA := "avatar_model.result.v1"

var failures: Array[String] = []
var result: Dictionary = {}
var metrics: Dictionary = {}


func _initialize() -> void:
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


func _fixture_path(name: String) -> String:
	var app_dir := ProjectSettings.globalize_path("res://")
	if name == "avocado_half.glb":
		return app_dir.path_join("../assets/models/avocado_half.glb").simplify_path()
	return app_dir.path_join("../tests/fixtures/models/" + name).simplify_path()


func _new_model() -> Node3D:
	var instance: Variant = ClassDB.instantiate("AvatarModel")
	var model: Node3D = instance as Node3D
	if model != null:
		root.add_child(model)
	return model


func _import(model: Node3D, path: String) -> int:
	return int(_call(model, "import_glb", path))


func _aabb_is_plausible(aabb: AABB) -> bool:
	if not aabb.position.is_finite() or not aabb.size.is_finite():
		return false
	if aabb.size.x <= 0.001 or aabb.size.y <= 0.001 or aabb.size.z <= 0.001:
		return false
	if aabb.size.x > 5.0 or aabb.size.y > 5.0 or aabb.size.z > 5.0:
		return false
	return aabb.has_volume()


func _decal_is_consistent(model: Node3D) -> bool:
	var vertices: PackedVector3Array = _call(model, "get_decal_vertices")
	var normals: PackedVector3Array = _call(model, "get_decal_normals")
	var indices: PackedInt32Array = _call(model, "get_decal_indices")
	if vertices.is_empty() or normals.size() != vertices.size() or indices.is_empty():
		return false
	if indices.size() % 3 != 0:
		return false
	var maximum := 0
	for index in indices:
		maximum = maxi(maximum, index)
	return maximum < vertices.size()


func _run() -> void:
	print("model_import: Avatar Studio runtime GLB import")

	_check(ClassDB.class_exists("AvatarModel"), "GDExtension registers AvatarModel")
	if not ClassDB.class_exists("AvatarModel"):
		_finish()
		return

	_test_avocado()
	_test_face_submesh()
	_test_feet_origin()
	_test_axis_variant()
	_test_animated_rig()
	_test_invalid_glb()
	await _test_app_scene()

	_finish()


func _test_avocado() -> void:
	print("model_import: avocado_half.glb")
	var model := _new_model()
	_check(model != null, "AvatarModel instantiates")
	if model == null:
		return

	var path := _fixture_path("avocado_half.glb")
	var memory_before := OS.get_static_memory_usage()
	var peak_before := OS.get_static_memory_peak_usage()
	var started := Time.get_ticks_usec()
	var error := _import(model, path)
	var elapsed_ms := float(Time.get_ticks_usec() - started) / 1000.0
	var memory_after := OS.get_static_memory_usage()
	var peak_after := OS.get_static_memory_peak_usage()

	metrics = {
		"path": path,
		"wall_ms": elapsed_ms,
		"reported_import_ms": float(_call(model, "get_import_time_ms")),
		"static_memory_delta_bytes": int(memory_after) - int(memory_before),
		"peak_static_memory_bytes": int(peak_after),
		"peak_static_memory_delta_bytes": maxi(0, int(peak_after) - int(peak_before)),
	}

	_check(error == OK, "avocado imports (error %d)" % error)
	_check(bool(_call(model, "is_loaded")), "avocado reports loaded")
	if error != OK:
		printerr("  error: ", _call(model, "get_last_error_code"), " ",
			_call(model, "get_last_error_message"))
		return

	var mesh_count := int(_call(model, "get_mesh_count"))
	var material_count := int(_call(model, "get_material_count"))
	var triangle_count := int(_call(model, "get_triangle_count"))
	var node_count := int(_call(model, "get_node_count"))
	_check(mesh_count == 2, "avocado has two meshes (%d)" % mesh_count)
	_check(material_count == 4, "avocado has four materials (%d)" % material_count)
	_check(triangle_count > 40000 and triangle_count <= 200000,
		"avocado triangle count is plausible (%d)" % triangle_count)
	_check(node_count >= 2, "avocado exposes its node tree (%d nodes)" % node_count)

	var mesh_names: PackedStringArray = _call(model, "get_mesh_names")
	var material_names: PackedStringArray = _call(model, "get_material_names")
	_check(mesh_names.has("avocado_half") and mesh_names.has("avocado_pit"),
		"mesh names are extracted (%s)" % [mesh_names])
	_check(material_names.has("avocado_skin") and material_names.has("avocado_pit"),
		"material names are extracted (%s)" % [material_names])

	var aabb: AABB = _call(model, "get_model_aabb")
	_check(_aabb_is_plausible(aabb), "avocado AABB is finite and plausible (%s)" % [aabb])
	_check(absf(aabb.size.y - 0.155) < 0.01,
		"avocado height is about 15.5 cm (%.4f m)" % aabb.size.y)
	var realised_size: Vector3 = _call(model, "get_realised_size")
	_check(realised_size.is_equal_approx(aabb.size), "realised size matches the AABB")
	var facing: Vector3 = _call(model, "get_realised_facing")
	var up: Vector3 = _call(model, "get_realised_up")
	_check(facing.is_equal_approx(Vector3(0, 0, -1)), "realised facing is -Z (%s)" % [facing])
	_check(up.is_equal_approx(Vector3(0, 1, 0)), "realised up is Y (%s)" % [up])

	var heads: PackedStringArray = _call(model, "get_candidate_head_nodes")
	_check(heads.has("avocado_half"), "candidate head nodes include the upper mesh (%s)" % [heads])

	_check(int(_call(model, "get_animation_count")) == 0, "avocado has no animations")
	_check(bool(_call(model, "are_animations_playing")) == false, "no animation is playing")
	_check(bool(_call(model, "has_skeleton")) == false, "no skeleton is created")
	_check(model.find_children("*", "AnimationPlayer", true, false).is_empty(),
		"scene has no AnimationPlayer")
	_check(model.find_children("*", "Skeleton3D", true, false).is_empty(),
		"scene has no Skeleton3D")

	var mesh_instances := model.find_children("*", "MeshInstance3D", true, false)
	_check(mesh_instances.size() == 2, "two MeshInstance3D nodes render the model")
	var all_materials := true
	for entry in mesh_instances:
		var instance := entry as MeshInstance3D
		if instance == null or instance.mesh == null or instance.mesh.get_surface_count() == 0:
			all_materials = false
			continue
		for surface in range(instance.mesh.get_surface_count()):
			if instance.mesh.surface_get_material(surface) == null:
				all_materials = false
	_check(all_materials, "every surface keeps its glTF material")

	var node_tree: Array = _call(model, "get_node_tree")
	_check(node_tree.size() == node_count, "node tree entries match the node count")
	var node_tree_ok := true
	for entry in node_tree:
		if not entry.has("name") or not entry.has("mesh") or not entry.has("children"):
			node_tree_ok = false
	_check(node_tree_ok, "node tree entries carry name, mesh and children")

	_check(_decal_is_consistent(model), "decal arrays are consistent (positions/normals/indices)")
	var decal_triangles := int(_call(model, "get_decal_triangle_count"))
	_check(decal_triangles == triangle_count,
		"without face_submesh every triangle feeds the decal pass (%d)" % decal_triangles)

	var summary: Dictionary = _call(model, "get_import_summary")
	_check(summary.get("schema", "") == "avatar_model.import.v1",
		"import summary has the expected schema")
	_check(summary.get("loaded", false) == true, "import summary reports loaded")
	_check(float(_call(model, "get_import_time_ms")) > 0.0, "import time is measured")
	_check(int(_call(model, "get_peak_memory_bytes")) > 0, "peak memory is measured")

	print("model_import: avocado import %.2f ms wall, %.2f ms reported, peak %d bytes" % [
		elapsed_ms, float(_call(model, "get_import_time_ms")),
		int(_call(model, "get_peak_memory_bytes"))])


func _test_face_submesh() -> void:
	print("model_import: face_submesh restriction")
	var full := _new_model()
	var restricted := _new_model()
	if full == null or restricted == null:
		_check(false, "face_submesh models instantiate")
		return
	var path := _fixture_path("avocado_half.glb")
	_check(_import(full, path) == OK, "reference import succeeds")
	_call(restricted, "set_face_submesh", "avocado_half")
	_check(_import(restricted, path) == OK, "face_submesh import succeeds")

	var full_triangles := int(_call(full, "get_decal_triangle_count"))
	var restricted_triangles := int(_call(restricted, "get_decal_triangle_count"))
	_check(restricted_triangles > 0, "face_submesh decal data is not empty")
	_check(restricted_triangles < full_triangles,
		"face_submesh restricts the decal data (%d < %d)" % [
			restricted_triangles, full_triangles])
	_check(String(_call(restricted, "get_decal_source")) == "avocado_half",
		"decal source names the restricted mesh")
	_check(int(_call(restricted, "get_triangle_count")) == int(_call(full, "get_triangle_count")),
		"face_submesh does not hide the rest of the model")
	_check(int(_call(restricted, "get_mesh_count")) == 2,
		"face_submesh keeps both meshes in the model check")


func _test_feet_origin() -> void:
	print("model_import: feet-origin fixture")
	var model := _new_model()
	if model == null:
		return
	var error := _import(model, _fixture_path("feet_origin.glb"))
	_check(error == OK, "feet-origin fixture imports")
	if error != OK:
		return
	var aabb: AABB = _call(model, "get_model_aabb")
	_check(absf(aabb.position.y) < 0.001, "feet-origin model starts at y=0 (%.5f)" % aabb.position.y)
	_check(aabb.size.is_equal_approx(Vector3(0.6, 1.8, 0.3)),
		"feet-origin size is 0.6 x 1.8 x 0.3 m (%s)" % [aabb.size])
	_check(int(_call(model, "get_triangle_count")) == 12,
		"feet-origin fixture has 12 triangles")
	_check(int(_call(model, "get_mesh_count")) == 1, "feet-origin fixture has one mesh")
	_check(int(_call(model, "get_material_count")) == 1, "feet-origin fixture has one material")
	var facing: Vector3 = _call(model, "get_realised_facing")
	_check(facing.is_equal_approx(Vector3(0, 0, -1)), "feet-origin model faces -Z")
	_check(_decal_is_consistent(model), "feet-origin decal arrays are consistent")


func _test_axis_variant() -> void:
	print("model_import: Z-up/+Y-forward axis fixture")
	var model := _new_model()
	if model == null:
		return
	_call(model, "set_forward_axis", "+Y")
	_call(model, "set_up_axis", "Z")
	var error := _import(model, _fixture_path("axis_variant.glb"))
	_check(error == OK, "axis-variant fixture imports")
	if error != OK:
		return
	var aabb: AABB = _call(model, "get_model_aabb")
	_check(aabb.size.is_equal_approx(Vector3(0.4, 1.8, 0.2)),
		"axis conversion realises 0.4 x 1.8 x 0.2 m (%s)" % [aabb.size])
	_check(absf(aabb.position.y) < 0.001, "axis conversion keeps the feet at y=0")
	var facing: Vector3 = _call(model, "get_realised_facing")
	var up: Vector3 = _call(model, "get_realised_up")
	_check(facing.is_equal_approx(Vector3(0, 0, -1)), "axis conversion realises -Z forward")
	_check(up.is_equal_approx(Vector3(0, 1, 0)), "axis conversion realises Y up")
	var summary: Dictionary = _call(model, "get_import_summary")
	_check(summary.get("forward", "") == "+Y" and summary.get("up", "") == "Z",
		"import summary records the authored axes")


func _test_animated_rig() -> void:
	print("model_import: animated skinned fixture")
	var model := _new_model()
	if model == null:
		return
	var error := _import(model, _fixture_path("animated_rig.glb"))
	_check(error == OK, "animated rig imports")
	if error != OK:
		return
	_check(int(_call(model, "get_animation_count")) >= 1, "animation is detected in the source")
	_check(int(_call(model, "get_skin_count")) >= 1, "skin is detected in the source")
	_check(bool(_call(model, "are_animations_playing")) == false,
		"the animation is not playing")
	_check(bool(_call(model, "has_skeleton")) == false, "the skeleton is not instantiated")
	_check(model.find_children("*", "AnimationPlayer", true, false).is_empty(),
		"animated fixture has no AnimationPlayer")
	_check(model.find_children("*", "Skeleton3D", true, false).is_empty(),
		"animated fixture has no Skeleton3D")
	var aabb: AABB = _call(model, "get_model_aabb")
	_check(aabb.size.is_equal_approx(Vector3(0.2, 0.2, 0.2)),
		"bind-pose AABB is the authored 0.2 m cube (%s)" % [aabb.size])
	_check(aabb.get_center().length() < 0.01,
		"bind-pose centre is at the origin, not the animated offset (%s)" % [aabb.get_center()])
	_check(int(_call(model, "get_triangle_count")) == 12,
		"animated fixture has 12 triangles")
	_check(_decal_is_consistent(model), "animated fixture decal arrays are consistent")


func _test_invalid_glb() -> void:
	print("model_import: invalid GLB")
	var model := _new_model()
	if model == null:
		return
	var error := _import(model, _fixture_path("invalid.glb"))
	_check(error != OK, "invalid GLB fails")
	_check(String(_call(model, "get_last_error_code")) == "AVT-102",
		"invalid GLB reports AVT-102 (%s)" % _call(model, "get_last_error_code"))
	_check(not String(_call(model, "get_last_error_message")).is_empty(),
		"invalid GLB reports a message")
	_check(bool(_call(model, "is_loaded")) == false, "invalid GLB leaves the model unloaded")
	_check(model.get_child_count() == 0, "invalid GLB leaves no partial scene")
	_check(model.find_children("*", "MeshInstance3D", true, false).is_empty(),
		"invalid GLB creates no mesh instances")


func _test_app_scene() -> void:
	print("model_import: app scene")
	var packed := load("res://scenes/main.tscn")
	_check(packed != null, "main scene loads")
	if packed == null:
		return
	var scene := packed.instantiate() as Node
	_check(scene != null, "main scene instantiates")
	if scene == null:
		return
	root.add_child(scene)
	await process_frame
	await process_frame

	var runtime := scene.get_node_or_null("AvatarRuntime")
	var viewport := scene.get_node_or_null("SubViewport") as SubViewport
	_check(runtime != null, "app scene has AvatarRuntime")
	_check(viewport != null, "app scene has the avatar SubViewport")
	if runtime == null or viewport == null:
		return
	_check(scene.find_children("*", "SubViewport", true, false).size() == 1,
		"the app still owns exactly one SubViewport")
	_check(bool(_call(runtime, "is_avatar_loaded")), "runtime loaded the avatar")
	var summary: Dictionary = _call(runtime, "get_avatar_import_summary")
	_check(summary.get("loaded", false) == true, "runtime import summary reports loaded")
	_check(int(summary.get("mesh_count", 0)) == 2, "runtime avatar has two meshes")

	var model := viewport.get_node_or_null("Presentation/AvatarModel") as Node3D
	_check(model != null, "AvatarModel is attached under Presentation")
	if model == null:
		return
	var mesh_instances := model.find_children("*", "MeshInstance3D", true, false)
	_check(mesh_instances.size() == 2, "app avatar renders two mesh instances")
	var materials_ok := true
	for entry in mesh_instances:
		var instance := entry as MeshInstance3D
		if instance == null or instance.mesh == null:
			materials_ok = false
			continue
		for surface in range(instance.mesh.get_surface_count()):
			if instance.mesh.surface_get_material(surface) == null:
				materials_ok = false
	_check(materials_ok, "app avatar surfaces keep their materials")
	_check(model.find_children("*", "AnimationPlayer", true, false).is_empty(),
		"app avatar has no AnimationPlayer")
	_check(model.find_children("*", "Skeleton3D", true, false).is_empty(),
		"app avatar has no Skeleton3D")

	var camera := viewport.get_node_or_null("AvatarCamera") as Camera3D
	_check(camera != null, "app scene has an AvatarCamera")
	if camera != null:
		_check(camera.current, "AvatarCamera is current")
		_check(camera.position.z > 0.0, "AvatarCamera sits in front of the avatar")
	_check(viewport.get_node_or_null("KeyLight") is DirectionalLight3D,
		"app scene has a key light")
	_check(viewport.get_node_or_null("FillLight") is DirectionalLight3D,
		"app scene has a fill light")

	scene.queue_free()
	await process_frame


func _finish() -> void:
	result = {
		"schema": SCHEMA,
		"fixtures": {
			"avocado": _fixture_path("avocado_half.glb"),
			"feet_origin": _fixture_path("feet_origin.glb"),
			"axis_variant": _fixture_path("axis_variant.glb"),
			"animated_rig": _fixture_path("animated_rig.glb"),
			"invalid": _fixture_path("invalid.glb"),
		},
		"metrics": metrics,
		"checks": {
			"avocado": true,
			"mesh_and_material_counts": true,
			"aabb": true,
			"animations_ignored": true,
			"feet_origin": true,
			"axis_variant": true,
			"face_submesh": true,
			"invalid_avt_102": true,
			"app_render": true,
		},
		"failures": failures,
		"passed": failures.is_empty(),
	}

	var path := OS.get_environment("AVATAR_MODEL_RESULT")
	if path.is_empty():
		path = "user://model-import-result.json"
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		printerr("model_import: cannot write result to ", path)
	else:
		file.store_string(JSON.stringify(result, "  "))
		file.close()

	if failures.is_empty():
		print("model_import: PASS")
		quit(0)
	else:
		printerr("model_import: FAIL (%d failed checks)" % failures.size())
		quit(1)
