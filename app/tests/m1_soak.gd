extends SceneTree

## M1 10-minute app soak and latency measurement (T26).
##
## Runs the real main scene (runtime, real camera/tracker pipeline, Live HUD and
## window output) at 1080p60 and writes `.tools/T26-soak.json` in the T05
## `avatar_bench.result.v1` shape. The T27 gate re-reads the JSON and applies the
## thresholds; this script only measures and reports.
##
##   --render                 required
##   --seconds N              measured duration (default 600)
##   --warmup N               discarded warmup (default 30)
##   --live-source camera|synthetic
##   --live-tracker PATH|scripted
##   --no-write               do not overwrite the evidence file (gate probes)

const SOAK_SCHEMA := "avatar_bench.result.v1"
const FRAME_TIME_LIMIT_MS := 16.6
const APP_P50_LIMIT_MS := 70.0
const APP_P95_LIMIT_MS := 100.0
const RSS_GROWTH_LIMIT_PERCENT := 5.0
const PACING_DRIFT_LIMIT_PERCENT := 5.0

var failures: Array[String] = []
var seconds := 600.0
var warmup_seconds := 30.0
var write_result := true
var scene: Node
var runtime: Node
var hud: Node


func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	seconds = _arg_float(args, "--seconds", 600.0)
	warmup_seconds = _arg_float(args, "--warmup", 30.0)
	write_result = not ("--no-write" in args)
	_run.call_deferred()


func _arg_float(args: PackedStringArray, name: String, fallback: float) -> float:
	var index := args.find(name)
	if index >= 0 and index + 1 < args.size():
		return float(args[index + 1])
	return fallback


func check(ok: bool, message: String) -> void:
	if not ok:
		failures.append(message)
		printerr("FAIL: ", message)


func _run() -> void:
	if DisplayServer.get_name() == "headless":
		failures.append("the soak needs a real display")
		_finish({})
		return
	await process_frame
	var packed := load("res://scenes/main.tscn")
	if packed == null:
		failures.append("main.tscn did not load")
		_finish({})
		return
	scene = packed.instantiate() as Node
	root.add_child(scene)
	await process_frame
	await process_frame
	await process_frame
	runtime = scene.get_node_or_null("AvatarRuntime")
	hud = scene.get_node_or_null("LiveHud")
	if runtime == null or hud == null:
		failures.append("main scene is missing AvatarRuntime or LiveHud")
		_finish({})
		return

	print("T26 soak: warmup ", warmup_seconds, " s, measured ", seconds, " s")
	var warmup_start := Time.get_ticks_usec()
	while (Time.get_ticks_usec() - warmup_start) < int(warmup_seconds * 1_000_000.0):
		await process_frame
	runtime.call("reset_pacing_stats")
	hud.call("reset_metrics")
	var baseline_output: Dictionary = hud.call("get_output_stats")
	var baseline_rss := int(hud.call("get_pipeline_stats").get("rss_bytes", 0))
	var start_us := Time.get_ticks_usec()
	var last_sample_us := start_us
	var frames := 0
	var rss_samples: Array[Dictionary] = []
	var peak_rss := baseline_rss
	while (Time.get_ticks_usec() - start_us) < int(seconds * 1_000_000.0):
		await process_frame
		frames += 1
		var now := Time.get_ticks_usec()
		if now - last_sample_us >= 5_000_000:
			last_sample_us = now
			var stats: Dictionary = hud.call("get_pipeline_stats")
			var rss := int(stats.get("rss_bytes", 0))
			peak_rss = maxi(peak_rss, rss)
			rss_samples.append({
				"t_s": float(now - start_us) / 1_000_000.0,
				"rss_bytes": rss,
				"cpu_percent_one_core": float(stats.get("cpu_percent_one_core", 0.0)),
			})
	var elapsed_s := float(Time.get_ticks_usec() - start_us) / 1_000_000.0
	print("T26 soak: measurement loop done, frames=", frames, " elapsed=", elapsed_s)
	var end_output: Dictionary = hud.call("get_output_stats")
	var pipeline: Dictionary = hud.call("get_pipeline_stats")
	var final_rss := int(pipeline.get("rss_bytes", peak_rss))
	peak_rss = maxi(peak_rss, final_rss)
	var growth := 0.0
	if baseline_rss > 0:
		growth = float(peak_rss - baseline_rss) / float(baseline_rss) * 100.0
	var submitted := int(end_output.get("submitted", 0)) - int(baseline_output.get("submitted", 0))
	var dropped_output := int(end_output.get("dropped", 0)) - int(baseline_output.get("dropped", 0))
	var pacing := _pacing(frames, elapsed_s)
	var latency: Dictionary = pipeline.get("app_latency_ms", {})
	var tracker: Dictionary = pipeline.get("tracker", {})
	var metrics := {
		"frame_time_ms": _distribution(
				float(runtime.call("get_frame_time_p50_ms")),
				float(runtime.call("get_frame_time_p95_ms")),
				float(runtime.call("get_frame_time_p99_ms")),
				float(runtime.call("get_frame_time_mean_ms")),
				float(runtime.call("get_frame_time_max_ms")),
				int(runtime.call("get_pacing_sample_count"))),
		"frame_interval_ms": _distribution(
				float(runtime.call("get_interval_p50_ms")),
				float(runtime.call("get_interval_p95_ms")),
				float(runtime.call("get_interval_p99_ms")),
				float(runtime.call("get_interval_mean_ms")),
				float(runtime.call("get_max_interval_ms")),
				int(runtime.call("get_pacing_sample_count"))),
		"interval_jitter_ms": _distribution(
				float(runtime.call("get_jitter_p50_ms")),
				float(runtime.call("get_jitter_p95_ms")),
				float(runtime.call("get_jitter_p99_ms")),
				float(runtime.call("get_jitter_mean_ms")),
				float(runtime.call("get_max_interval_ms")),
				int(runtime.call("get_pacing_sample_count"))),
		"app_latency_ms": {
			"p50": float(latency.get("p50", 0.0)),
			"p95": float(latency.get("p95", 0.0)),
			"p99": float(latency.get("p99", 0.0)),
			"mean": float(latency.get("mean", 0.0)),
			"max": float(latency.get("max", 0.0)),
			"samples": int(latency.get("samples", 0)),
		},
		"dropped_frames": dropped_output + int(runtime.call("get_dropped_frames")),
		"dropped_tracking_frames": int(tracker.get("dropped", 0)),
		"frames": {
			"requested": frames,
			"warmup": 0,
			"produced": int(pipeline.get("counters", {}).get("output_frames", 0)),
			"measured": frames,
			"missed": int(runtime.call("get_dropped_frames")),
		},
		"rss_bytes": final_rss,
		"rss_peak_bytes": peak_rss,
		"rss_growth_percent": growth,
		"cpu_percent_one_core": float(pipeline.get("cpu_percent_one_core", 0.0)),
		"gpu_percent": null,
		"stage_ms": {
			"capture": null,
			"ring": null,
			"tracker": float(tracker.get("inference_p50_ms", 0.0)),
			"rig": null,
			"render": null,
		},
		"output": {
			"submitted": submitted,
			"dropped": dropped_output,
			"expected": int(elapsed_s * 60.0),
			"max_submit_cost_ms": float(end_output.get("max_submit_cost_ms", 0.0)),
		},
		"camera": pipeline.get("camera", {}),
		"ring": pipeline.get("ring", {}),
	}
	var gates := {
		"frame_time_p99_ms": _gate(float(metrics["frame_time_ms"]["p99"]),
				FRAME_TIME_LIMIT_MS, "<="),
		"interval_jitter_p99_ms": _gate(float(metrics["interval_jitter_ms"]["p99"]),
				FRAME_TIME_LIMIT_MS, "<="),
		"app_latency_p50_ms": _gate(float(metrics["app_latency_ms"]["p50"]),
				APP_P50_LIMIT_MS, "<="),
		"app_latency_p95_ms": _gate(float(metrics["app_latency_ms"]["p95"]),
				APP_P95_LIMIT_MS, "<="),
		"rss_growth_percent": _gate(growth, RSS_GROWTH_LIMIT_PERCENT, "<="),
		"dropped_output_frames": _gate(float(dropped_output), 0.0, "<="),
		"pacing_drift_percent": _gate(absf(float(pacing.get("drift_percent", 0.0))),
				PACING_DRIFT_LIMIT_PERCENT, "<="),
	}
	var passed := true
	for key in gates.keys():
		passed = passed and bool(gates[key]["passed"])
	check(passed, "one or more soak gates failed")
	var payload := {
		"schema": SOAK_SCHEMA,
		"scenario": "live",
		"generated_utc": Time.get_datetime_string_from_system(true),
		"build": hud.call("get_build_info"),
		"hardware": _hardware(),
		"config": {
			"seconds": seconds,
			"warmup_seconds": warmup_seconds,
			"width": 1920,
			"height": 1080,
			"fps": 60,
			"source": hud.call("get_pipeline_config"),
		},
		"metrics": metrics,
		"pacing": pacing,
		"rss_samples": rss_samples,
		"gates": gates,
		"failures": failures,
		"passed": passed and failures.is_empty(),
	}
	_finish(payload)


func _pacing(frames: int, elapsed_s: float) -> Dictionary:
	var achieved := 0.0 if elapsed_s <= 0.0 else float(frames) / elapsed_s
	var drift := 0.0 if achieved == 0.0 else (achieved - 60.0) / 60.0 * 100.0
	var refresh := 0.0
	if DisplayServer.get_screen_count() > 0:
		refresh = DisplayServer.screen_get_refresh_rate(DisplayServer.window_get_current_screen())
	return {
		"display_refresh_hz": refresh,
		"runtime_refresh_hz": float(runtime.call("get_display_refresh_hz")),
		"mode": String(runtime.call("get_pacing_mode")),
		"target_interval_ms": float(runtime.call("get_target_interval_ms")),
		"achieved_fps": achieved,
		"drift_percent": drift,
		"max_burst_frames": int(runtime.call("get_max_burst_frames")),
		"display_modes": {
			"60": {"status": "measured",
				"note": "60 fps output cadence measured on this host display"},
			"120": {"status": "pending-human-hardware",
				"note": "display-mode pacing needs a 120 Hz panel; FramePacer unit covers it"},
			"144": {"status": "pending-human-hardware",
				"note": "display-mode pacing needs a 144 Hz panel; FramePacer unit covers it"},
		},
	}


func _gate(value: float, limit: float, operator: String) -> Dictionary:
	var passed := value <= limit if operator == "<=" else value >= limit
	return {"value": value, "limit": limit, "operator": operator, "passed": passed}


func _distribution(p50: float, p95: float, p99: float, mean: float, maximum: float,
		samples: int) -> Dictionary:
	return {
		"p50": p50,
		"p95": p95,
		"p99": p99,
		"mean": mean,
		"max": maximum,
		"samples": samples,
	}


func _hardware() -> Dictionary:
	var memory := OS.get_memory_info()
	var physical := int(memory.get("physical", 0))
	var cpu := OS.get_processor_name()
	var gpu := RenderingServer.get_video_adapter_name()
	var os_name := "%s %s" % [OS.get_name(), OS.get_version()]
	var arch := Engine.get_architecture_name()
	var tuple := "%s|%s|%s|%s|%d" % [os_name, arch, cpu, gpu, physical]
	return {
		"os": os_name,
		"arch": arch,
		"cpu": cpu,
		"cores_physical": 0,
		"cores_logical": OS.get_processor_count(),
		"ram_bytes": physical,
		"gpu": gpu,
		"fingerprint": "godot32:%08x" % (tuple.hash() & 0xffffffff),
	}


func _finish(payload: Dictionary) -> void:
	print("T26 soak: finishing, passed=", payload.get("passed", false))
	var final := payload
	if final.is_empty():
		final = {"schema": SOAK_SCHEMA, "scenario": "live", "passed": false,
			"failures": failures}
	if write_result:
		var path := ProjectSettings.globalize_path("res://../.tools/T26-soak.json")
		print("T26 soak: writing ", path)
		var file := FileAccess.open(path, FileAccess.WRITE)
		if file != null:
			file.store_string(JSON.stringify(final, "  "))
		else:
			printerr("T26 soak: cannot open ", path)
	print("T26 soak: ", JSON.stringify({"passed": final.get("passed", false),
			"failures": final.get("failures", []),
			"metrics": final.get("metrics", {}), "gates": final.get("gates", {})}))
	if scene != null:
		scene.queue_free()
	quit(0 if bool(final.get("passed", false)) else 1)
