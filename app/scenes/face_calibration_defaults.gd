extends RefCounted

## Best-effort face calibration for the bundled avatar (T26/T25c).
##
## T32/T34 replace this with the auto-placement and gizmo workflow. Until then
## these defaults are deliberately model-aware enough to look like a face on the
## avocado test model:
##
## - the face plane is 26% of the model height, so the eye pair spans about
##   three-fifths of the model width instead of a tiny cluster;
## - the eye line is 20% above the model centre (30% down from the top), and the
##   mouth is 51 canonical units below it, on the green area above the pit;
## - the head cut is 2.5 face scales below the eye line, so the whole avocado
##   turns rigidly instead of shearing around the pit.
##
## The camera-side source defaults are a *sampling window*, not the model
## placement: the eyes sample the eye corners and the mouth samples the lips,
## with the height following the painted aspect. The Setup auto-fit replaces
## them with measured values while the face is frontal and relaxed.

const DEFAULT_FEATHER := 0.18
const DEFAULT_CORNER_RADIUS := 0.35

const FACE_SCALE_OF_HEIGHT := 0.26
const FACE_ORIGIN_Y := 0.20
const HEAD_CUT_DISTANCE := 2.5
const HEAD_FALLOFF := 2.0

## Canonical face proportions (inter-ocular distance = 1.0). The eye line is the
## canonical origin; the mouth centre is measured from the dense landmark
## template (corners at v ~= -0.51).
const EYE_CENTRE_U := 0.5
const EYE_CENTRE_V := 0.0
const EYE_SIZE := Vector2(0.58, 0.36)
const MOUTH_CENTRE := Vector2(0.0, -0.58)
const MOUTH_SIZE := Vector2(0.86, 0.38)
## Source windows. Widths are typical measured values; the height matches the
## painted aspect so the decal does not stretch the patch.
const SOURCE_EYE_SIZE := Vector2(0.44, 0.273)
const SOURCE_MOUTH_SIZE := Vector2(0.99, 0.438)


static func make_face(aabb: AABB) -> Dictionary:
	var centre := aabb.get_center()
	var scale := maxf(aabb.size.y * FACE_SCALE_OF_HEIGHT, 0.02)
	return {
		"origin": Vector3(centre.x, centre.y + aabb.size.y * FACE_ORIGIN_Y,
				aabb.position.z - 0.001),
		"normal": Vector3(0.0, 0.0, -1.0),
		"up": Vector3.UP,
		"right": Vector3.RIGHT,
		"scale": scale,
	}


static func make_head_region(face: Dictionary) -> Dictionary:
	var scale := float(face.scale)
	return {
		"mode": "cut",
		"cut": {
			"origin": face.origin - Vector3.UP * scale * HEAD_CUT_DISTANCE,
			"normal": Vector3.UP,
			"falloff": scale * HEAD_FALLOFF,
		},
	}


static func make_regions() -> Array:
	return [
		{
			"id": "eye_l",
			"type": "eye",
			"center": Vector2(-EYE_CENTRE_U, EYE_CENTRE_V),
			"size": EYE_SIZE,
			"rotation_deg": 0.0,
			"shape": "ellipse",
			"feather": DEFAULT_FEATHER,
			"opacity": 1.0,
			"gain": Vector3.ONE,
			"lift": Vector3.ZERO,
		},
		{
			"id": "eye_r",
			"type": "eye",
			"center": Vector2(EYE_CENTRE_U, EYE_CENTRE_V),
			"size": EYE_SIZE,
			"rotation_deg": 0.0,
			"shape": "ellipse",
			"feather": DEFAULT_FEATHER,
			"opacity": 1.0,
			"gain": Vector3.ONE,
			"lift": Vector3.ZERO,
		},
		{
			"id": "mouth",
			"type": "mouth",
			"center": MOUTH_CENTRE,
			"size": MOUTH_SIZE,
			"rotation_deg": 0.0,
			"shape": "rounded_rect",
			"feather": 0.22,
			"opacity": 1.0,
			"gain": Vector3.ONE,
			"lift": Vector3.ZERO,
		},
	]


## Camera-side selection: which canonical face area to sample from the camera.
## It is a stable sampling window, not the model placement; the Setup window
## measures it from the tracker and lets the operator override it.
static func make_source_regions() -> Array:
	return [
		{
			"id": "eye_l",
			"center": Vector2(-EYE_CENTRE_U, EYE_CENTRE_V),
			"size": SOURCE_EYE_SIZE,
			"rotation_deg": 0.0,
			"shape": "ellipse",
		},
		{
			"id": "eye_r",
			"center": Vector2(EYE_CENTRE_U, EYE_CENTRE_V),
			"size": SOURCE_EYE_SIZE,
			"rotation_deg": 0.0,
			"shape": "ellipse",
		},
		{
			"id": "mouth",
			"center": MOUTH_CENTRE,
			"size": SOURCE_MOUTH_SIZE,
			"rotation_deg": 0.0,
			"shape": "rounded_rect",
		},
	]
