extends CanvasLayer

## Minimal Live surface (T25).
##
## The preview is the existing mirrored AvatarRuntime TextureRect. This layer
## adds the honest status bar, the panic banner, the edit guard and a small
## window-output monitor. It owns no rendering: the output is the T23 gate and
## the avatar viewport is sampled for the monitor only.
##
## T26 wires the real capture/tracker/FaceRig/decal pipeline through
## `live_pipeline_controller.gd`. The HUD numbers and the four states are now
## the pipeline's values; the debug trigger remains a review aid.

const PipelineController = preload("res://scenes/live_pipeline_controller.gd")
const SetupWindow = preload("res://scenes/setup_window.gd")

const HUD_INTERVAL := 0.25
const OUTPUT_INTERVAL := 1.0 / 30.0
const OUTPUT_SIZE := Vector2i(640, 360)
const UI_SCALES := [1.0, 1.5, 2.0]
const EDIT_KINDS := [
	"avatar", "calibration", "camera", "resolution", "transport", "outfit", "appearance",
]
const DEBUG_STATES := [
	"healthy", "camera_lost", "face_not_visible", "features_hidden", "degraded",
	"tracker_unavailable",
]

var runtime: Node = null
var viewport: SubViewport = null
var live_session: Object = null
var output_manager: Object = null
var output_target: TextureRect = null
var output_panel: PanelContainer = null
var monitor_title: Label = null
var root: Control = null
var banner: PanelContainer = null
var banner_title: Label = null
var banner_action: Label = null
var banner_code: Label = null
var stop_and_switch_button: Button = null
var panic_button: Button = null
var restore_button: Button = null
var stop_button: Button = null
var go_live_button: Button = null
var edit_menu: MenuButton = null
var debug_menu: MenuButton = null
var scale_button: Button = null
var labels: Dictionary = {}

var hud_timer := 0.0
var output_timer := 0.0
var ui_scale_index := 0
var ui_scale := 1.0
var notice: Dictionary = {}
var notice_until_usec := 0
var current_edit_kind := "avatar"
var debug_states_visible := false
var single_instance: Object = null
var single_instance_state: Dictionary = {}
var single_instance_ready := false
var activation_paths: Dictionary = {}
var window_raised := false
var pipeline_controller = null
var pipeline_state: Dictionary = {}
var setup_window = null
var setup_button: Button = null
var last_submit_cost_ms := 0.0
var max_submit_cost_ms := 0.0
var submit_pending := false


func _enter_tree() -> void:
	# Claim before any sibling's _ready can open a camera or an output device.
	single_instance_ready = _claim_single_instance()


func _ready() -> void:
	if not single_instance_ready:
		return
	runtime = get_parent().get_node_or_null("AvatarRuntime")
	viewport = get_parent().get_node_or_null("SubViewport")
	_build_ui()
	_register_panic_action()

	live_session = ClassDB.instantiate("AvatarLiveSession")
	if live_session == null:
		push_error("CFG-001: AvatarLiveSession is not registered")
		return
	live_session.call("start_global_hotkey")
	if "--live-debug" in OS.get_cmdline_user_args():
		set_debug_states_visible(true)
	_init_pipeline()
	_start_live()
	_build_setup()
	if "--setup" in OS.get_cmdline_user_args():
		open_setup()
	if bool(single_instance_state.get("new_instance", false)):
		var copy: Dictionary = single_instance_state.get("copy", {})
		_show_notice(String(copy.get("sentence", "")), String(copy.get("action", "")), "",
				false, 0.0)
		_update_hud(_now_ns())
	set_process(true)


func _exit_tree() -> void:
	if single_instance != null:
		single_instance.call("stop")
	if setup_window != null:
		setup_window.close()
	if pipeline_controller != null:
		pipeline_controller.stop()
	if live_session != null:
		live_session.call("stop_global_hotkey")
	if output_manager != null:
		output_manager.call("shutdown")


func _should_claim_single_instance() -> bool:
	# Test scripts drive main.tscn in a fresh process; claiming the real lock
	# would make an unrelated test exit because the developer has the app open.
	# The dedicated single-instance test passes --single-instance to opt in.
	if "--single-instance" in OS.get_cmdline_user_args():
		return true
	for argument in OS.get_cmdline_args():
		if argument == "--script":
			return false
	return true


func _claim_single_instance() -> bool:
	if not _should_claim_single_instance():
		return true
	var user_args := OS.get_cmdline_user_args()
	var new_instance := "--new-instance" in user_args
	single_instance = ClassDB.instantiate("AvatarSingleInstance")
	if single_instance == null:
		push_error("CFG-007: AvatarSingleInstance is not registered")
		return true
	var app_id := _arg_value(user_args, "--instance-app-id")
	if app_id == "":
		app_id = "avatar-studio"
	var config := {
		"app_id": app_id,
		"new_instance": new_instance,
		"avatar": _arg_value(user_args, "--avatar"),
		"show": _arg_value(user_args, "--show"),
	}
	single_instance_state = single_instance.call("start", config)
	var copy: Dictionary = single_instance_state.get("copy", {})
	var sentence := String(copy.get("sentence", "Avatar Studio is already running"))
	var action := String(copy.get("action", "Activate the existing window"))
	if bool(single_instance_state.get("secondary", false)):
		if bool(single_instance_state.get("delivered", false)):
			print("CFG-007: " + sentence + " - " + action)
			get_tree().quit(0)
		else:
			printerr("CFG-007: " + sentence + " - " + action)
			if DisplayServer.get_name() != "headless":
				OS.alert(sentence + "\n" + action, "Avatar Studio")
			get_tree().quit(1)
		return false
	if String(single_instance_state.get("state", "")) == "error":
		push_warning("CFG-007: single-instance lock failed: "
				+ String(single_instance_state.get("error", "")))
		return true
	return true


func _arg_value(args: PackedStringArray, name: String) -> String:
	var index := args.find(name)
	if index >= 0 and index + 1 < args.size():
		return args[index + 1]
	return ""


func _on_activation(activation: Dictionary) -> void:
	activation_paths = {
		"avatar": String(activation.get("avatar", "")),
		"show": String(activation.get("show", "")),
	}
	window_raised = true
	var window := get_window()
	if window != null:
		# Godot 4.7 deprecates move_to_foreground in favour of grab_focus.
		window.grab_focus()
	var state: Dictionary = single_instance.call("get_state")
	var copy: Dictionary = state.get("copy", {})
	_show_notice(String(copy.get("sentence", "Avatar Studio is already running")),
			String(copy.get("action", "Brought the existing window forward")),
			String(copy.get("code", "CFG-007")), false, 4.0)
	_update_hud(_now_ns())


func get_single_instance_state() -> Dictionary:
	if single_instance == null:
		return {}
	var state: Dictionary = single_instance.call("get_state")
	state["window_raised"] = window_raised
	state["activation_paths"] = activation_paths
	state["notice"] = notice
	return state


func _process(delta: float) -> void:
	if single_instance != null and bool(single_instance_state.get("primary", false)):
		var activation: Dictionary = single_instance.call("poll")
		if bool(activation.get("pending", false)):
			_on_activation(activation)
	if live_session == null:
		return
	var now := _now_ns()
	live_session.call("tick", now)
	if pipeline_controller != null:
		pipeline_state = pipeline_controller.tick()
		_feed_pipeline(pipeline_state)
	if runtime != null:
		live_session.call("report_frame", now, float(runtime.call("get_last_frame_time_ms")))
	if bool(live_session.call("consume_global_hotkey")):
		notice = {}
		_update_hud(now)

	hud_timer += delta
	if hud_timer >= HUD_INTERVAL:
		hud_timer = 0.0
		_update_hud(now)

	output_timer += delta
	if output_timer >= OUTPUT_INTERVAL:
		output_timer = 0.0
		_submit_output(now)

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_F2:
		if setup_window != null and bool(setup_window.is_open()):
			close_setup()
		else:
			open_setup()
		return
	if live_session == null:
		return
	if event.is_action_pressed("live_panic"):
		_toggle_panic("hotkey")


func _register_panic_action() -> void:
	if not InputMap.has_action("live_panic"):
		InputMap.add_action("live_panic")
	var has_ctrl := false
	var has_meta := false
	for event in InputMap.action_get_events("live_panic"):
		if event is InputEventKey and event.keycode == KEY_H:
			if event.ctrl_pressed and event.shift_pressed:
				has_ctrl = true
			if event.meta_pressed and event.shift_pressed:
				has_meta = true
	if not has_ctrl:
		var ctrl := InputEventKey.new()
		ctrl.keycode = KEY_H
		ctrl.ctrl_pressed = true
		ctrl.shift_pressed = true
		InputMap.action_add_event("live_panic", ctrl)
	if not has_meta:
		var meta := InputEventKey.new()
		meta.keycode = KEY_H
		meta.meta_pressed = true
		meta.shift_pressed = true
		InputMap.action_add_event("live_panic", meta)


func _build_ui() -> void:
	root = Control.new()
	root.name = "LiveRoot"
	root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	root.mouse_filter = Control.MOUSE_FILTER_PASS
	add_child(root)

	var margin := MarginContainer.new()
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	margin.add_theme_constant_override("margin_left", 16)
	margin.add_theme_constant_override("margin_top", 16)
	margin.add_theme_constant_override("margin_right", 16)
	margin.add_theme_constant_override("margin_bottom", 16)
	root.add_child(margin)

	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 8)
	margin.add_child(column)

	var top := PanelContainer.new()
	top.name = "TopBar"
	top.add_theme_stylebox_override("panel", _panel_style(0.74))
	column.add_child(top)
	var top_column := VBoxContainer.new()
	top_column.add_theme_constant_override("separation", 2)
	top.add_child(top_column)

	var row_one := HBoxContainer.new()
	row_one.add_theme_constant_override("separation", 12)
	top_column.add_child(row_one)
	labels["live"] = _make_label("LIVE", 18)
	labels["elapsed"] = _make_label("00:00:00", 18)
	labels["fps"] = _make_label("0 fps", 18)
	labels["frame"] = _make_label("0.0 ms", 18)
	for key in ["live", "elapsed", "fps", "frame"]:
		row_one.add_child(labels[key])

	var row_two := HBoxContainer.new()
	row_two.add_theme_constant_override("separation", 12)
	top_column.add_child(row_two)
	labels["confidence"] = _make_label("tracking -", 16)
	labels["face"] = _make_label("face unavailable", 16)
	labels["output"] = _make_label("output placeholder", 16)
	for key in ["confidence", "face", "output"]:
		row_two.add_child(labels[key])

	labels["global"] = _make_label("Global: checking", 13)
	labels["global"].autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	top_column.add_child(labels["global"])

	banner = PanelContainer.new()
	banner.name = "StatusBanner"
	banner.visible = false
	banner.add_theme_stylebox_override("panel", _panel_style(0.88))
	column.add_child(banner)
	var banner_box := VBoxContainer.new()
	banner_box.add_theme_constant_override("separation", 4)
	banner.add_child(banner_box)
	banner_title = _make_label("", 20)
	banner_title.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	banner_box.add_child(banner_title)
	banner_action = _make_label("", 16)
	banner_action.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	banner_box.add_child(banner_action)
	banner_code = _make_label("", 13)
	banner_code.add_theme_color_override("font_color", Color(0.72, 0.80, 0.92))
	banner_box.add_child(banner_code)
	stop_and_switch_button = _make_button("Stop and switch", _on_stop_and_switch_pressed)
	stop_and_switch_button.visible = false
	banner_box.add_child(stop_and_switch_button)

	var spacer := Control.new()
	spacer.size_flags_vertical = Control.SIZE_EXPAND_FILL
	column.add_child(spacer)

	var monitor_row := HBoxContainer.new()
	monitor_row.alignment = BoxContainer.ALIGNMENT_END
	column.add_child(monitor_row)
	output_panel = PanelContainer.new()
	output_panel.add_theme_stylebox_override("panel", _panel_style(0.74))
	monitor_row.add_child(output_panel)
	var monitor_box := VBoxContainer.new()
	monitor_box.add_theme_constant_override("separation", 2)
	output_panel.add_child(monitor_box)
	monitor_title = _make_label("Output (window, placeholder)", 13)
	monitor_box.add_child(monitor_title)
	output_target = TextureRect.new()
	output_target.name = "OutputWindow"
	output_target.custom_minimum_size = Vector2(160, 90)
	output_target.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	output_target.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	output_target.mouse_filter = Control.MOUSE_FILTER_IGNORE
	monitor_box.add_child(output_target)

	var bottom := PanelContainer.new()
	bottom.name = "BottomBar"
	bottom.add_theme_stylebox_override("panel", _panel_style(0.74))
	column.add_child(bottom)
	var buttons := HBoxContainer.new()
	buttons.add_theme_constant_override("separation", 8)
	bottom.add_child(buttons)

	panic_button = _make_button("Panic: hide face", _on_panic_pressed)
	restore_button = _make_button("Restore output", _on_restore_pressed)
	restore_button.visible = false
	stop_button = _make_button("Stop", _on_stop_pressed)
	go_live_button = _make_button("Go Live", _on_go_live_pressed)
	go_live_button.visible = false
	edit_menu = MenuButton.new()
	edit_menu.name = "EditMenu"
	edit_menu.text = "Edit..."
	edit_menu.add_theme_font_size_override("font_size", 16)
	var edit_popup := edit_menu.get_popup()
	for index in range(EDIT_KINDS.size()):
		var label: String = str(EDIT_KINDS[index]).capitalize()
		if index == 5:
			edit_popup.add_separator()
		edit_popup.add_item(label, index)
	edit_popup.id_pressed.connect(_on_edit_selected)

	debug_menu = MenuButton.new()
	debug_menu.name = "DebugStates"
	debug_menu.text = "Debug states"
	debug_menu.add_theme_font_size_override("font_size", 16)
	debug_menu.visible = false
	var debug_popup := debug_menu.get_popup()
	for index in range(DEBUG_STATES.size()):
		debug_popup.add_item(DEBUG_STATES[index].replace("_", " ").capitalize(), index)
	debug_popup.id_pressed.connect(_on_debug_selected)

	scale_button = _make_button("UI 100%", _on_scale_pressed)

	buttons.add_child(panic_button)
	buttons.add_child(restore_button)
	buttons.add_child(stop_button)
	buttons.add_child(go_live_button)
	buttons.add_child(edit_menu)
	buttons.add_child(debug_menu)
	setup_button = _make_button("Setup", _on_setup_pressed)
	buttons.add_child(setup_button)
	var button_spacer := Control.new()
	button_spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	buttons.add_child(button_spacer)
	buttons.add_child(scale_button)


func _build_setup() -> void:
	setup_window = SetupWindow.new()
	setup_window.name = "SetupWindow"
	root.add_child(setup_window)
	setup_window.configure(self, pipeline_controller, runtime, viewport)
	if setup_button != null:
		setup_button.text = "Setup"


func open_setup() -> void:
	if setup_window == null:
		return
	setup_window.open()
	if setup_button != null:
		setup_button.text = "Close setup"
	if live_session != null:
		notice = {}
		_update_hud(_now_ns())


func close_setup() -> void:
	if setup_window == null:
		return
	setup_window.close()
	if setup_button != null:
		setup_button.text = "Setup"
	_update_hud(_now_ns())


func get_setup_window():
	return setup_window


func _make_label(text: String, font_size: int) -> Label:
	var label := Label.new()
	label.text = text
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	label.custom_minimum_size = Vector2(0, font_size + 8)
	label.add_theme_font_size_override("font_size", font_size)
	label.add_theme_color_override("font_color", Color(0.95, 0.97, 1.0))
	label.add_theme_color_override("font_outline_color", Color(0.0, 0.0, 0.0, 0.9))
	label.add_theme_constant_override("outline_size", 3)
	return label


func _make_button(text: String, callback: Callable) -> Button:
	var button := Button.new()
	button.text = text
	button.focus_mode = Control.FOCUS_ALL
	button.add_theme_font_size_override("font_size", 16)
	button.pressed.connect(callback)
	return button


func _panel_style(alpha: float) -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.04, 0.06, 0.09, alpha)
	style.corner_radius_top_left = 8
	style.corner_radius_top_right = 8
	style.corner_radius_bottom_left = 8
	style.corner_radius_bottom_right = 8
	style.content_margin_left = 12
	style.content_margin_top = 8
	style.content_margin_right = 12
	style.content_margin_bottom = 8
	return style


func _start_live() -> void:
	if live_session == null:
		return
	var now := _now_ns()
	live_session.call("start_live", now)
	live_session.call("clear_debug_state")
	if pipeline_controller != null:
		if not pipeline_controller.start():
			push_warning("CAP-001: live pipeline did not start: "
					+ String(pipeline_controller.last_error))
	_init_output()
	if runtime != null:
		runtime.call("set_consumer_connected", true)
	notice = {}
	_update_hud(now)


func _stop_live() -> void:
	if live_session == null:
		return
	var now := _now_ns()
	live_session.call("stop_live", now)
	if pipeline_controller != null:
		pipeline_controller.stop()
	submit_pending = false
	if output_manager != null:
		output_manager.call("shutdown")
	if runtime != null:
		runtime.call("set_consumer_connected", false)
	if output_target != null:
		output_target.texture = null
	_show_notice("Live is stopped.", "Choose Go Live to start the output.", "", false, 0.0)
	_update_hud(now)


func _init_pipeline() -> void:
	# A headless shell smoke instantiates main.tscn; loading MediaPipe there
	# would print third-party warnings and open a camera for no reason. The
	# headless pipeline tests opt in with an explicit --live-source.
	if DisplayServer.get_name() == "headless" \
			and not ("--live-source" in OS.get_cmdline_user_args()):
		return
	pipeline_controller = PipelineController.new()
	var options := _pipeline_options()
	if not pipeline_controller.setup(runtime, options):
		push_warning("CAP-001: live pipeline setup failed: "
				+ String(pipeline_controller.last_error))


func _pipeline_options() -> Dictionary:
	var args := OS.get_cmdline_user_args()
	var source := _arg_value(args, "--live-source")
	if source == "":
		source = "camera"
	var tracker := _arg_value(args, "--live-tracker")
	if tracker == "":
		if source == "synthetic":
			tracker = "scripted"
		else:
			var dll := ProjectSettings.globalize_path("res://bin/libavatartracker.dll")
			if FileAccess.file_exists(dll):
				tracker = dll
	return {
		"source": source,
		"tracker": tracker,
		"camera": _arg_value(args, "--camera"),
		"tracker_input": 0,
		"fps": 60,
	}


func _feed_pipeline(state: Dictionary) -> void:
	if live_session == null or state.is_empty():
		return
	live_session.call("set_camera_status", String(state.get("camera", "none")))
	live_session.call("set_face_status", String(state.get("face", "unavailable")),
			float(state.get("confidence", 0.0)))
	live_session.call("set_tracker_available", bool(state.get("tracker_available", false)))


func _init_output() -> void:
	if live_session == null:
		return
	if output_manager == null:
		output_manager = ClassDB.instantiate("AvatarOutputManager")
		if output_manager == null:
			push_error("OUT-001: AvatarOutputManager is not registered")
			return
		live_session.call("bind_output", output_manager)
	var space := "linear" if RenderingServer.get_current_rendering_method() == "gl_compatibility" else "srgb"
	var ok: bool = output_manager.call("initialize", {
		"kind": "window",
		"width": OUTPUT_SIZE.x,
		"height": OUTPUT_SIZE.y,
		"alpha": "straight",
		"color_space": space,
		"source_alpha": "straight",
		"source_color_space": space,
		"target": output_target,
	})
	if not ok:
		push_warning("OUT-001: window output failed: " + String(output_manager.call("get_last_error")))


func _toggle_panic(reason: String) -> void:
	if live_session == null or not bool(live_session.call("is_live")):
		return
	live_session.call("toggle_panic", reason)
	notice = {}
	_update_hud(_now_ns())


func _submit_output(now_ns: int) -> void:
	if submit_pending:
		return
	if output_manager == null or not bool(output_manager.call("is_initialized")):
		return
	if viewport == null or DisplayServer.get_name() == "headless":
		return
	var texture := viewport.get_texture()
	if texture == null:
		return
	submit_pending = true
	_readback_and_submit(texture, now_ns)


func _readback_and_submit(texture: Texture2D, now_ns: int) -> void:
	# Read after the draw, not before it: a pre-draw get_image() can block on
	# the GPU and cost tens of milliseconds. This is still a synchronous
	# readback (T46 owns the async path), but it is off the frame's critical
	# section.
	await RenderingServer.frame_post_draw
	var start := Time.get_ticks_usec()
	var image := texture.get_image()
	last_submit_cost_ms = float(Time.get_ticks_usec() - start) / 1000.0
	max_submit_cost_ms = maxf(max_submit_cost_ms, last_submit_cost_ms)
	submit_pending = false
	if image == null or image.is_empty():
		return
	submit_output_image(image, now_ns)


func submit_output_image(image: Image, now_ns: int = -1) -> Dictionary:
	if output_manager == null or not bool(output_manager.call("is_initialized")):
		return {}
	if image == null or image.is_empty():
		return {}
	if now_ns < 0:
		now_ns = _now_ns()
	var frame := image
	if frame.get_width() != OUTPUT_SIZE.x or frame.get_height() != OUTPUT_SIZE.y:
		# `image` is our own readback copy; resize it in place instead of
		# duplicating an 8 MB 1080p buffer on every submit.
		frame.resize(OUTPUT_SIZE.x, OUTPUT_SIZE.y, Image.INTERPOLATE_BILINEAR)
	if frame.get_format() != Image.FORMAT_RGBA8:
		frame.convert(Image.FORMAT_RGBA8)
	var gate_signal: Dictionary = live_session.call("get_gate_signal", now_ns)
	var decision: Dictionary = output_manager.call("submit_frame", frame, gate_signal)
	live_session.call("set_output_state", String(decision.get("state", "blank")))
	if pipeline_controller != null:
		pipeline_controller.record_submit()
	return decision


func request_edit(kind: String) -> Dictionary:
	if live_session == null:
		return {}
	var decision: Dictionary = live_session.call("request_edit", kind)
	current_edit_kind = kind
	if bool(decision.get("allowed", false)):
		_show_notice(String(decision.get("sentence", "")), "", "", false, 3.0)
	else:
		_show_notice(String(decision.get("sentence", "")), String(decision.get("action", "")),
				"", true, 0.0)
	_update_hud(_now_ns())
	return decision


func stop_and_switch(kind: String) -> void:
	_stop_live()
	_show_notice("Stopped. You can change the %s now." % kind, "", "", false, 0.0)
	_update_hud(_now_ns())


func set_debug_state(name: String) -> void:
	if live_session == null:
		return
	live_session.call("set_debug_state", name)
	notice = {}
	_update_hud(_now_ns())


func set_debug_states_visible(visible: bool) -> void:
	debug_states_visible = visible
	if debug_menu != null:
		debug_menu.visible = visible


func cycle_ui_scale() -> float:
	ui_scale_index = (ui_scale_index + 1) % UI_SCALES.size()
	ui_scale = UI_SCALES[ui_scale_index]
	get_window().content_scale_factor = ui_scale
	if scale_button != null:
		scale_button.text = "UI %d%%" % int(round(ui_scale * 100.0))
	return ui_scale


func get_hud_snapshot() -> Dictionary:
	if live_session == null:
		return {}
	return live_session.call("get_hud", _now_ns())


func get_status() -> Dictionary:
	if live_session == null:
		return {}
	return live_session.call("get_status")


func get_pipeline_state() -> Dictionary:
	if pipeline_controller == null:
		return {}
	return pipeline_controller.get_state()


func get_pipeline_stats() -> Dictionary:
	if pipeline_controller == null:
		return {}
	return pipeline_controller.get_stats()


func get_pipeline_config() -> Dictionary:
	if pipeline_controller == null:
		return {}
	return pipeline_controller.get_config()


func get_build_info() -> Dictionary:
	if pipeline_controller == null:
		return {}
	return pipeline_controller.get_build_info()


func get_output_stats() -> Dictionary:
	if output_manager == null:
		return {}
	var stats: Dictionary = output_manager.call("get_stats")
	stats["last_submit_cost_ms"] = last_submit_cost_ms
	stats["max_submit_cost_ms"] = max_submit_cost_ms
	return stats


func reset_metrics() -> void:
	max_submit_cost_ms = 0.0
	last_submit_cost_ms = 0.0
	if pipeline_controller != null:
		pipeline_controller.reset_stats()


func is_live() -> bool:
	return live_session != null and bool(live_session.call("is_live"))


func is_panic() -> bool:
	return live_session != null and bool(live_session.call("is_panic"))


func _update_hud(now_ns: int) -> void:
	if live_session == null:
		return
	var hud: Dictionary = live_session.call("get_hud", now_ns)
	var status: Dictionary = hud.get("status", {})
	var live := bool(hud.get("live", false))
	var panic := bool(hud.get("panic", false))

	labels["live"].text = "LIVE" if live else "STOPPED"
	labels["elapsed"].text = _format_elapsed(float(hud.get("elapsed_s", 0.0)))
	labels["fps"].text = "%d fps" % int(round(float(hud.get("fps", 0.0))))
	labels["frame"].text = "%.1f ms" % float(hud.get("frame_time_ms", 0.0))

	var face_state := String(hud.get("face_state", "unavailable"))
	var confidence := float(hud.get("confidence", 0.0))
	labels["confidence"].text = "tracking -" if face_state == "unavailable" \
			else "tracking %d%%" % int(round(confidence * 100.0))
	labels["face"].text = "face " + face_state
	labels["output"].text = "output " + String(hud.get("output_state", "placeholder"))

	var hotkey: Dictionary = hud.get("global_hotkey", {})
	labels["global"].text = "Global: " + String(hotkey.get("detail", "unavailable"))

	if monitor_title != null:
		monitor_title.text = "Output (window, %s)" % String(hud.get("output_state", "placeholder"))

	var show_notice := not notice.is_empty() and (notice_until_usec == 0 or now_ns < notice_until_usec)
	if not show_notice and not notice.is_empty():
		notice = {}
	if show_notice:
		banner.visible = true
		banner_title.text = String(notice.get("sentence", ""))
		var notice_action := String(notice.get("action", ""))
		banner_action.text = ("-> " + notice_action) if notice_action != "" else ""
		banner_code.text = String(notice.get("code", ""))
		stop_and_switch_button.visible = bool(notice.get("show_stop", false))
	elif live and not bool(status.get("healthy", true)):
		banner.visible = true
		banner_title.text = String(status.get("sentence", ""))
		var status_action := String(status.get("action", ""))
		banner_action.text = ("-> " + status_action) if status_action != "" else ""
		banner_code.text = String(status.get("code", ""))
		stop_and_switch_button.visible = false
	else:
		banner.visible = false
		stop_and_switch_button.visible = false

	if output_panel != null:
		output_panel.visible = not banner.visible
	panic_button.disabled = panic or not live
	restore_button.visible = panic
	stop_button.visible = live
	go_live_button.visible = not live
	scale_button.text = "UI %d%%" % int(round(ui_scale * 100.0))


func _on_panic_pressed() -> void:
	_toggle_panic("button")


func _on_restore_pressed() -> void:
	_toggle_panic("button")


func _on_stop_pressed() -> void:
	_stop_live()


func _on_go_live_pressed() -> void:
	_start_live()


func _on_edit_selected(id: int) -> void:
	if id >= 0 and id < EDIT_KINDS.size():
		request_edit(EDIT_KINDS[id])


func _on_debug_selected(id: int) -> void:
	if id >= 0 and id < DEBUG_STATES.size():
		set_debug_state(DEBUG_STATES[id])


func _on_scale_pressed() -> void:
	cycle_ui_scale()


func _on_setup_pressed() -> void:
	if setup_window != null and bool(setup_window.is_open()):
		close_setup()
	else:
		open_setup()


func _on_stop_and_switch_pressed() -> void:
	stop_and_switch(current_edit_kind)


func _show_notice(sentence: String, action: String, code: String, show_stop: bool,
		seconds: float) -> void:
	notice = {
		"sentence": sentence,
		"action": action,
		"code": code,
		"show_stop": show_stop,
	}
	notice_until_usec = 0 if seconds <= 0.0 else _now_ns() + int(seconds * 1_000_000.0)


func _format_elapsed(seconds: float) -> String:
	var total := int(max(0.0, seconds))
	return "%02d:%02d:%02d" % [int(total / 3600), int((total % 3600) / 60), total % 60]


func _now_ns() -> int:
	return int(Time.get_ticks_usec()) * 1000
