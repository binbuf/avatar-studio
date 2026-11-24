extends RefCounted

## Automatic camera-side (source) cutout measurement for the Setup window.
##
## The tracker reports dense landmarks; the rig's homography normalises them into
## canonical face space (inter-ocular distance = 1.0). This helper measures the
## eye and mouth sampling windows from *stable* geometry instead of the raw
## landmark bounding box:
##
##   eye centre   midpoint of the outer and inner eye corners. It does not move
##                with a blink or with gaze, unlike the iris or the eyelid box.
##   eye width    corner-to-corner distance plus a small margin. The height
##                follows the target aspect so the sampled patch is not stretched.
##   mouth centre midpoint of the mouth corners. The lip centres move with a
##                smile and with every syllable; the corners do not.
##   mouth width  the corner distance capped by a face-proportion prior derived
##                from the nose width. A smile can be 50% wider than a resting
##                mouth, so the raw corner distance is a poor sampling window.
##
## The result is a *suggestion*. The Setup window only accepts it while the face
## is frontal and relaxed, smooths it over a short window, and then holds it. A
## manual camera edit turns the automatic fit off.

const REGION_IDS := ["eye_l", "eye_r", "mouth"]

## MediaPipe face-mesh topology (pinned by
## native/tracking/include/avatar/tracking/landmark_map.h).
const EYE_R_CORNERS := [33, 133]
const EYE_L_CORNERS := [263, 362]
const EYE_L_CONTOUR := [263, 249, 390, 373, 374, 380, 381, 382, 362, 398, 384, 385,
		386, 387, 388, 466]
const EYE_R_CONTOUR := [33, 7, 163, 144, 145, 153, 154, 155, 133, 173, 157, 158,
		159, 160, 161, 246]
const MOUTH_CORNERS := [61, 291]
const MOUTH_CONTOUR := [61, 146, 91, 181, 84, 17, 314, 405, 321, 375, 291, 409, 270,
		269, 267, 0, 37, 39, 40, 185]
const NOSE_ALA := [129, 358]
## Face/head outline points used for the diagnostic box: forehead, chin,
## temples, jaw, cheeks, eye corners and mouth corners.
const FACE_OUTLINE := [10, 152, 127, 356, 234, 454, 50, 280, 33, 263, 61, 291]

## Margin around the measured feature, as a multiplier on its width.
const EYE_MARGIN := 1.08
const MOUTH_MARGIN := 1.06
## Resting mouth width relative to the outer nose width. A smile is wider than
## this; the cap keeps the sampling window from swallowing the cheeks.
const NOSE_TO_MOUTH := 1.30
## Below this the nose landmarks are too close to be a useful proportion (the
## synthetic test face is one example); fall back to the corner width.
const MIN_NOSE_WIDTH := 0.30
const MIN_EYE_WIDTH := 0.16
const MAX_EYE_WIDTH := 0.70
const MIN_MOUTH_WIDTH := 0.35
const MAX_MOUTH_WIDTH := 1.10
## The canonical face uses inter-ocular distance = 1.0, so a width this large is
## almost certainly a tracking artefact rather than a real feature.
const MAX_CANONICAL_WIDTH := 1.6

## The face box is a head box: the landmark outline plus margin, widened under
## yaw so a turned head stays inside it.
const FACE_BOX_MARGIN := 1.08
const FACE_BOX_YAW_MIN_COS := 0.55
const FACE_BOX_MAX_EXPANSION := 1.8
## Canonical face width and eye-line height measured from the dense landmark
## template (temples ~= +/-1.045, eyes at v ~= 0). Used to turn a face box into a
## fallback canonical mapping before the rig has solved a frame.
const FACE_WIDTH_CANONICAL := 2.09
const EYE_LINE_FROM_TOP := 0.32


## Full measurement. `angles` is (roll, yaw, pitch) in degrees, from
## `LivePipelineController.get_head_angles()`. Returns
## {ok, regions, face_box, quality}. A partial measurement is never returned:
## callers keep their current cutouts when `ok` is false.
static func measure(landmarks: PackedVector2Array, homography: PackedFloat32Array,
		targets: Array, angles := Vector3.ZERO) -> Dictionary:
	var out := {"ok": false, "regions": [], "face_box": Rect2(), "quality": {}}
	var inverse := _invert(homography)
	if inverse.size() != 9 or landmarks.size() < 468:
		return out
	var regions: Array = []
	var eye_widths: Array = []
	for id in ["eye_l", "eye_r"]:
		var corners: Array = EYE_L_CORNERS if id == "eye_l" else EYE_R_CORNERS
		var a: Variant = _project(inverse, landmarks[int(corners[0])])
		var b: Variant = _project(inverse, landmarks[int(corners[1])])
		if a == null or b == null:
			return out
		var centre: Vector2 = (Vector2(a) + Vector2(b)) * 0.5
		var width := clampf(Vector2(a).distance_to(Vector2(b)) * EYE_MARGIN,
				MIN_EYE_WIDTH, MAX_EYE_WIDTH)
		var target := _find(targets, id)
		var size := Vector2(width, width / _aspect(target, 1.5))
		if size.x > MAX_CANONICAL_WIDTH or size.y > MAX_CANONICAL_WIDTH:
			return out
		regions.append(_region(id, centre, size, target))
		eye_widths.append(width)
	var cr: Variant = _project(inverse, landmarks[int(MOUTH_CORNERS[0])])
	var cl: Variant = _project(inverse, landmarks[int(MOUTH_CORNERS[1])])
	if cr == null or cl == null:
		return out
	var mouth_centre: Vector2 = (Vector2(cr) + Vector2(cl)) * 0.5
	var corner_width := Vector2(cr).distance_to(Vector2(cl))
	var nose_width := 0.0
	var nose_a: Variant = _project(inverse, landmarks[int(NOSE_ALA[0])])
	var nose_b: Variant = _project(inverse, landmarks[int(NOSE_ALA[1])])
	if nose_a != null and nose_b != null:
		nose_width = Vector2(nose_a).distance_to(Vector2(nose_b))
	var mouth_width := corner_width
	if nose_width >= MIN_NOSE_WIDTH:
		mouth_width = minf(mouth_width, nose_width * NOSE_TO_MOUTH)
	mouth_width = clampf(mouth_width * MOUTH_MARGIN, MIN_MOUTH_WIDTH, MAX_MOUTH_WIDTH)
	var mouth_target := _find(targets, "mouth")
	var mouth_size := Vector2(mouth_width, mouth_width / _aspect(mouth_target, 2.15))
	if mouth_size.x > MAX_CANONICAL_WIDTH or mouth_size.y > MAX_CANONICAL_WIDTH:
		return out
	regions.append(_region("mouth", mouth_centre, mouth_size, mouth_target))
	var box := face_box(landmarks, float(angles.y))
	out["ok"] = true
	out["regions"] = regions
	out["face_box"] = box
	out["quality"] = {
		"eye_width_l": eye_widths[0],
		"eye_width_r": eye_widths[1],
		"mouth_width": mouth_width,
		"mouth_corner_width": corner_width,
		"nose_width": nose_width,
		"yaw_deg": float(angles.y),
		"pitch_deg": float(angles.z),
		"roll_deg": float(angles.x),
		"landmarks": landmarks.size(),
	}
	return out


## Compatibility wrapper: the measured source regions, or an empty array.
static func fit(landmarks: PackedVector2Array, homography: PackedFloat32Array,
		targets: Array, angles := Vector3.ZERO) -> Array:
	var result := measure(landmarks, homography, targets, angles)
	return result.get("regions", [])


## True when the sample is frontal and relaxed enough to define the neutral
## cutouts. A turned head foreshortens the eyes; a smile widens the mouth.
static func neutral(quality: Dictionary, yaw_limit := 20.0, pitch_limit := 15.0,
		roll_limit := 20.0) -> bool:
	if quality.is_empty():
		return false
	if absf(float(quality.get("yaw_deg", 0.0))) > yaw_limit:
		return false
	if absf(float(quality.get("pitch_deg", 0.0))) > pitch_limit:
		return false
	if absf(float(quality.get("roll_deg", 0.0))) > roll_limit:
		return false
	var mouth := float(quality.get("mouth_width", 0.0))
	if mouth <= 0.0 or mouth > 1.12:
		return false
	return true


## Robust centre/width per region from the settle window. Each sample is
## {centers: Array[Vector2], widths: Array[float]}; medians reject a blink,
## a smile or a single bad landmark frame without freezing the fit.
static func settle(samples: Dictionary) -> Dictionary:
	var out := {}
	for id in REGION_IDS:
		if not samples.has(id):
			continue
		var entry: Dictionary = samples[id]
		var centres: Array = entry.get("centers", [])
		var widths: Array = entry.get("widths", [])
		if centres.is_empty() or widths.is_empty():
			continue
		var xs: Array = []
		var ys: Array = []
		for point in centres:
			xs.append(float(point.x))
			ys.append(float(point.y))
		out[id] = {
			"center": Vector2(_median(xs), _median(ys)),
			"width": _median(widths),
		}
	return out


static func _median(values: Array) -> float:
	if values.is_empty():
		return 0.0
	var sorted: Array = values.duplicate()
	sorted.sort()
	var middle := sorted.size() / 2
	if sorted.size() % 2 == 1:
		return float(sorted[middle])
	return (float(sorted[middle - 1]) + float(sorted[middle])) * 0.5


## True when `current` differs from the last applied values by more than
## `epsilon`, so a converged fit stops re-applying every tick.
static func differs(current: Dictionary, applied: Dictionary, epsilon := 0.002) -> bool:
	for id in current.keys():
		if not applied.has(id):
			return true
		var now: Dictionary = current[id]
		var was: Dictionary = applied[id]
		if Vector2(now.get("center", Vector2.ZERO)).distance_to(
				Vector2(was.get("center", Vector2.ZERO))) > epsilon:
			return true
		if Vector2(now.get("size", Vector2.ZERO)).distance_to(
				Vector2(was.get("size", Vector2.ZERO))) > epsilon:
			return true
	return false


## Diagnostic face/head box in normalised image coordinates. The outline is
## widened under yaw so a profile view does not collapse the box.
static func face_box(landmarks: PackedVector2Array, yaw_deg := 0.0) -> Rect2:
	if landmarks.size() < 468:
		return Rect2()
	var min_x := INF
	var min_y := INF
	var max_x := -INF
	var max_y := -INF
	var count := 0
	for raw in FACE_OUTLINE:
		var index := int(raw)
		if index < 0 or index >= landmarks.size():
			continue
		var point := landmarks[index]
		if not point.is_finite():
			continue
		min_x = minf(min_x, point.x)
		min_y = minf(min_y, point.y)
		max_x = maxf(max_x, point.x)
		max_y = maxf(max_y, point.y)
		count += 1
	if count < 4 or max_x <= min_x or max_y <= min_y:
		return Rect2()
	var box := Rect2(Vector2(min_x, min_y), Vector2(max_x - min_x, max_y - min_y))
	box = box.grow(maxf(box.size.x, box.size.y) * (FACE_BOX_MARGIN - 1.0) * 0.5)
	var yaw := deg_to_rad(clampf(yaw_deg, -80.0, 80.0))
	var expansion := clampf(1.0 / maxf(cos(yaw), FACE_BOX_YAW_MIN_COS), 1.0,
			FACE_BOX_MAX_EXPANSION)
	var centre := box.get_center()
	box = Rect2(centre - box.size * expansion * 0.5, box.size * expansion)
	var low := Vector2(maxf(box.position.x, 0.0), maxf(box.position.y, 0.0))
	var high := Vector2(minf(box.end.x, 1.0), minf(box.end.y, 1.0))
	if high.x > low.x and high.y > low.y:
		box = Rect2(low, high - low)
	return box


## Canonical -> image stand-in used before the rig has a solve. Canonical (0,0)
## is the eye line, 32% down the face box; +u is image right and +v is image up.
## The canonical face is 2.09 inter-ocular units wide.
static func face_box_homography(box: Rect2) -> PackedFloat32Array:
	if box.size.x <= 0.001 or box.size.y <= 0.001:
		return PackedFloat32Array()
	var scale := box.size.x / FACE_WIDTH_CANONICAL
	var eye_y := box.position.y + box.size.y * EYE_LINE_FROM_TOP
	var cx := box.position.x + box.size.x * 0.5
	return PackedFloat32Array([scale, 0.0, cx, 0.0, -scale, eye_y, 0.0, 0.0, 1.0])


static func _region(id: String, centre: Vector2, size: Vector2, target: Dictionary) -> Dictionary:
	return {
		"id": id,
		"center": centre,
		"size": size,
		"rotation_deg": float(target.get("rotation_deg", 0.0)) if not target.is_empty() else 0.0,
		"shape": String(target.get("shape", "ellipse")) if not target.is_empty() else "ellipse",
	}


static func _aspect(target: Dictionary, fallback: float) -> float:
	if target.is_empty():
		return fallback
	var size: Vector2 = target.get("size", Vector2.ZERO)
	if size.x > 0.0 and size.y > 0.0:
		return size.x / size.y
	return fallback


static func _find(entries: Array, id: String) -> Dictionary:
	for entry in entries:
		if entry is Dictionary and str(entry.get("id", "")) == id:
			return entry.duplicate(true)
	return {}


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
