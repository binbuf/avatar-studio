extends RefCounted

## Persisted live face calibration for the diagnostic Setup screen (T25c).
##
## This is the operator's manual mapping, not the package's face calibration
## (T29/T34). It is deliberately a small versioned JSON file in `user://` so a
## restart keeps the eye/mouth placement without touching the `.avatar`
## package. The T34 gizmo replaces the editor that writes it.
##
## Shape (version 1):
##   face:   {origin: [x, y, z], normal: [x, y, z], up: [x, y, z], scale: float}
##   regions: [{id, center: [u, v], size: [u, v], feather: float}, ...]
##   head:   {mode: "cut", cut: {origin: [x, y, z], normal: [x, y, z], falloff: float}}
##
## The decoder rejects unknown versions and malformed values instead of
## silently applying a half-calibration. The caller falls back to the
## provisional defaults from `face_calibration_defaults.gd`.

const Defaults = preload("res://scenes/face_calibration_defaults.gd")

const VERSION := 1
const PATH := "user://live_calibration.json"
const REGION_IDS := ["eye_l", "eye_r", "mouth"]


static func defaults(aabb: AABB) -> Dictionary:
	var face: Dictionary = Defaults.make_face(aabb)
	return {
		"version": VERSION,
		"face": face.duplicate(true),
		"regions": Defaults.make_regions(),
		"head": Defaults.make_head_region(face),
	}


## Returns a complete, validated calibration. Missing keys fall back to the
## defaults derived from `aabb`; malformed present keys reject the file.
static func normalise(raw: Dictionary, aabb: AABB) -> Dictionary:
	var base := defaults(aabb)
	if raw.is_empty():
		return base
	if int(raw.get("version", VERSION)) != VERSION:
		return {}
	var face := _read_face(raw.get("face", {}), base.face)
	if face.is_empty():
		return {}
	var regions := _read_regions(raw.get("regions", []), base.regions)
	if regions.is_empty():
		return {}
	var head := _read_head(raw.get("head", {}), base.head, face)
	if head.is_empty():
		return {}
	return {
		"version": VERSION,
		"face": face,
		"regions": regions,
		"head": head,
	}


static func encode(values: Dictionary) -> Dictionary:
	return {
		"version": VERSION,
		"face": {
			"origin": _array3(values.face.origin),
			"normal": _array3(values.face.normal),
			"up": _array3(values.face.up),
			"scale": float(values.face.scale),
		},
		"regions": _encode_regions(values.regions),
		"head": {
			"mode": "cut",
			"cut": {
				"origin": _array3(values.head.cut.origin),
				"normal": _array3(values.head.cut.normal),
				"falloff": float(values.head.cut.falloff),
			},
		},
	}


## Saves `values` as version 1 JSON. Returns {ok, path, error}.
static func save(values: Dictionary, path := PATH) -> Dictionary:
	var directory := path.get_base_dir()
	if directory != "":
		DirAccess.make_dir_recursive_absolute(directory)
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return {"ok": false, "path": path, "error": "cannot write " + path}
	file.store_string(JSON.stringify(encode(values), "  ", false))
	file.close()
	return {"ok": true, "path": path, "error": ""}


## Loads and decodes a saved calibration. Returns
## {ok, path, values, error, missing}. `values` is the decoded dictionary in
## the same shape as [method encode], ready for [method normalise].
static func load_file(path := PATH) -> Dictionary:
	if not FileAccess.file_exists(path):
		return {"ok": false, "path": path, "values": {}, "error": "not found", "missing": true}
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return {"ok": false, "path": path, "values": {}, "error": "cannot read " + path,
				"missing": false}
	var text := file.get_as_text()
	file.close()
	var parsed: Variant = JSON.parse_string(text)
	if not (parsed is Dictionary):
		return {"ok": false, "path": path, "values": {}, "error": "invalid JSON",
				"missing": false}
	var raw: Dictionary = parsed
	if int(raw.get("version", 0)) != VERSION:
		return {"ok": false, "path": path, "values": {}, "error": "unsupported version",
				"missing": false}
	return {"ok": true, "path": path, "values": raw, "error": "", "missing": false}


## Face values a Setup control panel writes back. Kept here so the panel and
## the tests share one definition of the multiplier ranges.
static func face_scale(values: Dictionary) -> float:
	return float(values.face.scale)


static func head_distance(values: Dictionary) -> float:
	var face: Dictionary = values.face
	var cut: Dictionary = values.head.cut
	var up: Vector3 = face.up.normalized()
	return (Vector3(face.origin) - Vector3(cut.origin)).dot(up) / float(face.scale)


static func head_falloff(values: Dictionary) -> float:
	return float(values.head.cut.falloff) / float(values.face.scale)


static func region(values: Dictionary, id: String) -> Dictionary:
	for entry in values.regions:
		if str(entry.get("id", "")) == id:
			return entry
	return {}


static func with_face_scale(values: Dictionary, multiplier: float) -> Dictionary:
	var next := values.duplicate(true)
	var base_scale := float(values.face.scale)
	next.face.scale = maxf(base_scale * multiplier, 0.001)
	return next


static func with_face_offset(values: Dictionary, offset: Vector3) -> Dictionary:
	var next := values.duplicate(true)
	var scale := float(values.face.scale)
	next.face.origin = Vector3(values.face.origin) + offset * scale
	return next


static func with_head(values: Dictionary, distance: float, falloff: float) -> Dictionary:
	var next := values.duplicate(true)
	var face: Dictionary = next.face
	var up: Vector3 = Vector3(face.up).normalized()
	var scale := float(face.scale)
	next.head.cut.origin = Vector3(face.origin) - up * scale * maxf(distance, 0.05)
	next.head.cut.normal = up
	next.head.cut.falloff = maxf(scale * falloff, 0.001)
	return next


static func with_region(values: Dictionary, id: String, center: Vector2, size: Vector2) -> Dictionary:
	var next := values.duplicate(true)
	for entry in next.regions:
		if str(entry.get("id", "")) == id:
			entry.center = center
			entry.size = Vector2(maxf(size.x, 0.01), maxf(size.y, 0.01))
			break
	return next


static func _read_face(raw: Variant, fallback: Dictionary) -> Dictionary:
	if not (raw is Dictionary):
		return fallback.duplicate(true)
	var source: Dictionary = raw
	var origin: Variant = _vec3(source.get("origin", fallback.origin))
	var normal: Variant = _vec3(source.get("normal", fallback.normal))
	var up: Variant = _vec3(source.get("up", fallback.up))
	var scale: Variant = source.get("scale", fallback.scale)
	if origin == null or normal == null or up == null:
		return {}
	if not (origin as Vector3).is_finite() or not (normal as Vector3).is_finite() \
			or not (up as Vector3).is_finite():
		return {}
	if not (scale is float or scale is int) or not is_finite(float(scale)) or float(scale) <= 0.0:
		return {}
	return {
		"origin": origin as Vector3,
		"normal": (normal as Vector3).normalized(),
		"up": (up as Vector3).normalized(),
		"scale": float(scale),
	}


static func _read_regions(raw: Variant, fallback: Array) -> Array:
	if not (raw is Array):
		return fallback.duplicate(true)
	var incoming: Array = raw
	var out: Array = []
	for id in REGION_IDS:
		var source: Dictionary = {}
		for entry in incoming:
			if entry is Dictionary and str(entry.get("id", "")) == id:
				source = entry
				break
		var base: Dictionary = {}
		for entry in fallback:
			if str(entry.get("id", "")) == id:
				base = entry
				break
		if base.is_empty():
			return []
		var center: Variant = _vec2(source.get("center", base.center))
		var size: Variant = _vec2(source.get("size", base.size))
		var feather: Variant = source.get("feather", base.feather)
		if center == null or size == null:
			return []
		if not (center as Vector2).is_finite() or not (size as Vector2).is_finite():
			return []
		if (size as Vector2).x <= 0.0 or (size as Vector2).y <= 0.0:
			return []
		if not is_finite(float(feather)) or float(feather) < 0.0 or float(feather) > 1.0:
			return []
		var region_entry := base.duplicate(true)
		region_entry.center = center as Vector2
		region_entry.size = size as Vector2
		region_entry.feather = float(feather)
		out.append(region_entry)
	return out


static func _read_head(raw: Variant, fallback: Dictionary, face: Dictionary) -> Dictionary:
	if not (raw is Dictionary):
		return fallback.duplicate(true)
	var source: Dictionary = raw
	var cut_raw: Variant = source.get("cut", {})
	if not (cut_raw is Dictionary):
		return {}
	var cut: Dictionary = cut_raw
	var origin: Variant = _vec3(cut.get("origin", fallback.cut.origin))
	var normal: Variant = _vec3(cut.get("normal", fallback.cut.normal))
	var falloff: Variant = cut.get("falloff", fallback.cut.falloff)
	if origin == null or normal == null:
		return {}
	if not (origin as Vector3).is_finite() or not (normal as Vector3).is_finite():
		return {}
	if not is_finite(float(falloff)) or float(falloff) <= 0.0:
		return {}
	var n := (normal as Vector3).normalized()
	var up := Vector3(face.up).normalized()
	# AvatarModel requires the cut plane below the face along its up axis.
	if n.dot(up) <= 0.0 or (Vector3(face.origin) - (origin as Vector3)).dot(n) <= 0.0:
		return {}
	return {
		"mode": "cut",
		"cut": {
			"origin": origin as Vector3,
			"normal": n,
			"falloff": float(falloff),
		},
	}


static func _encode_regions(regions: Array) -> Array:
	var out: Array = []
	for entry in regions:
		out.append({
			"id": str(entry.id),
			"center": _array2(entry.center),
			"size": _array2(entry.size),
			"feather": float(entry.feather),
		})
	return out


static func _array2(value: Vector2) -> Array:
	return [value.x, value.y]


static func _array3(value: Vector3) -> Array:
	return [value.x, value.y, value.z]


static func _vec2(value: Variant) -> Variant:
	if value is Vector2:
		return value
	if value is Array and (value as Array).size() >= 2:
		return Vector2(float(value[0]), float(value[1]))
	return null


static func _vec3(value: Variant) -> Variant:
	if value is Vector3:
		return value
	if value is Array and (value as Array).size() >= 3:
		return Vector3(float(value[0]), float(value[1]), float(value[2]))
	return null
