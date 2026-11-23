extends SceneTree

# T25c capture/tracker-input probe.
#
# Opens the real C920 and the real C-ABI tracker for each requested mode and
# records the negotiated capture mode, frames published, tracker inference
# p50/p95 and the face state/confidence. It does not need a person to measure
# mode negotiation or inference; the yaw/nod confidence review is the HITL item.
#
#   <godot> --headless --path app --script res://tests/live_capture_probe.gd \
#       --quit-after 120000

const MODES := [
	{"label": "auto", "width": 0, "height": 0, "fps": 0},
	{"label": "1280x720@30", "width": 1280, "height": 720, "fps": 30},
	{"label": "640x480@30", "width": 640, "height": 480, "fps": 30},
	{"label": "1920x1080@30", "width": 1920, "height": 1080, "fps": 30},
]
const INPUTS := [192, 256]
const WARMUP_US := 1_500_000
const MEASURE_US := 3_000_000

var runs: Array = []
var failures: Array[String] = []
var only_mode := ""
var only_input := 0


func _initialize() -> void:
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--mode="):
			only_mode = argument.substr("--mode=".length())
		elif argument.begins_with("--input="):
			only_input = int(argument.substr("--input=".length()))
	_run.call_deferred()


func _run() -> void:
	var dll := ProjectSettings.globalize_path("res://bin/libavatartracker.dll")
	if not FileAccess.file_exists(dll):
		failures.append("tracker DLL missing: " + dll)
		_finish()
		return
	for mode in MODES:
		if only_mode != "" and not String(mode.label).begins_with(only_mode):
			continue
		for input in INPUTS:
			if only_input > 0 and int(input) != only_input:
				continue
			await _probe(mode, int(input), dll)
	_finish()


func _probe(mode: Dictionary, tracker_input: int, dll: String) -> void:
	var ring = ClassDB.instantiate("AvatarCameraRing")
	if ring == null or not bool(ring.call("initialize", 4)):
		failures.append("camera ring did not initialize")
		return
	var pipeline = ClassDB.instantiate("AvatarLivePipeline")
	if pipeline == null:
		failures.append("AvatarLivePipeline is not registered")
		return
	pipeline.call("set_camera_ring", ring)
	var config := {
		"source": "camera",
		"tracker": dll,
		"tracker_input": tracker_input,
		"fps": 30,
	}
	if int(mode.width) > 0:
		config["capture_width"] = int(mode.width)
		config["capture_height"] = int(mode.height)
		config["capture_fps"] = int(mode.fps)
	if not bool(pipeline.call("initialize", config)):
		failures.append("pipeline initialize failed for %s" % mode.label)
		return
	pipeline.call("set_preview_enabled", true)
	if not bool(pipeline.call("start")):
		failures.append("pipeline start failed for %s: %s" % [mode.label,
				String(pipeline.call("get_last_error"))])
		return

	var warm_start := Time.get_ticks_usec()
	while Time.get_ticks_usec() - warm_start < WARMUP_US:
		pipeline.call("tick")
		await process_frame

	var measured_start := Time.get_ticks_usec()
	var frames := 0
	while Time.get_ticks_usec() - measured_start < MEASURE_US:
		pipeline.call("tick")
		frames += 1
		await process_frame
	var elapsed := maxf(0.001, float(Time.get_ticks_usec() - measured_start) / 1_000_000.0)

	var cfg: Dictionary = pipeline.call("get_config")
	var stats: Dictionary = pipeline.call("get_stats")
	var state: Dictionary = pipeline.call("get_state")
	var tracker: Dictionary = stats.get("tracker", {})
	var camera: Dictionary = stats.get("camera", {})
	var run := {
		"request": String(mode.label),
		"requested_input": tracker_input,
		"negotiated": "%dx%d@%d %s" % [int(cfg.get("width", 0)), int(cfg.get("height", 0)),
				int(round(float(cfg.get("fps", 0.0)))), String(cfg.get("format", "?"))],
		"capture_request": String(cfg.get("capture_request", "auto")),
		"tracker_kind": String(cfg.get("tracker", "none")),
		"tracker_input": int(cfg.get("tracker_input", 0)),
		"tracker_available": bool(cfg.get("tracker_available", false)),
		"frames_ticks": frames,
		"tick_fps": frames / elapsed,
		"camera_frames_published": int(camera.get("frames_published", 0)),
		"camera_restarts": int(camera.get("restarts", 0)),
		"camera_state": String(camera.get("state", "")),
		"inference_p50_ms": float(tracker.get("inference_p50_ms", 0.0)),
		"inference_p95_ms": float(tracker.get("inference_p95_ms", 0.0)),
		"tracker_dropped": int(tracker.get("dropped", 0)),
		"face": String(state.get("face", "unavailable")),
		"confidence": float(state.get("confidence", 0.0)),
	}
	runs.append(run)
	print("T25c probe: ", JSON.stringify(run))
	pipeline.call("stop")
	pipeline.call("shutdown")
	var settle_start := Time.get_ticks_usec()
	while Time.get_ticks_usec() - settle_start < 400_000:
		await process_frame


func _finish() -> void:
	var result := {
		"schema": "avatar_capture_probe.v1",
		"failures": failures,
		"passed": failures.is_empty(),
		"runs": runs,
	}
	var path := ProjectSettings.globalize_path("res://../.tools/T25c-capture-probe.json")
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(JSON.stringify(result, "  "))
	file.close()
	print("T25c capture probe: ", JSON.stringify(result))
	quit(0 if failures.is_empty() else 1)
