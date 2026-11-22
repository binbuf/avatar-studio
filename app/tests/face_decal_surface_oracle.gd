extends RefCounted
## CPU triangle rasterizer, independent of the production vertex buffers/shader.
## Orthographic test camera; depth, rest attributes and posed normals are
## barycentrically interpolated. Deformation uses Quaternion.slerp, not native math.
const Fixtures = preload("res://tests/face_decal_flat_fixtures.gd")

static func alpha_at(p: Vector3, n: Vector3, posed: Vector3, posed_n: Vector3,
		camera: Camera3D, face: Dictionary, regions: Array, homography: Basis,
		image: Image) -> Array:
	if n.dot(face.normal) <= 0.0:
		return [Color(0, 0, 0, 0), -1]
	var canonical := Vector2((p - face.origin).dot(face.right), (p - face.origin).dot(face.up)) / float(face.scale)
	var h := homography * Vector3(canonical.x, canonical.y, 1)
	if h.z <= 0:
		return [Color(0, 0, 0, 0), -1]
	var uv := Vector2(h.x, h.y) / h.z
	if uv.x < 0 or uv.x > 1 or uv.y < 0 or uv.y > 1:
		return [Color(0, 0, 0, 0), -1]
	var facing := smoothstep(.10, .35, posed_n.normalized().dot((camera.global_position - posed).normalized()))
	var rgb := Fixtures.srgb_to_linear_rgb(Fixtures.sample_bilinear(image, uv))
	var out := Color(0, 0, 0, 0)
	var region_index := -1
	for i in range(regions.size()):
		var r: Dictionary = regions[i]
		var d: Vector2 = (canonical - r.center - r.anchor) / (r.size * .5)
		d = d.rotated(-r.rotation)
		var sdf := d.length() - 1.0
		if r.shape == 1:
			var q := d.abs() - Vector2.ONE * (1.0 - float(r.corner_radius))
			sdf = q.max(Vector2.ZERO).length() + minf(maxf(q.x, q.y), 0) - float(r.corner_radius)
		var mask := 1.0 - smoothstep(-maxf(r.feather, .00001), 0, sdf)
		var a: float = mask * facing * facing * r.opacity
		var colour: Vector3 = rgb * r.gain + r.lift
		out = Color(colour.x * a, colour.y * a, colour.z * a, a) + out * (1.0 - a)
		if a > .1:
			region_index = i
	return [out, region_index]

static func render(model: Node3D, camera: Camera3D, base: Image, face: Dictionary,
		regions: Array, homography: Basis, image: Image, rotation: Quaternion) -> Dictionary:
	var size := base.get_size()
	var depth := PackedFloat32Array()
	depth.resize(size.x * size.y)
	depth.fill(INF)
	var eye_depth := PackedFloat32Array()
	eye_depth.resize(size.x * size.y)
	eye_depth.fill(INF)
	var out := base.duplicate() as Image
	var region: Dictionary = model.call("get_head_region")
	var data: Array = model.call("get_head_surface_data")
	var surfaces: Array[MeshInstance3D] = []
	collect(model, surfaces)
	var surface_arrays: Array[Array] = []
	var surface_instances: Array[MeshInstance3D] = []
	for instance in surfaces:
		for surface in range(instance.mesh.get_surface_count()):
			surface_arrays.append(instance.mesh.surface_get_arrays(surface))
			surface_instances.append(instance)
	var camera_inverse := camera.global_transform.affine_inverse()
	var srgb_output := RenderingServer.get_current_rendering_method() != "gl_compatibility"
	for s in range(data.size()):
		var item: Dictionary = data[s]
		var arrays: Array = surface_arrays[s]
		var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
		var rest: PackedVector3Array = item.rest_local
		var normals: PackedVector3Array = item.rest_normals
		var transform: Transform3D = item.rest_transform
		var rest_points := PackedVector3Array()
		var rest_normals := PackedVector3Array()
		var posed := PackedVector3Array()
		var posed_normals := PackedVector3Array()
		var screen := PackedVector2Array()
		var zs := PackedFloat32Array()
		var in_head := false
		if region.mode == "node":
			var ancestor: Node = surface_instances[s]
			while ancestor != model:
				if str(ancestor.name) == str(region.node):
					in_head = true
				ancestor = ancestor.get_parent()
		for i in range(rest.size()):
			var p := transform * rest[i]
			var n := (transform.basis.inverse().transposed() * normals[i]).normalized()
			var weight := 1.0 if in_head else 0.0
			if region.mode == "cut":
				weight = smoothstep(0, float(region.cut.falloff), (p - region.cut.origin).dot(region.cut.normal))
			var q := Quaternion.IDENTITY.slerp(rotation, weight)
			var position: Vector3 = model.global_transform * (region.pivot + q * (p - region.pivot))
			rest_points.append(p)
			rest_normals.append(n)
			posed.append(position)
			posed_normals.append((model.global_basis.inverse().transposed() * (q * n)).normalized())
			screen.append(camera.unproject_position(position))
			zs.append(-(camera_inverse * position).z)
		if indices.is_empty():
			for i in range(rest.size()):
				indices.append(i)
		for tri in range(0, indices.size(), 3):
			var a := indices[tri]
			var b := indices[tri + 1]
			var c := indices[tri + 2]
			var ab := screen[b] - screen[a]
			var ac := screen[c] - screen[a]
			var determinant := ab.cross(ac)
			if absf(determinant) < .00001:
				continue
			var lo := screen[a].min(screen[b]).min(screen[c]).floor().max(Vector2.ZERO)
			var hi := screen[a].max(screen[b]).max(screen[c]).ceil().min(Vector2(size - Vector2i.ONE))
			for y in range(int(lo.y), int(hi.y) + 1):
				for x in range(int(lo.x), int(hi.x) + 1):
					var ap := Vector2(x + .5, y + .5) - screen[a]
					var v := ap.cross(ac) / determinant
					var w := ab.cross(ap) / determinant
					var u := 1.0 - v - w
					if minf(u, minf(v, w)) < -.00001:
						continue
					var index := y * size.x + x
					var z := zs[a] * u + zs[b] * v + zs[c] * w
					var sample_result := alpha_at(rest_points[a]*u + rest_points[b]*v + rest_points[c]*w,
						rest_normals[a]*u + rest_normals[b]*v + rest_normals[c]*w,
						posed[a]*u + posed[b]*v + posed[c]*w,
						posed_normals[a]*u + posed_normals[b]*v + posed_normals[c]*w,
						camera, face, regions, homography, image) if item.decal else [Color(0,0,0,0), -1]
					if sample_result[1] in [0, 1]:
						eye_depth[index] = minf(eye_depth[index], z)
					if z > depth[index] + .000001:
						continue
					depth[index] = z
					var colour: Color = sample_result[0]
					var bg := base.get_pixel(x, y)
					if colour.a <= 0:
						out.set_pixel(x, y, bg)
						continue
					if srgb_output:
						bg = bg.srgb_to_linear()
					var composite := Color(colour.r + bg.r*(1-colour.a),
						colour.g + bg.g*(1-colour.a), colour.b + bg.b*(1-colour.a), 1)
					out.set_pixel(x, y, composite.linear_to_srgb() if srgb_output else composite)
	var occluded_eye_pixels := 0
	for i in range(depth.size()):
		if is_finite(eye_depth[i]) and eye_depth[i] > depth[i] + .001:
			occluded_eye_pixels += 1
	return {"image": out, "occluded_eye_pixels": occluded_eye_pixels}

static func collect(node: Node, out: Array[MeshInstance3D]) -> void:
	for child in node.get_children():
		if child.name == "HeadDecal":
			continue
		if child is MeshInstance3D:
			out.append(child)
		collect(child, out)
