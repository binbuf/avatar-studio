extends RefCounted

## Small editor preferences for the Setup window (T25c).
##
## These are *editor* toggles, not calibration: auto-fitting the camera cutouts
## and the eye symmetry lock do not change what the decal consumes, so they stay
## out of `live_calibration.json` and out of the `.avatar` package. They persist
## in `user://` so a restart keeps the operator's choice.

const PATH := "user://setup_prefs.json"
const DEFAULTS := {"auto_fit": true, "lock_eyes": false}


static func load_values() -> Dictionary:
	var values := DEFAULTS.duplicate(true)
	if not FileAccess.file_exists(PATH):
		return values
	var file := FileAccess.open(PATH, FileAccess.READ)
	if file == null:
		return values
	var parsed: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if not (parsed is Dictionary):
		return values
	for key in DEFAULTS.keys():
		var value: Variant = (parsed as Dictionary).get(key, null)
		if value is bool:
			values[key] = value
	return values


static func save_values(values: Dictionary) -> void:
	var out := DEFAULTS.duplicate(true)
	for key in DEFAULTS.keys():
		out[key] = bool(values.get(key, DEFAULTS[key]))
	var file := FileAccess.open(PATH, FileAccess.WRITE)
	if file == null:
		return
	file.store_string(JSON.stringify(out, "  ", false))
	file.close()
