extends SceneTree

# T25a headless contract for single instance and activation.
#
# Run with:
#   <godot> --headless --path app --script res://tests/single_instance.gd \
#       --quit-after 60000 -- --single-instance --instance-app-id t25a-godot-1
#
# The main scene claims the real per-user lock. A separate probe process (the
# native avatar_instance_probe, the same library the application uses) sends an
# activation and exits. A second adapter in this process must report secondary
# and deliver, and a --new-instance adapter must be isolated from both.
#
# The gate sets AVATAR_INSTANCE_PROBE and AVATAR_T25A_RESULT.

var failures: Array[String] = []
var result: Dictionary = {}


func _initialize() -> void:
	_run.call_deferred()


func _check(condition: bool, message: String) -> void:
	if condition:
		print("  ok   ", message)
	else:
		failures.append(message)
		printerr("  FAIL ", message)


func _app_id() -> String:
	var args := OS.get_cmdline_user_args()
	var index := args.find("--instance-app-id")
	if index >= 0 and index + 1 < args.size():
		return args[index + 1]
	return "t25a-godot"


func _wait_for(condition: Callable, timeout_ms: int) -> bool:
	var deadline := Time.get_ticks_msec() + timeout_ms
	while Time.get_ticks_msec() < deadline:
		if bool(condition.call()):
			return true
		await process_frame
	return bool(condition.call())


func _activation_count_at_least(hud: Object, wanted: int) -> bool:
	var state: Dictionary = hud.call("get_single_instance_state")
	return int(state.get("activation_count", 0)) >= wanted


func _run() -> void:
	print("single_instance: T25a contract")
	var app_id := _app_id()
	var probe := OS.get_environment("AVATAR_INSTANCE_PROBE")
	_check(probe != "", "AVATAR_INSTANCE_PROBE names the native probe")
	_check("--single-instance" in OS.get_cmdline_user_args(),
			"the test opted into the real startup path")
	if probe == "":
		_finish(app_id)
		return

	var packed := load("res://scenes/main.tscn")
	_check(packed != null, "main scene loads")
	if packed == null:
		_finish(app_id)
		return
	var scene := packed.instantiate() as Node
	_check(scene != null, "main scene instantiates")
	if scene == null:
		_finish(app_id)
		return
	root.add_child(scene)
	await process_frame
	await process_frame

	var hud := scene.get_node_or_null("LiveHud")
	_check(hud != null, "main scene owns the Live surface")
	if hud == null:
		_finish(app_id)
		return

	var primary: Dictionary = hud.call("get_single_instance_state")
	var identity: Dictionary = primary.get("identity", {})
	_check(bool(primary.get("primary", false)), "first launch owns the lock")
	_check(bool(primary.get("channel_ready", false)), "activation channel is ready")
	_check(String(identity.get("lock_name", "")) != "", "lock has an OS name")
	_check(String(identity.get("activation_name", "")) != "", "activation has an OS name")
	_check(String(identity.get("shared_memory_name", "")) != "",
			"shared-memory name is reserved")
	_check(bool(identity.get("virtual_camera_enabled", false)),
			"normal launch allows the virtual camera")
	_check(bool(identity.get("output_registration_enabled", false)),
			"normal launch allows output registration")

	var avatar_path := "C:/tmp/My Avatar.avatar" if OS.get_name() == "Windows" \
			else "/tmp/My Avatar.avatar"
	var show_path := "C:/tmp/stream show.show" if OS.get_name() == "Windows" \
			else "/tmp/stream show.show"
	var pid := OS.create_process(probe, PackedStringArray([
		"--app-id", app_id, "--activate", "--avatar", avatar_path, "--show", show_path,
	]))
	_check(pid > 0, "second launch process started")
	var activated := await _wait_for(
			func() -> bool: return _activation_count_at_least(hud, 1), 8000)
	_check(activated, "second launch activated the first")
	var exit_deadline := Time.get_ticks_msec() + 5000
	while pid > 0 and OS.is_process_running(pid) and Time.get_ticks_msec() < exit_deadline:
		await process_frame
	_check(pid > 0 and not OS.is_process_running(pid), "second launch exited")
	var probe_exit := OS.get_process_exit_code(pid) if pid > 0 else -1
	_check(probe_exit == 0, "second launch exited 0 (got %d)" % probe_exit)

	var after_probe: Dictionary = hud.call("get_single_instance_state")
	var paths: Dictionary = after_probe.get("activation_paths", {})
	_check(String(paths.get("avatar", "")) == avatar_path, "activation carried the avatar path")
	_check(String(paths.get("show", "")) == show_path, "activation carried the show path")
	_check(bool(after_probe.get("window_raised", false)), "activation raised the window")
	var notice: Dictionary = after_probe.get("notice", {})
	_check(String(notice.get("sentence", "")) == "Avatar Studio is already running",
			"second-instance sentence is the reviewed copy")
	_check(String(notice.get("code", "")) == "CFG-007", "second-instance code is CFG-007")

	var second: Object = ClassDB.instantiate("AvatarSingleInstance")
	_check(second != null, "AvatarSingleInstance instantiates")
	var second_state: Dictionary = second.call("start", {
		"app_id": app_id,
		"avatar": "second-launch.avatar",
	}) if second != null else {}
	_check(bool(second_state.get("secondary", false)), "second launch is secondary")
	_check(not bool(second_state.get("primary", false)), "second launch did not take the lock")
	_check(String(second_state.get("state", "")) == "secondary-activated",
			"second launch delivered the activation")
	_check(String(second_state.get("identity", {}).get("lock_name", "")) ==
			String(identity.get("lock_name", "")), "second launch uses the same lock")
	var second_received := await _wait_for(
			func() -> bool: return _activation_count_at_least(hud, 2), 4000)
	_check(second_received, "the first instance received the in-process activation")
	var after_second: Dictionary = hud.call("get_single_instance_state")
	_check(String(after_second.get("activation_paths", {}).get("avatar", "")) ==
			"second-launch.avatar", "the in-process activation carried its path")
	if second != null:
		second.call("stop")

	var isolated: Object = ClassDB.instantiate("AvatarSingleInstance")
	_check(isolated != null, "a --new-instance adapter instantiates")
	var isolated_state: Dictionary = isolated.call("start", {
		"app_id": app_id,
		"new_instance": true,
	}) if isolated != null else {}
	var isolated_identity: Dictionary = isolated_state.get("identity", {})
	_check(bool(isolated_state.get("primary", false)),
			"new instance is primary while the normal instance runs")
	_check(bool(isolated_identity.get("new_instance", false)), "new instance identity is marked")
	_check(not bool(isolated_identity.get("virtual_camera_enabled", true)),
			"new instance disables the virtual camera")
	_check(not bool(isolated_identity.get("output_registration_enabled", true)),
			"new instance disables output registration")
	_check(String(isolated_identity.get("lock_name", "")) !=
			String(identity.get("lock_name", "")), "new instance uses a distinct lock")
	_check(String(isolated_identity.get("activation_name", "")) !=
			String(identity.get("activation_name", "")), "new instance uses a distinct channel")
	_check(String(isolated_identity.get("shared_memory_name", "")) !=
			String(identity.get("shared_memory_name", "")),
			"new instance uses a distinct shared-memory name")
	_check(bool(isolated_state.get("channel_ready", false)),
			"new instance owns its own activation channel")

	var normal_second: Object = ClassDB.instantiate("AvatarSingleInstance")
	var normal_state: Dictionary = normal_second.call("start", {"app_id": app_id}) \
			if normal_second != null else {}
	_check(bool(normal_state.get("secondary", false)) and
			bool(normal_state.get("delivered", false)),
			"a normal second launch still reaches the normal primary")
	if normal_second != null:
		normal_second.call("stop")
	var isolated_poll: Dictionary = isolated.call("poll") if isolated != null else {}
	_check(int(isolated_poll.get("count", 0)) == 0,
			"the test instance did not receive the normal activation")
	if isolated != null:
		isolated.call("stop")

	result["primary"] = primary
	result["probe"] = {
		"pid": pid,
		"exit_code": probe_exit,
		"avatar": avatar_path,
		"show": show_path,
	}
	result["second"] = second_state
	result["isolated"] = isolated_state
	result["normal_second"] = normal_state
	result["activation"] = after_second
	_finish(app_id)


func _finish(app_id: String) -> void:
	result["passed"] = failures.is_empty()
	result["app_id"] = app_id
	result["failures"] = failures
	var path := OS.get_environment("AVATAR_T25A_RESULT")
	if path == "":
		path = "user://T25a-single-instance.json"
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file != null:
		file.store_string(JSON.stringify(result, "  "))
		file.close()
	print("single_instance: %s (%d failure(s))" % [
		"PASS" if failures.is_empty() else "FAIL", failures.size(),
	])
	quit(0 if failures.is_empty() else 1)
