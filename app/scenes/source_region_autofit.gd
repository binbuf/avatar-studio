extends RefCounted

## Automatic camera-side (source) cutout fit for the Setup window.
##
## The tracker already produces dense landmarks; the rig's homography normalises
## them into canonical face space. This helper turns the eye/mouth contour points
## into a source rect per region:
##
##   centre  the tracked feature centre in canonical space;
##   width   the contour's canonical width plus a small margin;
##   height  the width divided by the target rect's aspect ratio.
##
## The height follows the *target* aspect on purpose. The decal maps the whole
## source rect onto the target rect, so a matching aspect keeps the sampled patch
## from being stretched. The feature's own height is not used: eyelids and lips
## move with every blink and syllable, while the corner-to-corner width is stable.
##
## The result is a suggestion, not a calibration: the Setup window smooths it and
## applies it live, and any manual camera edit turns it off. Version 1/2
## calibration files keep their stored source rects; this never rewrites them on
## its own.

const REGION_IDS := ["eye_l", "eye_r", "mouth"]

## MediaPipe face-mesh contours (478-point topology, pinned by
## native/tracking/include/avatar/tracking/landmark_map.h). The eye lists are the
## 16-point lid contours; the mouth list is the outer lip contour.
const EYE_L_CONTOUR := [263, 249, 390, 373, 374, 380, 381, 382, 362, 398, 384, 385,
		386, 387, 388, 466]
const EYE_R_CONTOUR := [33, 7, 163, 144, 145, 153, 154, 155, 133, 173, 157, 158,
		159, 160, 161, 246]
const MOUTH_CONTOUR := [61, 146, 91, 181, 84, 17, 314, 405, 321, 375, 291, 409, 270,
		269, 267, 0, 37, 39, 40, 185]

## Margin around the tracked contour, as a multiplier on its width.
const FEATURE_MARGIN := 1.12
const MIN_WIDTH := 0.08
const MAX_WIDTH := 1.20
## The canonical face uses inter-ocular distance = 1.0, so a width this large is
## almost certainly a tracking artefact rather than a real feature.
const MAX_CANONICAL_WIDTH := 1.6
## Plausible *contour* widths in canonical units. Below the floor a sample is a
## tracking glitch; above the ceiling it is an extreme expression.
const CONTOUR_FLOOR := {"eye_l": 0.12, "eye_r": 0.12, "mouth": 0.18}
const CONTOUR_CEILING := {"eye_l": 0.60, "eye_r": 0.60, "mouth": 1.10}


## Returns one source rect per region, or an empty array when the landmarks or
## the homography cannot produce a usable fit. Callers keep their current cutouts
## in that case; a partial fit is not applied.
static func fit(landmarks: PackedVector2Array, homography: PackedFloat32Array,
		targets: Array) -> Array:
	var inverse := _invert(homography)
	if inverse.size() != 9 or landmarks.size() < 468:
		return []
	var out: Array = []
	for id in REGION_IDS:
		var target := _find(targets, id)
		var box := _canonical_box(landmarks, _contour(id), inverse)
		if box.size.x <= 0.0 or box.size.y <= 0.0:
			return []
		var aspect := 1.0
		if not target.is_empty():
			var target_size: Vector2 = target.get("size", Vector2(0.2, 0.2))
			if target_size.x > 0.0 and target_size.y > 0.0:
				aspect = target_size.x / target_size.y
		var width := clampf(box.size.x * FEATURE_MARGIN, MIN_WIDTH, MAX_WIDTH)
		var size := Vector2(width, width / maxf(aspect, 0.05))
		size.x = clampf(size.x, MIN_WIDTH, MAX_CANONICAL_WIDTH)
		size.y = clampf(size.y, MIN_WIDTH, MAX_CANONICAL_WIDTH)
		out.append({
			"id": id,
			"center": box.get_center(),
			"size": size,
			"width": size.x,
			"contour_width": box.size.x,
			"rotation_deg": float(target.get("rotation_deg", 0.0)) if not target.is_empty() else 0.0,
			"shape": String(target.get("shape", "ellipse")) if not target.is_empty() else "ellipse",
		})
	return out


## One stable sampling width from the settle window's raw contour widths.
##
## Eyes average: the corner-to-corner distance does not change with a blink or
## gaze. The mouth takes the smallest plausible width instead, because a smile is
## much wider than a resting mouth and a window sized on the smile would paint a
## small mouth once the subject relaxes. Samples below the floor are tracking
## glitches rather than a closed mouth, so they are ignored.
static func settled_width(id: String, widths: Array) -> float:
	var floor_width := float(CONTOUR_FLOOR.get(id, 0.12))
	var ceiling := float(CONTOUR_CEILING.get(id, 0.60))
	var sum := 0.0
	var count := 0
	var smallest := INF
	for raw in widths:
		var value := float(raw)
		if not is_finite(value) or value < floor_width or value > MAX_CANONICAL_WIDTH:
			continue
		sum += value
		count += 1
		smallest = minf(smallest, value)
	if count == 0:
		return 0.0
	var width := smallest if id == "mouth" else sum / float(count)
	return clampf(width, floor_width, ceiling)


## Low-pass filters a fitted set into `state` (id -> {center, size}). Landmarks
## jitter by a few pixels; the cutout must not.
static func smooth(state: Dictionary, fitted: Array, alpha: float) -> void:
	var blend := clampf(alpha, 0.0, 1.0)
	for entry in fitted:
		var id := String(entry.get("id", ""))
		if id == "":
			continue
		var centre: Vector2 = entry.get("center", Vector2.ZERO)
		var size: Vector2 = entry.get("size", Vector2.ZERO)
		if state.has(id):
			var previous: Dictionary = state[id]
			state[id] = {
				"center": Vector2(previous.center).lerp(centre, blend),
				"size": Vector2(previous.size).lerp(size, blend),
			}
		else:
			state[id] = {"center": centre, "size": size}


## True when `current` differs from the last applied values by more than
## `epsilon`, so a converged fit stops re-applying every tick.
static func differs(current: Dictionary, applied: Dictionary, epsilon := 0.002) -> bool:
	for id in current.keys():
		if not applied.has(id):
			return true
		var now: Dictionary = current[id]
		var was: Dictionary = applied[id]
		if Vector2(now.center).distance_to(Vector2(was.center)) > epsilon:
			return true
		if Vector2(now.size).distance_to(Vector2(was.size)) > epsilon:
			return true
	return false


## The smoothed values, as source-region dictionaries. Missing ids keep their
## previous entry so a partial fit never drops a region.
static func regions_from_state(state: Dictionary, fallback: Array) -> Array:
	var out: Array = []
	for id in REGION_IDS:
		var previous := _find(fallback, id)
		if not state.has(id):
			out.append(previous)
			continue
		var entry: Dictionary = previous.duplicate(true) if not previous.is_empty() else {}
		var fitted: Dictionary = state[id]
		entry["id"] = id
		entry["center"] = Vector2(fitted.center)
		entry["size"] = Vector2(fitted.size)
		out.append(entry)
	return out


static func _contour(id: String) -> Array:
	match id:
		"eye_l":
			return EYE_L_CONTOUR
		"eye_r":
			return EYE_R_CONTOUR
		_:
			return MOUTH_CONTOUR


static func _find(entries: Array, id: String) -> Dictionary:
	for entry in entries:
		if entry is Dictionary and str(entry.get("id", "")) == id:
			return entry.duplicate(true)
	return {}


## Bounding box of the contour points in canonical face space.
static func _canonical_box(landmarks: PackedVector2Array, indices: Array,
		inverse: PackedFloat32Array) -> Rect2:
	var min_u := INF
	var max_u := -INF
	var min_v := INF
	var max_v := -INF
	var count := 0
	for raw in indices:
		var index := int(raw)
		if index < 0 or index >= landmarks.size():
			continue
		var canonical: Variant = _project(inverse, landmarks[index])
		if canonical == null:
			continue
		var point: Vector2 = canonical
		min_u = minf(min_u, point.x)
		max_u = maxf(max_u, point.x)
		min_v = minf(min_v, point.y)
		max_v = maxf(max_v, point.y)
		count += 1
	if count < 3 or not is_finite(min_u) or not is_finite(max_v):
		return Rect2()
	var box := Rect2(Vector2(min_u, min_v), Vector2(max_u - min_u, max_v - min_v))
	if box.size.x <= 0.0001 or box.size.y <= 0.0001:
		return Rect2()
	if box.size.x > MAX_CANONICAL_WIDTH or box.size.y > MAX_CANONICAL_WIDTH:
		return Rect2()
	return box


static func _project(inverse: PackedFloat32Array, point: Vector2) -> Variant:
	var w := inverse[6] * point.x + inverse[7] * point.y + inverse[8]
	if absf(w) < 1e-9:
		return null
	var u := (inverse[0] * point.x + inverse[1] * point.y + inverse[2]) / w
	var v := (inverse[3] * point.x + inverse[4] * point.y + inverse[5]) / w
	var result := Vector2(u, v)
	if not result.is_finite():
		return null
	return result


## Inverts a row-major 3x3 homography. Returns an empty array when singular.
static func _invert(h: PackedFloat32Array) -> PackedFloat32Array:
	if h.size() != 9:
		return PackedFloat32Array()
	var a := h[0]
	var b := h[1]
	var c := h[2]
	var d := h[3]
	var e := h[4]
	var f := h[5]
	var g := h[6]
	var i := h[7]
	var j := h[8]
	var determinant := a * (e * j - f * i) - b * (d * j - f * g) + c * (d * i - e * g)
	if absf(determinant) < 1e-12:
		return PackedFloat32Array()
	var inv := PackedFloat32Array()
	inv.resize(9)
	inv[0] = (e * j - f * i) / determinant
	inv[1] = (c * i - b * j) / determinant
	inv[2] = (b * f - c * e) / determinant
	inv[3] = (f * g - d * j) / determinant
	inv[4] = (a * j - c * g) / determinant
	inv[5] = (c * d - a * f) / determinant
	inv[6] = (d * i - e * g) / determinant
	inv[7] = (b * g - a * i) / determinant
	inv[8] = (a * e - b * d) / determinant
	return inv
