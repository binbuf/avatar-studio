extends RefCounted

## Provisional M1 face calibration for the bundled avatar (T26).
##
## T32/T34 replace this with the auto-placement and gizmo workflow. Until then a
## single cut-mode head region and three face regions derived from the model
## AABB are enough to prove that the real tracker, FaceRig and decal are wired
## to the same surface. The values match the reviewed T20/T21 fixture geometry;
## they are production constants, not test imports.

const DEFAULT_FEATHER := 0.18
const DEFAULT_CORNER_RADIUS := 0.35


static func make_face(aabb: AABB) -> Dictionary:
	var centre := aabb.get_center()
	var scale := maxf(aabb.size.y * 0.24, 0.02)
	return {
		"origin": Vector3(centre.x, centre.y + aabb.size.y * 0.08,
				aabb.position.z - 0.001),
		"normal": Vector3(0.0, 0.0, -1.0),
		"up": Vector3.UP,
		"right": Vector3.RIGHT,
		"scale": scale,
	}


static func make_head_region(face: Dictionary) -> Dictionary:
	return {
		"mode": "cut",
		"cut": {
			"origin": face.origin - Vector3.UP * float(face.scale) * 1.5,
			"normal": Vector3.UP,
			"falloff": face.scale,
		},
	}


static func make_regions() -> Array:
	return [
		{
			"id": "eye_l",
			"type": "eye",
			"center": Vector2(-0.5, 0.25),
			"size": Vector2(0.42, 0.30),
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
			"center": Vector2(0.5, 0.25),
			"size": Vector2(0.42, 0.30),
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
			"center": Vector2(0.0, -0.22),
			"size": Vector2(0.54, 0.38),
			"rotation_deg": 0.0,
			"shape": "rounded_rect",
			"feather": 0.22,
			"opacity": 1.0,
			"gain": Vector3.ONE,
			"lift": Vector3.ZERO,
		},
	]
