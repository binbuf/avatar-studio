extends SceneTree
# Independent Quaternion.slerp oracle plus rendered golden frames. No production math in oracle.
var failures: Array[String] = []
var result := {"schema": "avatar_head.result.v1", "poses": 0}
var rendering := false
var update_goldens := false
const FACE := {"origin": Vector3(0, 1.65, -.15), "normal": Vector3(0, 0, -1), "up": Vector3.UP, "scale": .3}
const CUT := {"mode": "cut", "cut": {"origin": Vector3(0, 1.05, 0), "normal": Vector3.UP, "falloff": .35}}
const POSES := [Vector3(0, 30, 0), Vector3(0, -30, 0), Vector3(20, 0, 0), Vector3(-20, 0, 0), Vector3(0, 0, 20), Vector3(0, 0, -20)]
var viewport: SubViewport
var stage: Node3D
var material: ShaderMaterial

func _initialize() -> void:
    rendering = "--render" in OS.get_cmdline_user_args()
    update_goldens = "--update-goldens" in OS.get_cmdline_user_args()
    _run.call_deferred()

func check(ok: bool, message: String) -> void:
    if not ok:
        failures.append(message)
        printerr("FAIL: ", message)

func new_model(name: String, submesh: String = "") -> Node3D:
    var m: Node3D = ClassDB.instantiate("AvatarModel")
    (stage if rendering else root).add_child(m)
    if not submesh.is_empty(): m.call("set_face_submesh", submesh)
    var path := ProjectSettings.globalize_path("res://../tests/fixtures/models/" + name + ".glb")
    check(m.call("import_glb", path) == OK, "import " + name)
    return m

func meshes(node: Node) -> Array[MeshInstance3D]:
    var out: Array[MeshInstance3D] = []
    for child in node.get_children():
        if child is MeshInstance3D and child.name != "HeadDecal": out.append(child)
        if child.name != "HeadDecal": out.append_array(meshes(child))
    return out

func weight(p: Vector3) -> float:
    var t := clampf((p.y - 1.05) / .35, 0, 1)
    return t * t * (3 - 2 * t)

func expected(p: Vector3, q: Quaternion, mode: String, is_head: bool) -> Vector3:
    var pivot := Vector3(0, 1.65, .075) if mode == "cut" else Vector3(0, 1.2, -.15)
    var w := weight(p) if mode == "cut" else (1.0 if is_head else 0.0)
    return pivot + Quaternion.IDENTITY.slerp(q, w) * (p - pivot)

func validate(m: Node3D, q: Quaternion, mode: String, verify_decal: bool = false) -> void:
    var maximum := 0.0
    var normal_error := 0.0
    var tangent_error := 0.0
    var body_error := 0.0
    var seams: Dictionary = {}
    var seam_error := 0.0
    var data: Array = m.call("get_head_surface_data")
    var instances := meshes(m)
    for s in range(data.size()):
        var item: Dictionary = data[s]
        var rest: PackedVector3Array = item.rest_local
        var vertices: PackedVector3Array = item.vertices
        var normals: PackedVector3Array = item.normals
        var rest_normals: PackedVector3Array = item.rest_normals
        var tangents: PackedFloat32Array = item.tangents
        var rest_tangents: PackedFloat32Array = item.rest_tangents
        var transform: Transform3D = item.rest_transform
        var current: Transform3D = item.transform
        var head: bool = item.node == "face"
        check(tangents.size() == rest.size() * 4, "fixture exercises tangent buffer encoding")
        for i in range(rest.size()):
            var p := transform * rest[i]
            var actual := current * vertices[i]
            var target := expected(p, q, mode, head)
            maximum = maxf(maximum, actual.distance_to(target))
            if p.y <= 1.05: body_error = maxf(body_error, actual.distance_to(p))
            var w := weight(p) if mode == "cut" else (1.0 if head else 0.0)
            var n := (transform.basis.inverse().transposed() * rest_normals[i]).normalized()
            var target_normal := Quaternion.IDENTITY.slerp(q, w) * n
            var actual_normal := (current.basis.inverse().transposed() * normals[i]).normalized()
            normal_error = maxf(normal_error, actual_normal.distance_to(target_normal))
            if tangents.size() == rest.size() * 4:
                var t := Vector3(rest_tangents[i*4], rest_tangents[i*4+1], rest_tangents[i*4+2])
                var target_t := Quaternion.IDENTITY.slerp(q, w) * (transform.basis * t).normalized()
                var current_t := (current.basis * Vector3(tangents[i*4], tangents[i*4+1], tangents[i*4+2])).normalized()
                tangent_error = maxf(tangent_error, current_t.distance_to(target_t))
                if tangents[i*4+3] != rest_tangents[i*4+3]: tangent_error = 1
            # Coincident seam vertices, including separate body/head node origins, must stay welded.
            var key := Vector3i(roundi(p.x * 100000), roundi(p.y * 100000), roundi(p.z * 100000))
            if seams.has(key): seam_error = maxf(seam_error, actual.distance_to(seams[key]))
            else: seams[key] = actual
        if verify_decal:
            var decal := instances[s].get_node_or_null("HeadDecal") as MeshInstance3D
            check(decal != null and decal.mesh == instances[s].mesh, "decal and model share live geometry")
            check(decal.transform == Transform3D.IDENTITY, "decal follows exact model transform")
            var attrs: Array = instances[s].mesh.surface_get_arrays(0)
            var custom: PackedFloat32Array = attrs[Mesh.ARRAY_CUSTOM0]
            for i in range(rest.size()):
                var p := transform * rest[i]
                if Vector3(custom[3*i], custom[3*i+1], custom[3*i+2]).distance_to(p) > 1e-6:
                    check(false, "CUSTOM0 must remain immutable rest model position")
                    break
    check(maximum < 2e-6, "%s position oracle %f" % [mode, maximum])
    check(normal_error < .0002, "%s rotated unit normals %f" % [mode, normal_error])
    check(tangent_error < .0003, "%s rotated tangent frames %f" % [mode, tangent_error])
    check(body_error == 0, "%s body stays bit-exact %f" % [mode, body_error])
    check(seam_error < 2e-6, "%s welded neck seam %f" % [mode, seam_error])
    # Smoothness at the cut endpoints, with no topological split.
    if mode == "cut":
        for y in [1.05, 1.4]:
            var a := expected(Vector3(.1, y - .00001, -.15), q, mode, true)
            var b := expected(Vector3(.1, y + .00001, -.15), q, mode, true)
            check(a.distance_to(b) < .0001, "continuous cut endpoints")

func reference(m: Node3D, q: Quaternion, mode: String) -> Node3D:
    var group := Node3D.new()
    stage.add_child(group)
    var instances := meshes(m)
    var data: Array = m.call("get_head_surface_data")
    for s in range(data.size()):
        var item: Dictionary = data[s]
        var arrays: Array = instances[s].mesh.surface_get_arrays(0).duplicate(true)
        var rest: PackedVector3Array = item.rest_local
        var normals: PackedVector3Array = item.rest_normals
        var transform: Transform3D = item.rest_transform
        var head: bool = item.node == "face"
        var out := PackedVector3Array()
        var ns := PackedVector3Array()
        for i in range(rest.size()):
            var p := transform * rest[i]
            out.append(expected(p, q, mode, head))
            var w := weight(p) if mode == "cut" else (1.0 if head else 0.0)
            ns.append(Quaternion.IDENTITY.slerp(q, w) * (transform.basis.inverse().transposed() * normals[i]).normalized())
        arrays[Mesh.ARRAY_VERTEX] = out
        arrays[Mesh.ARRAY_NORMAL] = ns
        var mesh := ArrayMesh.new()
        mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [], {},
            (Mesh.ARRAY_CUSTOM_RGB_FLOAT << Mesh.ARRAY_FORMAT_CUSTOM0_SHIFT) |
            (Mesh.ARRAY_CUSTOM_RGB_FLOAT << Mesh.ARRAY_FORMAT_CUSTOM1_SHIFT))
        mesh.surface_set_material(0, instances[s].mesh.surface_get_material(0))
        var instance := MeshInstance3D.new()
        instance.mesh = mesh
        group.add_child(instance)
        var decal := MeshInstance3D.new()
        decal.mesh = mesh
        decal.material_override = material
        instance.add_child(decal)
    return group

func capture() -> Image:
    await process_frame
    await RenderingServer.frame_post_draw
    return viewport.get_texture().get_image()

func difference(a: Image, b: Image) -> float:
    if a.get_size() != b.get_size(): return 1.0
    var total := 0.0
    for y in range(a.get_height()):
        for x in range(a.get_width()):
            var av := a.get_pixel(x,y)
            var bv := b.get_pixel(x,y)
            total += absf(av.r-bv.r) + absf(av.g-bv.g) + absf(av.b-bv.b)
    return total / (a.get_width() * a.get_height() * 3)

func golden(m: Node3D, q: Quaternion, mode: String, pose: int) -> void:
    var actual := await capture()
    m.hide()
    var oracle := reference(m, q, mode)
    var target := await capture()
    oracle.free()
    m.show()
    var error := difference(actual, target)
    check(error < .002, "%s rendered independent oracle %d: %f" % [mode, pose, error])
    var pink := 0
    for y in range(target.get_height()):
        for x in range(target.get_width()):
            var pixel := target.get_pixel(x,y)
            if pixel.r > .6 and pixel.b > .25 and pixel.g < .4: pink += 1
    check(pink > 20, "rest-space decal visible in golden")
    var path := ProjectSettings.globalize_path("res://../tests/golden/head-region/%s-%d.png" % [mode, pose])
    if update_goldens:
        DirAccess.make_dir_recursive_absolute(path.get_base_dir())
        check(target.save_png(path) == OK, "write independent reference golden")
    else:
        var stored := Image.load_from_file(path)
        check(stored != null and not stored.is_empty(), "golden exists " + path)
        if stored != null and not stored.is_empty():
            check(difference(actual, stored) < .002, "matches committed golden " + path)
    result.poses += 1

func setup_render() -> void:
    viewport = SubViewport.new()
    viewport.size = Vector2i(320, 320)
    viewport.own_world_3d = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    stage = Node3D.new()
    viewport.add_child(stage)
    var camera := Camera3D.new()
    camera.position = Vector3(0, 1.05, -4)
    camera.rotation.y = PI
    camera.projection = Camera3D.PROJECTION_ORTHOGONAL
    camera.size = 2.5
    stage.add_child(camera)
    var env := WorldEnvironment.new()
    var environment := Environment.new()
    environment.background_mode = Environment.BG_COLOR
    environment.background_color = Color(.03, .03, .03)
    env.environment = environment
    stage.add_child(env)
    material = ShaderMaterial.new()
    material.shader = load("res://tests/shaders/head_region_decal.gdshader")
    material.set_shader_parameter("face_origin", FACE.origin)
    material.set_shader_parameter("face_scale", FACE.scale)
    material.render_priority = 1

func test_validation() -> void:
    var m := new_model("head_cut")
    check(m.call("get_head_region").derived, "absent head derives cut")
    check("RIG-006" in str(m.call("get_warnings")), "derived region warns RIG-006")
    check(m.call("configure_head_region", CUT, FACE, null), "valid cut")
    check(m.call("get_head_region").pivot.distance_to(Vector3(0,1.65,.075)) < 1e-6, "derived face pivot")
    for bad in [0.0, -1.0, INF, NAN, "bad"]:
        var cut: Dictionary = CUT.duplicate(true)
        cut.cut.falloff = bad
        check(not m.call("configure_head_region", cut, FACE, null), "invalid falloff rejected")
        check(m.call("get_last_error_code") == "AVT-117", "AVT-117 diagnostic")
    var above: Dictionary = CUT.duplicate(true)
    above.cut.origin = Vector3(0,2,0)
    check(not m.call("configure_head_region", above, FACE, null), "plane above face rejected")
    above.cut.origin = FACE.origin
    check(not m.call("configure_head_region", above, FACE, null), "plane at face rejected")
    above.cut.normal = Vector3.ZERO
    check(not m.call("configure_head_region", above, FACE, null), "zero normal rejected")
    check(m.call("is_loaded"), "invalid region edit preserves imported model")
    check(m.call("get_head_region").mode == "cut", "invalid edit preserves prior region")
    check(m.call("configure_head_region", CUT, FACE, Vector3(0,1.2,0)), "explicit pivot")
    check(m.call("get_head_region").pivot == Vector3(0,1.2,0), "explicit pivot retained")
    check(not m.call("configure_head_region", CUT, FACE, Vector3(NAN,0,0)), "nonfinite pivot rejected")
    m.free()
    m = new_model("head_node")
    check(m.call("configure_head_region", {}, FACE, null), "unique head auto-selected")
    check(m.call("get_head_region").mode == "node", "automatic node mode")
    check(not m.call("configure_head_region", {"mode":"node", "node":"body"}, FACE, null), "unrelated node rejected")
    check(not m.call("configure_head_region", {"mode":"node", "node":"missing"}, FACE, null), "missing node rejected")
    check(m.call("configure_head_region", {"mode":"node", "node":"head"}, FACE, null), "valid head subtree")
    m.free()
    m = new_model("head_node", "body")
    check(not m.call("configure_head_region", {"mode":"node", "node":"head"}, FACE, null), "face_submesh outside selected subtree rejected")
    m.free()
    m = new_model("head_node", "face")
    check(m.call("configure_head_region", {"mode":"node", "node":"head"}, FACE, null), "face_submesh belongs to head descendant")
    m.free()

func benchmark() -> void:
    var m := new_model("head_cut")
    m.call("configure_head_region", CUT, FACE, null)
    check(m.call("get_triangle_count") == 20000, "20k-triangle budget fixture")
    var samples: Array[float] = []
    for i in range(330):
        var q := Quaternion.from_euler(Vector3(.2*sin(i*.1), .5*sin(i*.07), .2*cos(i*.1)))
        var start := Time.get_ticks_usec()
        m.call("set_head_rotation", q)
        var elapsed := (Time.get_ticks_usec() - start) / 1000.0
        if i >= 30: samples.append(elapsed)
        if rendering: await process_frame
    samples.sort()
    result["deformation_median_ms"] = samples[150]
    result["deformation_p95_ms"] = samples[285]
    result["triangles"] = 20000
    result["vertices"] = 10201
    print("T19 CPU+buffer submission median=", samples[150], " ms, p95=", samples[285], " ms")
    m.free()
    m = new_model("head_node")
    m.call("configure_head_region", {"mode":"node", "node":"head"}, FACE, null)
    check(m.call("get_triangle_count") == 20000, "20k-triangle node budget fixture")
    samples.clear()
    for i in range(330):
        var q := Quaternion.from_euler(Vector3(.2*sin(i*.1), .5*sin(i*.07), .2*cos(i*.1)))
        var start := Time.get_ticks_usec()
        m.call("set_head_rotation", q)
        var elapsed := (Time.get_ticks_usec() - start) / 1000.0
        if i >= 30: samples.append(elapsed)
        if rendering: await process_frame
    samples.sort()
    result["node_median_ms"] = samples[150]
    result["node_p95_ms"] = samples[285]
    m.free()

func _run() -> void:
    if rendering: setup_render()
    test_validation()
    await benchmark()
    # Nonuniform scale and nested authored transforms exercise normal-space conversion.
    var transformed := new_model("head_transformed")
    check(transformed.call("configure_head_region", CUT, FACE, null), "transformed cut configure")
    var tq := Quaternion.from_euler(Vector3(.2, .4, -.2))
    transformed.call("set_head_rotation", tq)
    validate(transformed, tq, "cut")
    transformed.free()
    for mode in ["cut", "node"]:
        var m := new_model("head_" + mode)
        var region: Dictionary = CUT if mode == "cut" else {"mode":"node", "node":"head"}
        check(m.call("configure_head_region", region, FACE, null), mode + " configure")
        if rendering:
            for instance in meshes(m):
                var base := instance.mesh.surface_get_material(0) as BaseMaterial3D
                base.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
            check(m.call("create_decal_pass", material) != null, "create shared decal pass")
            check(m.call("create_decal_pass", material) != null, "replace shared decal pass")
            for instance in meshes(m):
                check(instance.get_child_count() == 1, "replacing pass does not duplicate instances")
        for pose in range(POSES.size()):
            var q := Quaternion.from_euler(POSES[pose] * PI / 180)
            m.call("set_head_rotation", q)
            validate(m, q, mode, rendering)
            if rendering: await golden(m, q, mode, pose)
        # Switching modes must restore prior pose and refuse invalid updates atomically.
        if mode == "node":
            check(m.call("configure_head_region", CUT, FACE, null), "node to cut resets subtree")
            m.call("set_head_rotation", Quaternion.IDENTITY)
            validate(m, Quaternion.IDENTITY, "cut")
        m.call("clear_decal_pass")
        for instance in meshes(m): check(not instance.has_node("HeadDecal"), "clear pass removes owned instances")
        m.free()
    result["failures"] = failures
    result["passed"] = failures.is_empty()
    result["rendered"] = rendering
    var path := ProjectSettings.globalize_path("res://../.tools/T19-head-region%s.json" % ("-render" if rendering else ""))
    var file := FileAccess.open(path, FileAccess.WRITE)
    file.store_string(JSON.stringify(result, "  "))
    print("T19 ", JSON.stringify(result))
    quit(0 if failures.is_empty() else 1)
