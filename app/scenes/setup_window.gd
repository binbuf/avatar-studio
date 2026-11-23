extends Control

## T25c Setup window: one opaque diagnostic surface with three live panes.
##
##   Camera        raw T24 ring frame + landmarks, face box, homography, anchors
##   Mapping       canonical-face values the decal actually uses
##   Final output  the avatar + decal with no overlays, white/black background
##
## The bottom controls move the eye/mouth/face/head mapping live, apply and
## persist the mapping to `user://live_calibration.json`, reset it, and choose
## the capture mode and tracker input. The OBS output is a separate window and
## never renders this surface.

const Calibration = preload("res://scenes/live_calibration.gd")
const CameraView = preload("res://scenes/setup_camera_view.gd")
const MappingView = preload("res://scenes/setup_mapping_view.gd")
const ModelView = preload("res://scenes/setup_model_view.gd")

const BACKGROUND := Color(0.020, 0.030, 0.045, 1.0)
const PANEL := Color(0.045, 0.060, 0.085, 1.0)
const TEXT := Color(0.92, 0.95, 1.0, 1.0)
const MUTED := Color(0.62, 0.70, 0.82, 1.0)
const OK_COLOUR := Color(0.55, 1.0, 0.65, 1.0)
const WARN_COLOUR := Color(1.0, 0.78, 0.30, 1.0)
const REGION_IDS := ["eye_l", "eye_r", "mouth"]
const CAPTURE_MODES := [
	{"label": "Auto (preferred)", "width": 0, "height": 0, "fps": 0},
	{"label": "1280x720@30", "width": 1280, "height": 720, "fps": 30},
	{"label": "640x480@30", "width": 640, "height": 480, "fps": 30},
	{"label": "1920x1080@30", "width": 1920, "height": 1080, "fps": 30},
]
const TRACKER_INPUTS := [192, 256]

var hud: CanvasLayer = null
var controller = null
var runtime: Node = null
var viewport: SubViewport = null

var camera_view = null
var mapping_view = null
var model_view = null
var final_frame: Control = null
var final_background: ColorRect = null
var final_avatar: TextureRect = null
var final_note: Label = null
var status_panel: PanelContainer = null
var controls_panel: PanelContainer = null
var fine_tune_scroll: ScrollContainer = null
var split_outer: HSplitContainer = null
var split_inner: HSplitContainer = null
var pane_footers: Array = []
var pane_titles: Array = []
var status_extra_labels: Array = []
var control_section_labels: Array = []
var compact := false
var labels: Dictionary = {}
var mapping_text: Label = null
var model_header_label: Label = null
var action_status: Label = null
var background_option: OptionButton = null
var capture_option: OptionButton = null
var tracker_option: OptionButton = null
var preview_check: CheckButton = null
var file_dialog: FileDialog = null
var sliders: Dictionary = {}
var draw_buttons: Dictionary = {}
var selected_region := "eye_l"

var base: Dictionary = {}
var working: Dictionary = {}
var params: Dictionary = {}
var base_face_scale := 1.0
var base_face_origin := Vector3.ZERO
var background := "black"
var opened := false
var last_apply: Dictionary = {}
var last_error := ""

var _overlay_timer := 0.0
var _status_timer := 0.0
var _identity_homography := PackedFloat32Array([1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0])
# Last good live mapping. When the rig has no valid solve the pipeline reports
# an identity homography, which would park the cutouts at a bogus fixed place.
# Hold the last good homography and anchors instead, and flag the panes so the
# operator can see the mapping is held rather than tracking.
var _held_homography := PackedFloat32Array()
var _held_centroids: Array = []
var _held_rest: Array = []
var _held_anchors: Array = []
var _display_anchors: Array = []


func configure(hud_node: CanvasLayer, controller_node, runtime_node: Node,
		viewport_node: SubViewport) -> void:
	hud = hud_node
	controller = controller_node
	runtime = runtime_node
	viewport = viewport_node
	_build_ui()
	_build_file_dialog()
	if controller != null:
		base = controller.get_calibration()
	_reset_params_from_base()
	set_process(false)


func open() -> void:
	opened = true
	visible = true
	set_process(true)
	if controller != null:
		controller.set_preview_enabled(true)
		base = controller.get_calibration()
		# Keep the launch source: the human review opens the real camera, while
		# an unattended session launched with --live-source still keeps the file.
		var config: Dictionary = controller.get_config()
		if String(config.get("source", "")) == "still" \
				and String(config.get("still_image", "")) == "" \
				and hud != null and hud.has_method("use_default_still"):
			hud.call("use_default_still")
	_reset_params_from_base()
	_set_all_draw_modes("")
	if viewport != null and final_avatar != null:
		final_avatar.texture = viewport.get_texture()
	_overlay_timer = 0.0
	_status_timer = 0.0
	_refresh(true)


func close() -> void:
	opened = false
	visible = false
	set_process(false)
	_set_all_draw_modes("")
	if hud != null and hud.has_method("use_camera"):
		hud.call("use_camera")
	if controller != null:
		controller.set_preview_enabled(false)


func is_open() -> bool:
	return opened


func _process(delta: float) -> void:
	if not opened:
		return
	_apply_compact_layout()
	_overlay_timer += delta
	_status_timer += delta
	# Match the camera rate so the cutouts follow the face without visible lag.
	if _overlay_timer >= 1.0 / 30.0:
		_overlay_timer = 0.0
		_refresh_overlays()
	if _status_timer >= 0.25:
		_status_timer = 0.0
		_refresh_status()


## At 150-200% UI scale the effective window can be 640x360 or smaller. Keep the
## three panes usable by hiding the secondary pane notes and shrinking the
## control strip; the controls still scroll inside their panel.
func _apply_compact_layout() -> void:
	var window := get_window()
	if window == null:
		return
	var logical_width := float(window.size.x) / maxf(window.content_scale_factor, 0.01)
	var want := logical_width < 800.0
	if want == compact:
		return
	compact = want
	_apply_pane_ratios()
	for footer in pane_footers:
		if footer is Control:
			footer.visible = not compact
	for title in pane_titles:
		if title is Label:
			title.add_theme_font_size_override("font_size", 11 if compact else 14)
			title.autowrap_mode = TextServer.AUTOWRAP_OFF if compact \
					else TextServer.AUTOWRAP_WORD_SMART
			title.clip_text = compact
	for label in status_extra_labels:
		if label is Label:
			label.visible = not compact
	for label in control_section_labels:
		if label is Control:
			label.visible = not compact
	if background_option != null:
		background_option.add_theme_font_size_override("font_size", 11 if compact else 14)
	if action_status != null:
		action_status.visible = not compact
	if fine_tune_scroll != null:
		fine_tune_scroll.visible = not compact
	if controls_panel != null:
		controls_panel.custom_minimum_size = Vector2(0, 64 if compact else 240)


## SplitContainer's default split position follows the children's expand flags
## and stretch ratios. Setting the ratios (rather than split_offset) keeps the
## panes proportional at every window size while leaving the dividers draggable.
func _apply_pane_ratios() -> void:
	if split_outer != null and split_outer.get_child_count() == 2:
		split_outer.get_child(0).size_flags_stretch_ratio = 0.24 if compact else 0.30
		split_outer.get_child(1).size_flags_stretch_ratio = 0.76 if compact else 0.70
	if split_inner != null and split_inner.get_child_count() == 2:
		split_inner.get_child(0).size_flags_stretch_ratio = 0.38 if compact else 0.42
		split_inner.get_child(1).size_flags_stretch_ratio = 0.62 if compact else 0.58


func _refresh(force: bool) -> void:
	_refresh_overlays()
	_refresh_status()
	if force and viewport != null and final_avatar != null:
		final_avatar.texture = viewport.get_texture()


func _refresh_overlays() -> void:
	if controller == null:
		return
	var state: Dictionary = controller.last_state
	if state.is_empty():
		state = controller.get_state()
	var values: Dictionary = controller.get_calibration()
	var regions: Array = values.get("regions", [])
	var homography: PackedFloat32Array = state.get("canonical_to_image",
			PackedFloat32Array())
	var centroids: Array = state.get("regions", [])
	var rest: Array = state.get("region_rest", [])
	var anchors := _region_anchors(state)
	var has_solve := homography.size() == 9 and homography != _identity_homography
	var mapping_mode := "none"
	if has_solve:
		_held_homography = homography
		_held_centroids = centroids.duplicate(true)
		_held_rest = rest.duplicate(true)
		_held_anchors = anchors.duplicate(true)
		mapping_mode = "live" if bool(state.get("features_valid", false)) else "held"
	elif _held_homography.size() == 9:
		# Keep the cutouts on the last tracked face instead of the identity
		# projection the rig reports while it has no solve.
		homography = _held_homography
		centroids = _held_centroids.duplicate(true)
		rest = _held_rest.duplicate(true)
		anchors = _held_anchors.duplicate(true)
		mapping_mode = "held"
	else:
		# Development stills and the first frames can have a tracked face but no
		# rig solve yet. A face-box stand-in keeps the setup UI usable; the pane
		# labels it and draws the cutouts dashed so it is never mistaken for a
		# calibrated mapping.
		var fallback := _face_box_homography(controller.get_face_box())
		if fallback.size() == 9:
			homography = fallback
			# No rig anchor without a solve; place the authored centres on the
			# face box. The tracker centroids are still drawn as anchor marks.
			anchors = []
			for index in range(REGION_IDS.size()):
				anchors.append(Vector2.ZERO)
			mapping_mode = "fallback"
	_display_anchors = anchors.duplicate(true)
	var effective_regions: Array = []
	for index in range(regions.size()):
		var entry: Dictionary = regions[index].duplicate(true)
		var centre: Vector2 = regions[index].get("center", Vector2.ZERO)
		if index < anchors.size():
			centre += Vector2(anchors[index])
		entry.center = centre
		effective_regions.append(entry)
	if camera_view != null:
		camera_view.update_slot(controller.get_preview_slot())
		camera_view.set_overlay(controller.get_landmarks(), controller.get_face_box(),
				homography, centroids, effective_regions, selected_region, mapping_mode)
	var display_state := state.duplicate(true)
	display_state["canonical_to_image"] = homography
	display_state["regions"] = centroids
	display_state["region_rest"] = rest
	if mapping_view != null:
		mapping_view.set_data(values, display_state)
		mapping_view.set_selected(selected_region)
	if model_view != null:
		model_view.set_data(values.get("face", {}), regions, anchors, controller.model,
				_get_avatar_camera(), _get_viewport_size(), selected_region)
	_update_mapping_text(display_state)


## Per-region FaceRig anchors: the live centroid minus its rest value. The decal
## shader adds these to the authored region rect, so the model outline must too.
func _region_anchors(state: Dictionary) -> Array:
	var out: Array = []
	var regions: Array = state.get("regions", [])
	var rest: Array = state.get("region_rest", [])
	for index in range(REGION_IDS.size()):
		if index < regions.size() and index < rest.size():
			out.append(Vector2(regions[index]) - Vector2(rest[index]))
		else:
			out.append(Vector2.ZERO)
	return out


## Stand-in canonical -> image homography from the tracker face box. Canonical
## (0,0) maps to the box centre, +u to image right and +v to image up. Used only
## when the rig has no solve yet, so the setup UI stays usable on a still or the
## first frames; the camera pane labels and dashes the cutouts in that state.
static func _face_box_homography(box: Rect2) -> PackedFloat32Array:
	if box.size.x <= 0.001 or box.size.y <= 0.001:
		return PackedFloat32Array()
	var cx := box.position.x + box.size.x * 0.5
	var cy := box.position.y + box.size.y * 0.5
	return PackedFloat32Array([box.size.x * 0.5, 0.0, cx, 0.0, -box.size.y * 0.5, cy,
			0.0, 0.0, 1.0])


func _get_avatar_camera() -> Camera3D:
	if viewport == null:
		return null
	return viewport.get_node_or_null("AvatarCamera") as Camera3D


func _get_viewport_size() -> Vector2i:
	if viewport != null:
		return viewport.size
	return Vector2i(1920, 1080)


func _refresh_status() -> void:
	if hud == null or controller == null:
		return
	var state: Dictionary = controller.last_state
	if state.is_empty():
		state = controller.get_state()
	var snapshot: Dictionary = hud.call("get_hud_snapshot")
	var status: Dictionary = hud.call("get_status")
	var config: Dictionary = controller.get_config()

	_set_label("camera", "cam " + String(state.get("camera", "unknown")))
	var tracker := String(config.get("tracker", "none"))
	var tracker_ready := bool(state.get("tracker_available", false))
	_set_label("tracker", "trk %s %s" % [tracker,
			"ready" if tracker_ready else "unavailable"])
	var face := String(state.get("face", "unavailable"))
	var confidence := float(state.get("confidence", 0.0))
	_set_label("face", "face " + face)
	_set_label("confidence", "conf %d%%" % int(round(confidence * 100.0)))
	_set_label("fps", "%d fps" % int(round(float(snapshot.get("fps", 0.0)))))
	_set_label("capture", "%dx%d@%d %s" % [int(config.get("width", 0)),
			int(config.get("height", 0)), int(round(float(config.get("fps", 0.0)))),
			String(config.get("format", "?"))])

	var features := bool(state.get("features_valid", false))
	var have_output := bool(state.get("have_output", false))
	var is_frozen := bool(state.get("frozen", false)) or bool(controller.is_frozen())
	var source_label := "camera"
	var source_path := ""
	if controller != null:
		source_label = String(controller.get_source_label())
		source_path = String(controller.get_source_path())
	if source_label == "still (file)" and source_path != "":
		source_label += " " + source_path
	_set_label("source", "source " + source_label)
	_set_label("features", "features %s" % str(features))
	_set_label("have_output", "output %s" % str(have_output))
	var hold := features and have_output and tracker_ready \
			and (face == "not_visible" or face == "features_hidden")
	var hold_label: Label = labels["hold"]
	if is_frozen:
		hold_label.text = "FROZEN (still dev)"
		hold_label.add_theme_color_override("font_color", WARN_COLOUR)
	else:
		hold_label.text = "HOLD (150 ms loss window)" if hold else ""
		hold_label.add_theme_color_override("font_color", WARN_COLOUR if hold else MUTED)

	var code := String(status.get("code", ""))
	var sentence := String(status.get("sentence", ""))
	var reason_label: Label = labels["reason"]
	reason_label.text = ("%s  %s" % [code, sentence]).strip_edges()
	reason_label.add_theme_color_override("font_color",
			WARN_COLOUR if code != "" and code != "OK" else MUTED)
	var hint_label: Label = labels["hint"]
	if is_frozen:
		reason_label.text = "FROZEN still (dev): Setup holds the last frame and pose; the OBS output is blank."
		reason_label.add_theme_color_override("font_color", WARN_COLOUR)
		hint_label.visible = false
	elif code == "TRK-004":
		hint_label.text = "Low light hint: " + String(status.get("action", ""))
		hint_label.visible = true
	else:
		hint_label.visible = false

	var final_note_text := "No overlays. OBS output is separate."
	if is_frozen:
		final_note_text = "Frozen diagnostic: this pane holds the avatar; the OBS output is blank."
	elif not features:
		final_note_text = "Decal blank: features_valid is false (the OBS gate blanks too)."
	elif not have_output:
		final_note_text = "Decal blank: no FaceRig output yet."
	elif face != "tracked" and face != "degraded":
		final_note_text = "Holding the last FaceRig pose; the OBS gate blanks on the loss."
	if final_note != null:
		final_note.text = final_note_text
		final_note.add_theme_color_override("font_color", MUTED)
	_sync_selected_buttons()


func _set_label(key: String, text: String) -> void:
	if labels.has(key):
		labels[key].text = text


func _update_mapping_text(state: Dictionary) -> void:
	if mapping_text == null:
		return
	var values: Dictionary = controller.get_calibration() if controller != null else {}
	if values.is_empty():
		mapping_text.text = "no calibration"
		return
	var h: PackedFloat32Array = state.get("canonical_to_image", PackedFloat32Array())
	var h_text := "H: -"
	if h.size() == 9:
		h_text = "H: [%.2f %.2f %.2f | %.2f %.2f %.2f | %.3f %.3f %.3f]" % [
				h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8]]
	var region_text := ""
	for region in values.get("regions", []):
		var centre: Vector2 = region.get("center", Vector2.ZERO)
		var size: Vector2 = region.get("size", Vector2.ZERO)
		region_text += "%s(%.2f,%.2f %.2fx%.2f) " % [String(region.get("id", "?")),
				centre.x, centre.y, size.x, size.y]
	mapping_text.text = "%s\nface scale %.3f  head cut %.2f  falloff %.2f\n%s" % [
			h_text, float(values.face.scale), Calibration.head_distance(values),
			Calibration.head_falloff(values), region_text]


func _build_ui() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_STOP
	visible = false

	var background_rect := ColorRect.new()
	background_rect.color = BACKGROUND
	background_rect.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	background_rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(background_rect)

	var margin := MarginContainer.new()
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	margin.add_theme_constant_override("margin_left", 12)
	margin.add_theme_constant_override("margin_top", 10)
	margin.add_theme_constant_override("margin_right", 12)
	margin.add_theme_constant_override("margin_bottom", 10)
	add_child(margin)

	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 6)
	margin.add_child(column)

	_build_status(column)
	_build_panes(column)
	_build_controls(column)


func _build_status(parent: Control) -> void:
	var panel := PanelContainer.new()
	panel.name = "SetupStatus"
	panel.add_theme_stylebox_override("panel", _panel_style(0.90))
	parent.add_child(panel)
	status_panel = panel
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 2)
	panel.add_child(box)

	var row_one := HBoxContainer.new()
	row_one.add_theme_constant_override("separation", 14)
	box.add_child(row_one)
	for key in ["camera", "tracker", "face", "confidence", "fps", "capture"]:
		labels[key] = _label("", 11, MUTED)
		labels[key].size_flags_horizontal = Control.SIZE_EXPAND_FILL
		labels[key].clip_text = true
		row_one.add_child(labels[key])

	var row_two := HBoxContainer.new()
	row_two.add_theme_constant_override("separation", 8)
	box.add_child(row_two)
	for key in ["features", "have_output", "hold", "source"]:
		labels[key] = _label("", 10, MUTED)
		labels[key].size_flags_horizontal = Control.SIZE_EXPAND_FILL
		labels[key].clip_text = true
		row_two.add_child(labels[key])
	labels["reason"] = _label("", 10, MUTED)
	labels["reason"].size_flags_horizontal = Control.SIZE_EXPAND_FILL
	labels["reason"].autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	row_two.add_child(labels["reason"])
	labels["hint"] = _label("", 10, WARN_COLOUR)
	labels["hint"].size_flags_horizontal = Control.SIZE_EXPAND_FILL
	labels["hint"].autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	labels["hint"].visible = false
	row_two.add_child(labels["hint"])
	status_extra_labels.append(labels["reason"])
	status_extra_labels.append(labels["hint"])


func _build_panes(parent: Control) -> void:
	split_outer = HSplitContainer.new()
	split_outer.name = "SetupPanes"
	split_outer.add_theme_constant_override("separation", 6)
	split_outer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	split_outer.size_flags_vertical = Control.SIZE_EXPAND_FILL
	parent.add_child(split_outer)

	# Camera pane: the live feed and the draw-a-shape interaction.
	var camera_pane := _pane("Camera", 0.30)
	split_outer.add_child(camera_pane)
	var camera_header: HBoxContainer = camera_pane.get_meta("header")
	var draw_group := ButtonGroup.new()
	draw_group.allow_unpress = true
	for id in REGION_IDS:
		var button := Button.new()
		button.name = "Draw" + String(id)
		button.text = _region_button_name(id)
		button.toggle_mode = true
		button.button_group = draw_group
		button.add_theme_font_size_override("font_size", 11)
		button.toggled.connect(_on_draw_toggled.bind(id))
		camera_header.add_child(button)
		draw_buttons[id] = button
	camera_view = CameraView.new()
	camera_view.name = "CameraView"
	camera_view.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	camera_view.region_drawn.connect(_on_region_drawn)
	camera_view.region_selected.connect(_on_region_selected)
	camera_view.region_dragged.connect(_on_region_dragged)
	camera_view.region_resized.connect(_on_region_resized)
	var camera_content: Control = camera_pane.get_meta("content")
	camera_content.add_child(camera_view)
	var camera_note := _label(
			"Draw, move or scale on the camera, mapping or model.",
			11, MUTED)
	camera_note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	var camera_footer: VBoxContainer = camera_pane.get_meta("footer")
	camera_footer.add_child(camera_note)

	# Mapping pane: the canonical-face values the decal actually uses.
	split_inner = HSplitContainer.new()
	split_inner.add_theme_constant_override("separation", 6)
	split_inner.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	split_inner.size_flags_vertical = Control.SIZE_EXPAND_FILL
	split_inner.size_flags_stretch_ratio = 0.70
	split_outer.add_child(split_inner)
	var mapping_pane := _pane("Mapping (canonical face)", 0.42)
	split_inner.add_child(mapping_pane)
	mapping_view = MappingView.new()
	mapping_view.name = "MappingView"
	mapping_view.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mapping_view.region_drawn.connect(_on_region_drawn)
	mapping_view.region_selected.connect(_on_region_selected)
	mapping_view.region_dragged.connect(_on_region_dragged)
	mapping_view.region_resized.connect(_on_region_resized)
	var mapping_content: Control = mapping_pane.get_meta("content")
	mapping_content.add_child(mapping_view)
	mapping_text = _label("", 10, MUTED)
	mapping_text.name = "MappingValues"
	mapping_text.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	mapping_text.visible = false
	var mapping_footer: VBoxContainer = mapping_pane.get_meta("footer")
	mapping_footer.add_child(mapping_text)

	# Final output pane: the avatar with draggable cutout outlines.
	var final_pane := _pane("Model - drag cutouts", 0.58)
	split_inner.add_child(final_pane)
	var final_header: HBoxContainer = final_pane.get_meta("header")
	model_header_label = _label("", 11, MUTED)
	model_header_label.name = "SelectedRegion"
	final_header.add_child(model_header_label)
	background_option = OptionButton.new()
	background_option.name = "Background"
	background_option.add_item("White", 0)
	background_option.add_item("Black", 1)
	background_option.selected = 1
	background_option.item_selected.connect(_on_background_selected)
	final_header.add_child(background_option)

	final_frame = Control.new()
	final_frame.name = "FinalFrame"
	final_frame.clip_contents = true
	final_frame.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	var final_content: Control = final_pane.get_meta("content")
	final_content.add_child(final_frame)
	final_background = ColorRect.new()
	final_background.color = Color.BLACK
	final_background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	final_background.mouse_filter = Control.MOUSE_FILTER_IGNORE
	final_frame.add_child(final_background)
	final_avatar = TextureRect.new()
	final_avatar.name = "FinalAvatar"
	final_avatar.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	final_avatar.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	final_avatar.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	final_avatar.mouse_filter = Control.MOUSE_FILTER_IGNORE
	final_frame.add_child(final_avatar)
	model_view = ModelView.new()
	model_view.name = "ModelView"
	model_view.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	model_view.region_selected.connect(_on_region_selected)
	model_view.region_dragged.connect(_on_region_dragged)
	model_view.region_drawn.connect(_on_region_drawn)
	model_view.region_resized.connect(_on_region_resized)
	final_frame.add_child(model_view)
	final_note = _label(
			"Draw a box, drag a cutout, or drag the handle to scale. OBS output is separate.",
			11, MUTED)
	final_note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	var final_footer: VBoxContainer = final_pane.get_meta("footer")
	final_footer.add_child(final_note)


func _pane(title: String, ratio: float) -> PanelContainer:
	var panel := PanelContainer.new()
	panel.add_theme_stylebox_override("panel", _panel_style(0.86))
	panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	panel.size_flags_stretch_ratio = ratio
	panel.clip_contents = true
	panel.custom_minimum_size = Vector2(0, 0)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 3)
	panel.add_child(box)
	var header := HBoxContainer.new()
	box.add_child(header)
	var title_label := _label(title, 14, TEXT)
	title_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	title_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	title_label.custom_minimum_size = Vector2(0, 0)
	header.add_child(title_label)
	pane_titles.append(title_label)
	var content := Control.new()
	content.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.add_child(content)
	var footer := VBoxContainer.new()
	box.add_child(footer)
	pane_footers.append(footer)
	panel.set_meta("header", header)
	panel.set_meta("content", content)
	panel.set_meta("footer", footer)
	return panel


func _build_controls(parent: Control) -> void:
	var panel := PanelContainer.new()
	panel.name = "SetupControls"
	panel.add_theme_stylebox_override("panel", _panel_style(0.90))
	panel.custom_minimum_size = Vector2(0, 240)
	parent.add_child(panel)
	controls_panel = panel
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 4)
	panel.add_child(box)

	# The primary actions stay visible even when the fine-tune area scrolls.
	var actions := HFlowContainer.new()
	actions.add_theme_constant_override("h_separation", 8)
	actions.add_theme_constant_override("v_separation", 2)
	box.add_child(actions)
	var apply_button := _button("Apply to model", _on_apply_pressed)
	apply_button.name = "ApplyToModel"
	actions.add_child(apply_button)
	var reset_button := _button("Reset", _on_reset_pressed)
	reset_button.name = "ResetCalibration"
	actions.add_child(reset_button)
	var snapshot_button := _button("Snapshot", _on_snapshot_pressed)
	snapshot_button.name = "Snapshot"
	actions.add_child(snapshot_button)
	var load_button := _button("Load still...", _on_load_still_pressed)
	load_button.name = "LoadStill"
	actions.add_child(load_button)
	var camera_button := _button("Use camera", _on_use_camera_pressed)
	camera_button.name = "UseCamera"
	actions.add_child(camera_button)
	var close_button := _button("Back to Live", _on_close_pressed)
	close_button.name = "CloseSetup"
	actions.add_child(close_button)
	action_status = _label("", 11, OK_COLOUR)
	action_status.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	action_status.custom_minimum_size = Vector2(220, 0)
	action_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	actions.add_child(action_status)

	# Fine tune: every slider lives in a scroll area so the panel never clips.
	fine_tune_scroll = ScrollContainer.new()
	fine_tune_scroll.name = "FineTune"
	fine_tune_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	fine_tune_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	fine_tune_scroll.custom_minimum_size = Vector2(0, 120)
	box.add_child(fine_tune_scroll)
	var controls := VBoxContainer.new()
	controls.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	controls.add_theme_constant_override("separation", 3)
	fine_tune_scroll.add_child(controls)

	var region_label := _label("Fine tune: eye L / eye R / mouth", 11, MUTED)
	controls.add_child(region_label)
	control_section_labels.append(region_label)
	var region_grid := _grid()
	controls.add_child(region_grid)
	for id in REGION_IDS:
		var prefix := String(id).replace("_", " ").capitalize()
		_add_slider(region_grid, "center_%s_x" % id, prefix + " u", -1.5, 1.5, 0.01, 0.0)
		_add_slider(region_grid, "center_%s_y" % id, prefix + " v", -1.8, 1.5, 0.01, 0.0)
		_add_slider(region_grid, "size_%s_x" % id, prefix + " w", 0.05, 1.5, 0.01, 0.1)
		_add_slider(region_grid, "size_%s_y" % id, prefix + " h", 0.05, 1.5, 0.01, 0.1)

	var face_label := _label("Face and head cut", 11, MUTED)
	controls.add_child(face_label)
	control_section_labels.append(face_label)
	var face_grid := _grid()
	controls.add_child(face_grid)
	_add_slider(face_grid, "face_scale", "face scale", 0.5, 2.0, 0.01, 1.0)
	_add_slider(face_grid, "face_u", "face u", -0.5, 0.5, 0.01, 0.0)
	_add_slider(face_grid, "face_v", "face v", -0.5, 0.5, 0.01, 0.0)
	_add_slider(face_grid, "head_distance", "head cut", 0.2, 3.0, 0.01, 1.5)
	_add_slider(face_grid, "head_falloff", "falloff", 0.1, 2.0, 0.01, 1.0)

	var inputs_label := _label("Capture and tracker input", 11, MUTED)
	controls.add_child(inputs_label)
	control_section_labels.append(inputs_label)
	var inputs := HFlowContainer.new()
	inputs.add_theme_constant_override("h_separation", 8)
	inputs.add_theme_constant_override("v_separation", 2)
	controls.add_child(inputs)
	capture_option = OptionButton.new()
	capture_option.name = "CaptureMode"
	for mode in CAPTURE_MODES:
		capture_option.add_item(String(mode.label))
	capture_option.item_selected.connect(_on_capture_selected)
	inputs.add_child(_label("capture", 11, MUTED))
	inputs.add_child(capture_option)

	tracker_option = OptionButton.new()
	tracker_option.name = "TrackerInput"
	for value in TRACKER_INPUTS:
		tracker_option.add_item(str(value))
	tracker_option.item_selected.connect(_on_tracker_selected)
	inputs.add_child(_label("tracker input", 11, MUTED))
	inputs.add_child(tracker_option)

	preview_check = CheckButton.new()
	preview_check.name = "CameraPreview"
	preview_check.text = "Camera preview"
	preview_check.button_pressed = true
	preview_check.toggled.connect(_on_preview_toggled)
	inputs.add_child(preview_check)


func _grid() -> GridContainer:
	var grid := GridContainer.new()
	grid.columns = 9
	grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	grid.add_theme_constant_override("h_separation", 8)
	grid.add_theme_constant_override("v_separation", 2)
	return grid


func _add_slider(grid: GridContainer, key: String, label_text: String, minimum: float,
		maximum: float, step: float, value: float) -> void:
	var label := _label(label_text, 11, MUTED)
	label.custom_minimum_size = Vector2(64, 0)
	grid.add_child(label)
	var slider := HSlider.new()
	slider.min_value = minimum
	slider.max_value = maximum
	slider.step = step
	slider.value = value
	slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	slider.custom_minimum_size = Vector2(60, 0)
	slider.value_changed.connect(_on_slider_changed.bind(key))
	grid.add_child(slider)
	var readout := _label("%.2f" % value, 11, TEXT)
	readout.custom_minimum_size = Vector2(42, 0)
	grid.add_child(readout)
	sliders[key] = {"slider": slider, "label": readout}


func _label(text: String, font_size: int, colour: Color) -> Label:
	var label := Label.new()
	label.text = text
	label.add_theme_font_size_override("font_size", font_size)
	label.add_theme_color_override("font_color", colour)
	label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return label


func _button(text: String, callback: Callable) -> Button:
	var button := Button.new()
	button.text = text
	button.add_theme_font_size_override("font_size", 13)
	button.pressed.connect(callback)
	return button


func _panel_style(alpha: float) -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = Color(PANEL.r, PANEL.g, PANEL.b, alpha)
	style.corner_radius_top_left = 6
	style.corner_radius_top_right = 6
	style.corner_radius_bottom_left = 6
	style.corner_radius_bottom_right = 6
	style.content_margin_left = 8
	style.content_margin_top = 6
	style.content_margin_right = 8
	style.content_margin_bottom = 6
	return style


func _reset_params_from_base() -> void:
	if base.is_empty() and controller != null:
		base = controller.get_calibration()
	if base.is_empty():
		return
	base_face_scale = float(base.face.scale)
	base_face_origin = Vector3(base.face.origin)
	params = {
		"face_scale": 1.0,
		"face_u": 0.0,
		"face_v": 0.0,
		"head_distance": Calibration.head_distance(base),
		"head_falloff": Calibration.head_falloff(base),
	}
	for region in base.regions:
		var id := String(region.id)
		var centre: Vector2 = region.center
		var size: Vector2 = region.size
		params["center_%s_x" % id] = centre.x
		params["center_%s_y" % id] = centre.y
		params["size_%s_x" % id] = size.x
		params["size_%s_y" % id] = size.y
	_sync_sliders()
	working = base.duplicate(true)
	_sync_selected_buttons()


func _sync_sliders() -> void:
	for key in sliders.keys():
		if not params.has(key):
			continue
		var entry: Dictionary = sliders[key]
		var slider: HSlider = entry.slider
		var readout: Label = entry.label
		slider.set_value_no_signal(float(params[key]))
		readout.text = "%.2f" % float(params[key])


func _rebuild_values() -> Dictionary:
	if base.is_empty():
		return {}
	var next := base.duplicate(true)
	var face: Dictionary = next.face
	var normal: Vector3 = Vector3(face.normal).normalized()
	var up: Vector3 = Vector3(face.up).normalized()
	var right := normal.cross(up).normalized()
	var scale := maxf(base_face_scale * float(params.face_scale), 0.001)
	face.scale = scale
	face.origin = base_face_origin + (right * float(params.face_u) + up * float(params.face_v)) * scale
	next.face = face
	next.head.cut.origin = Vector3(face.origin) - up * scale * maxf(float(params.head_distance), 0.05)
	next.head.cut.normal = up
	next.head.cut.falloff = maxf(scale * float(params.head_falloff), 0.001)
	for region in next.regions:
		var id := String(region.id)
		region.center = Vector2(float(params.get("center_%s_x" % id, 0.0)),
				float(params.get("center_%s_y" % id, 0.0)))
		region.size = Vector2(float(params.get("size_%s_x" % id, 0.1)),
				float(params.get("size_%s_y" % id, 0.1)))
	return next


func _on_slider_changed(value: float, key: String) -> void:
	params[key] = value
	if sliders.has(key):
		var entry: Dictionary = sliders[key]
		var readout: Label = entry.label
		readout.text = "%.2f" % value
	working = _rebuild_values()
	if working.is_empty():
		return
	var head_changed := key.begins_with("face_") or key.begins_with("head_")
	var result: Dictionary = controller.apply_calibration(working, head_changed)
	if not bool(result.get("ok", false)):
		last_error = String(result.get("error", "calibration rejected"))
		action_status.text = last_error
		action_status.add_theme_color_override("font_color", WARN_COLOUR)
		return
	last_error = ""
	action_status.text = "Live mapping updated (not saved)"
	action_status.add_theme_color_override("font_color", MUTED)
	_refresh_overlays()


func _on_draw_toggled(pressed: bool, id: String) -> void:
	if pressed:
		selected_region = id
		_set_all_draw_modes(id)
		_set_action("Draw " + _region_name(id)
				+ ": drag on the camera, mapping or model.")
	else:
		_set_all_draw_modes("")
	_refresh_overlays()


func _set_all_draw_modes(id: String) -> void:
	if camera_view != null:
		camera_view.set_draw_mode(id)
	if mapping_view != null:
		mapping_view.set_draw_mode(id)
	if model_view != null:
		model_view.set_draw_mode(id)


func _on_region_selected(id: String) -> void:
	selected_region = id
	# Selection happens with no draw mode active, so do not clear the modes
	# here: that would reset the pane's in-progress drag state.
	_sync_selected_buttons()
	_refresh_overlays()


func _on_region_dragged(id: String, center: Vector2) -> void:
	# `center` is the effective canonical point (authored + FaceRig anchor);
	# the persisted region is the authored point.
	_apply_region_values(id, _authored_center(id, center), Vector2.ZERO,
			"Moved " + _region_name(id), false)


func _on_region_drawn(id: String, center: Vector2, size: Vector2) -> void:
	selected_region = id
	_apply_region_values(id, _authored_center(id, center), size,
			"Drew " + _region_name(id))


func _on_region_resized(id: String, size: Vector2) -> void:
	if controller == null:
		return
	var region: Dictionary = Calibration.region(controller.get_calibration(), id)
	if region.is_empty():
		return
	_apply_region_values(id, Vector2(region.center), size,
			"Scaled " + _region_name(id))


func _apply_region_values(id: String, center: Vector2, size: Vector2, message: String,
		resize := true) -> Dictionary:
	if controller == null or base.is_empty():
		return {"ok": false, "error": "no calibration"}
	params["center_%s_x" % id] = center.x
	params["center_%s_y" % id] = center.y
	if resize:
		params["size_%s_x" % id] = maxf(size.x, 0.02)
		params["size_%s_y" % id] = maxf(size.y, 0.02)
	_sync_sliders()
	working = _rebuild_values()
	var result: Dictionary = controller.apply_calibration(working, false)
	if not bool(result.get("ok", false)):
		_set_action("Rejected: " + String(result.get("error", "")), true)
		return result
	_set_action(message + " (not saved)")
	_refresh_overlays()
	return {"ok": true, "error": "", "center": center, "size": size}


func _region_anchor(id: String) -> Vector2:
	var index := REGION_IDS.find(id)
	if index < 0:
		return Vector2.ZERO
	# Use the same anchors the panes are drawing, so a drag while the mapping is
	# held does not jump by the difference between live and held anchors.
	if index < _display_anchors.size():
		return Vector2(_display_anchors[index])
	if controller == null:
		return Vector2.ZERO
	var state: Dictionary = controller.last_state
	var regions: Array = state.get("regions", [])
	var rest: Array = state.get("region_rest", [])
	if index < regions.size() and index < rest.size():
		return Vector2(regions[index]) - Vector2(rest[index])
	return Vector2.ZERO


## The model/camera show the effective centre (authored + FaceRig anchor); the
## persisted calibration stores the authored centre.
func _authored_center(id: String, effective: Vector2) -> Vector2:
	return effective - _region_anchor(id)


func _set_action(text: String, warn := false) -> void:
	if action_status == null:
		return
	action_status.text = text
	action_status.add_theme_color_override("font_color", WARN_COLOUR if warn else MUTED)


func _region_name(id: String) -> String:
	match id:
		"eye_l":
			return "eye L"
		"eye_r":
			return "eye R"
		"mouth":
			return "mouth"
	return id


func _region_button_name(id: String) -> String:
	match id:
		"eye_l":
			return "L eye"
		"eye_r":
			return "R eye"
		"mouth":
			return "Mouth"
	return id


func _sync_selected_buttons() -> void:
	for id in draw_buttons.keys():
		var button: Button = draw_buttons[id]
		button.set_pressed_no_signal(id == selected_region and camera_view != null \
				and camera_view.get_draw_mode() == id)
	if model_header_label != null:
		model_header_label.text = _region_name(selected_region)


func _on_apply_pressed() -> void:
	if controller == null:
		return
	working = _rebuild_values()
	var applied: Dictionary = controller.apply_calibration(working, true)
	if not bool(applied.get("ok", false)):
		last_error = String(applied.get("error", "calibration rejected"))
		action_status.text = "Apply failed: " + last_error
		action_status.add_theme_color_override("font_color", WARN_COLOUR)
		return
	var saved: Dictionary = controller.save_calibration()
	last_apply = saved.duplicate(true)
	if not bool(saved.get("ok", false)):
		action_status.text = "Applied live, but save failed: " + String(saved.get("error", ""))
		action_status.add_theme_color_override("font_color", WARN_COLOUR)
		return
	base = controller.get_calibration()
	_reset_params_from_base()
	action_status.text = "Applied to model and saved to %s" % String(saved.get("path", ""))
	action_status.add_theme_color_override("font_color", OK_COLOUR)
	_refresh_overlays()


func _on_reset_pressed() -> void:
	if controller == null:
		return
	var reset: Dictionary = controller.reset_calibration()
	if not bool(reset.get("ok", false)):
		last_error = String(reset.get("error", "reset failed"))
		action_status.text = "Reset failed: " + last_error
		action_status.add_theme_color_override("font_color", WARN_COLOUR)
		return
	base = controller.get_calibration()
	_reset_params_from_base()
	action_status.text = "Reset to provisional defaults (press Apply to save)"
	action_status.add_theme_color_override("font_color", WARN_COLOUR)
	_refresh_overlays()


func _on_background_selected(index: int) -> void:
	set_background("white" if index == 0 else "black")


func set_background(name: String) -> void:
	background = "white" if name == "white" else "black"
	if final_background != null:
		final_background.color = Color.WHITE if background == "white" else Color.BLACK
	if background_option != null:
		background_option.selected = 0 if background == "white" else 1


func get_background() -> String:
	return background


func _on_capture_selected(index: int) -> void:
	if controller == null or index < 0 or index >= CAPTURE_MODES.size():
		return
	var mode: Dictionary = CAPTURE_MODES[index]
	var options: Dictionary = controller.get_options()
	options.erase("capture_width")
	options.erase("capture_height")
	options.erase("capture_fps")
	if int(mode.width) > 0:
		options["capture_width"] = int(mode.width)
		options["capture_height"] = int(mode.height)
		options["capture_fps"] = int(mode.fps)
	var result: Dictionary = controller.restart(options)
	if bool(result.get("ok", false)):
		action_status.text = "Restarted capture: " + String(mode.label)
		action_status.add_theme_color_override("font_color", OK_COLOUR)
	else:
		action_status.text = "Capture restart failed: " + String(result.get("error", ""))
		action_status.add_theme_color_override("font_color", WARN_COLOUR)
	_refresh_status()


func _on_tracker_selected(index: int) -> void:
	if controller == null or index < 0 or index >= TRACKER_INPUTS.size():
		return
	var options: Dictionary = controller.get_options()
	options["tracker_input"] = int(TRACKER_INPUTS[index])
	var result: Dictionary = controller.restart(options)
	if bool(result.get("ok", false)):
		action_status.text = "Restarted tracker at %d input" % int(TRACKER_INPUTS[index])
		action_status.add_theme_color_override("font_color", OK_COLOUR)
	else:
		action_status.text = "Tracker restart failed: " + String(result.get("error", ""))
		action_status.add_theme_color_override("font_color", WARN_COLOUR)
	_refresh_status()


func _on_preview_toggled(pressed: bool) -> void:
	if camera_view != null:
		camera_view.set_preview_enabled(pressed)
	if controller != null:
		controller.set_preview_enabled(pressed)


func _build_file_dialog() -> void:
	file_dialog = FileDialog.new()
	file_dialog.name = "StillFileDialog"
	file_dialog.title = "Load a still image (development)"
	file_dialog.access = FileDialog.ACCESS_FILESYSTEM
	file_dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE
	file_dialog.filters = PackedStringArray(["*.png, *.jpg, *.jpeg, *.webp ; Images"])
	file_dialog.file_selected.connect(_on_still_selected)
	add_child(file_dialog)


func _on_load_still_pressed() -> void:
	if file_dialog != null:
		file_dialog.popup_centered_ratio(0.7)


func _on_snapshot_pressed() -> void:
	if controller == null:
		return
	var user_path := ProjectSettings.globalize_path("user://T25c-snapshot.png")
	var result: Dictionary = controller.snapshot(user_path)
	if not bool(result.get("ok", false)):
		if action_status != null:
			action_status.text = "Snapshot failed: " + String(result.get("error", ""))
			action_status.add_theme_color_override("font_color", WARN_COLOUR)
		return
	# Keep a local copy next to the other verification artifacts. The user://
	# file is the one the operator can reload with --live-source still.
	var tools_path := ProjectSettings.globalize_path("res://../.tools/T25c-snapshot.png")
	DirAccess.make_dir_recursive_absolute(tools_path.get_base_dir())
	DirAccess.copy_absolute(user_path, tools_path)
	if action_status != null:
		action_status.text = "Frozen on snapshot: %s (reload with --live-source still --still)" % user_path
		action_status.add_theme_color_override("font_color", OK_COLOUR)
	_refresh(true)


func _on_still_selected(path: String) -> void:
	if hud == null or not hud.has_method("use_still"):
		return
	if bool(hud.call("use_still", path)):
		if action_status != null:
			action_status.text = "Still: " + path.get_file()
	else:
		if action_status != null:
			action_status.text = "Still load failed"


func _on_use_camera_pressed() -> void:
	if hud != null and hud.has_method("use_camera"):
		if bool(hud.call("use_camera")):
			if action_status != null:
				action_status.text = "Camera source resumed"
				action_status.add_theme_color_override("font_color", OK_COLOUR)
		else:
			if action_status != null:
				action_status.text = "Camera source failed; check the device"
				action_status.add_theme_color_override("font_color", WARN_COLOUR)
	elif action_status != null:
		action_status.text = "Camera source unavailable"
	_refresh(true)


func _on_close_pressed() -> void:
	if hud != null:
		hud.call("close_setup")
	else:
		close()


# ---------------------------------------------------------------------------
# Test and operator hooks
# ---------------------------------------------------------------------------

func get_panes() -> Dictionary:
	return {
		"camera": camera_view,
		"mapping": mapping_view,
		"model": model_view,
		"final": final_frame,
		"status": find_child("SetupStatus", true, false),
		"controls": find_child("SetupControls", true, false),
	}


func get_calibration() -> Dictionary:
	return controller.get_calibration() if controller != null else {}


func get_mapping_values() -> Dictionary:
	return {
		"region_count": mapping_view.region_count if mapping_view != null else 0,
		"head_distance": mapping_view.head_distance if mapping_view != null else 0.0,
		"head_falloff": mapping_view.head_falloff if mapping_view != null else 0.0,
		"face_scale": mapping_view.face_scale if mapping_view != null else 0.0,
		"effective": mapping_view.effective.duplicate() if mapping_view != null else [],
	}


func get_camera_overlay() -> Dictionary:
	return {
		"landmark_count": camera_view.landmark_count if camera_view != null else 0,
		"face_box": camera_view.face_box if camera_view != null else Rect2(),
		"quad_points": camera_view.quad_points if camera_view != null else PackedVector2Array(),
		"anchor_count": camera_view.anchor_count if camera_view != null else 0,
		"has_frame": camera_view.has_frame if camera_view != null else false,
		"preview_enabled": camera_view.preview_enabled if camera_view != null else false,
	}


func get_action_status() -> String:
	return action_status.text if action_status != null else ""


func is_frozen() -> bool:
	return controller.is_frozen() if controller != null else false


func snapshot(path: String = "") -> Dictionary:
	if controller == null:
		return {"ok": false, "error": "no controller"}
	var target := path
	if target == "":
		target = ProjectSettings.globalize_path("user://T25c-snapshot.png")
	return controller.snapshot(target)


func use_camera() -> bool:
	if hud != null and hud.has_method("use_camera"):
		var ok: bool = bool(hud.call("use_camera"))
		_refresh(true)
		return ok
	return false


func get_source_label() -> String:
	return controller.get_source_label() if controller != null else "unavailable"


func get_source_path() -> String:
	return controller.get_source_path() if controller != null else ""


func set_region_center(id: String, centre: Vector2) -> void:
	params["center_%s_x" % id] = centre.x
	params["center_%s_y" % id] = centre.y
	_sync_sliders()
	working = _rebuild_values()
	if controller != null:
		controller.apply_calibration(working, false)
	_refresh_overlays()


func set_region_size(id: String, size: Vector2) -> void:
	params["size_%s_x" % id] = size.x
	params["size_%s_y" % id] = size.y
	_sync_sliders()
	working = _rebuild_values()
	if controller != null:
		controller.apply_calibration(working, false)
	_refresh_overlays()


func set_face_scale(multiplier: float) -> void:
	params["face_scale"] = multiplier
	_sync_sliders()
	working = _rebuild_values()
	if controller != null:
		controller.apply_calibration(working, true)
	_refresh_overlays()


func apply_to_model() -> Dictionary:
	_on_apply_pressed()
	return last_apply.duplicate(true)


func reset_calibration() -> Dictionary:
	_on_reset_pressed()
	return get_calibration()


func set_camera_preview(enabled: bool) -> void:
	if preview_check != null:
		preview_check.button_pressed = enabled
	_on_preview_toggled(enabled)


# ---------------------------------------------------------------------------
# Direct-manipulation hooks
# ---------------------------------------------------------------------------

## Selects a region and turns on drawing for it on the camera, mapping and
## model panes. Pass "" to stop drawing.
func set_draw_mode(id: String) -> void:
	if not REGION_IDS.has(id):
		id = ""
	selected_region = id if id != "" else selected_region
	_set_all_draw_modes(id)
	_sync_selected_buttons()
	_refresh_overlays()


## Test hook: draw a region from a normalised image-space rect (0..1).
func draw_region_from_image_rect(id: String, image_rect_norm: Rect2) -> Dictionary:
	if camera_view == null:
		return {"ok": false, "error": "no camera view"}
	var result: Dictionary = camera_view.call("canonical_rect_from_image_rect",
			image_rect_norm)
	if result.is_empty():
		return {"ok": false, "error": "no tracking homography"}
	var authored := _authored_center(id, Vector2(result.center))
	var applied := _apply_region_values(id, authored, Vector2(result.size),
			"Drew " + _region_name(id))
	if bool(applied.get("ok", false)):
		applied["center"] = authored
		applied["size"] = Vector2(result.size)
	return applied


## Test hook: draw a region from a pane-space rect (the coordinates the camera
## view's mouse handler receives).
func draw_region_from_screen_rect(id: String, screen_rect: Rect2) -> Dictionary:
	if camera_view == null:
		return {"ok": false, "error": "no camera view"}
	var result: Dictionary = camera_view.call("canonical_rect_from_screen_rect",
			screen_rect)
	if result.is_empty():
		return {"ok": false, "error": "no tracking homography"}
	var authored := _authored_center(id, Vector2(result.center))
	var applied := _apply_region_values(id, authored, Vector2(result.size),
			"Drew " + _region_name(id))
	if bool(applied.get("ok", false)):
		applied["center"] = authored
		applied["size"] = Vector2(result.size)
	return applied


## Test hook: move a region to the canonical point under a pane-space point on
## the model. This is the same path the mouse drag uses.
func drag_region_on_model(id: String, pane_point: Vector2) -> Dictionary:
	if model_view == null:
		return {"ok": false, "error": "no model view"}
	var canonical: Variant = model_view.call("canonical_from_screen", pane_point)
	if canonical == null:
		return {"ok": false, "error": "point is not on the face plane"}
	var authored := _authored_center(id, Vector2(canonical))
	var applied := _apply_region_values(id, authored, Vector2.ZERO,
			"Moved " + _region_name(id), false)
	if bool(applied.get("ok", false)):
		applied["center"] = authored
	return applied


## Test hook: draw a region from a pane-space box on the model. This is the same
## path the model pane's draw mode uses.
func draw_region_on_model(id: String, pane_rect: Rect2) -> Dictionary:
	if model_view == null:
		return {"ok": false, "error": "no model view"}
	var result: Dictionary = model_view.call("canonical_rect_from_screen_rect", pane_rect)
	if result.is_empty():
		return {"ok": false, "error": "box is not on the face plane"}
	var authored := _authored_center(id, Vector2(result.center))
	var applied := _apply_region_values(id, authored, Vector2(result.size),
			"Drew " + _region_name(id) + " on the model")
	if bool(applied.get("ok", false)):
		applied["center"] = authored
		applied["size"] = Vector2(result.size)
	return applied


## Test hook: scale a region about its centre, as the model handle does.
func resize_region_on_model(id: String, size: Vector2) -> Dictionary:
	if controller == null:
		return {"ok": false, "error": "no controller"}
	var region: Dictionary = Calibration.region(controller.get_calibration(), id)
	if region.is_empty():
		return {"ok": false, "error": "no region " + id}
	return _apply_region_values(id, Vector2(region.center), size,
			"Scaled " + _region_name(id))


func get_mapping_view():
	return mapping_view


func get_camera_region_polygons() -> Dictionary:
	return camera_view.get_region_polygons() if camera_view != null else {}


func get_mapping_region_polygons() -> Dictionary:
	return mapping_view.get_region_polygons() if mapping_view != null else {}


## Test hook: move a region to the canonical point under a camera-pane point.
func drag_region_on_camera(id: String, pane_point: Vector2) -> Dictionary:
	if camera_view == null:
		return {"ok": false, "error": "no camera view"}
	var canonical: Variant = camera_view.call("canonical_from_screen", pane_point)
	if canonical == null:
		return {"ok": false, "error": "point is not on the camera image"}
	var authored := _authored_center(id, Vector2(canonical))
	var applied := _apply_region_values(id, authored, Vector2.ZERO,
			"Moved " + _region_name(id), false)
	if bool(applied.get("ok", false)):
		applied["center"] = authored
	return applied


## Test hook: scale a region from the camera-pane handle path.
func resize_region_on_camera(id: String, size: Vector2) -> Dictionary:
	if controller == null:
		return {"ok": false, "error": "no controller"}
	var region: Dictionary = Calibration.region(controller.get_calibration(), id)
	if region.is_empty():
		return {"ok": false, "error": "no region " + id}
	return _apply_region_values(id, Vector2(region.center), size,
			"Scaled " + _region_name(id))


## Test hook: move a region to the canonical point under a mapping-pane point.
func drag_region_on_mapping(id: String, pane_point: Vector2) -> Dictionary:
	if mapping_view == null:
		return {"ok": false, "error": "no mapping view"}
	var canonical: Variant = mapping_view.call("canonical_from_screen", pane_point)
	if canonical == null:
		return {"ok": false, "error": "point is not in the mapping view"}
	var authored := _authored_center(id, Vector2(canonical))
	var applied := _apply_region_values(id, authored, Vector2.ZERO,
			"Moved " + _region_name(id), false)
	if bool(applied.get("ok", false)):
		applied["center"] = authored
	return applied


## Test hook: draw a region from a pane-space box on the mapping view.
func draw_region_on_mapping(id: String, pane_rect: Rect2) -> Dictionary:
	if mapping_view == null:
		return {"ok": false, "error": "no mapping view"}
	var result: Dictionary = mapping_view.call("canonical_rect_from_screen_rect",
			pane_rect)
	if result.is_empty():
		return {"ok": false, "error": "box is not in the mapping view"}
	var authored := _authored_center(id, Vector2(result.center))
	var applied := _apply_region_values(id, authored, Vector2(result.size),
			"Drew " + _region_name(id))
	if bool(applied.get("ok", false)):
		applied["center"] = authored
		applied["size"] = Vector2(result.size)
	return applied


## Test hook: scale a region from the mapping-pane handle path.
func resize_region_on_mapping(id: String, size: Vector2) -> Dictionary:
	if controller == null:
		return {"ok": false, "error": "no controller"}
	var region: Dictionary = Calibration.region(controller.get_calibration(), id)
	if region.is_empty():
		return {"ok": false, "error": "no region " + id}
	return _apply_region_values(id, Vector2(region.center), size,
			"Scaled " + _region_name(id))


func get_model_view():
	return model_view


func get_camera_image_rect() -> Rect2:
	return camera_view.image_rect if camera_view != null else Rect2()


func get_region_screen_polygons() -> Dictionary:
	return model_view.get_polygons() if model_view != null else {}


func get_fine_tune_rect() -> Rect2:
	return fine_tune_scroll.get_global_rect() if fine_tune_scroll != null else Rect2()


func get_selected_region() -> String:
	return selected_region
