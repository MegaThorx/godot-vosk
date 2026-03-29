extends Control

# ---------------------------------------------------------------------------
# Vosk speech recognition demo
#
# Requirements:
#   1. Download a small Vosk model (e.g. vosk-model-small-en-us-0.15) and
#      place it under res://model/ in this project.
#   2. On Android the model must be copied to the user data directory first
#      (res:// is inside the APK and Vosk needs a plain filesystem path).
#   3. An AudioStreamMicrophone bus with an AudioEffectCapture effect must
#      exist on the "Mic" bus (or adjust CAPTURE_BUS_NAME below).
# ---------------------------------------------------------------------------

const MODEL_PATH_DESKTOP := "res://model/"
const MODEL_PATH_ANDROID_DEST := "user://model"
const CAPTURE_BUS_NAME := "Mic"
const SAMPLE_RATE := 16000.0

var _vosk  # VoskSpeechRecognizer — untyped so the script parses even if the extension is absent
var _audio_player: AudioStreamPlayer
var _capture_effect: AudioEffectCapture
var _poll_timer: Timer

@onready var _status_label: Label = $MarginContainer/VBoxContainer/StatusLabel
@onready var _partial_label: Label = $MarginContainer/VBoxContainer/PartialLabel
@onready var _results_label: RichTextLabel = $MarginContainer/VBoxContainer/ResultsLabel


func _ready() -> void:
	if not ClassDB.class_exists("VoskSpeechRecognizer"):
		_set_status("Error: VoskSpeechRecognizer not found — ensure the GDExtension "
				+ "compiled successfully and vosk.gdextension is in project/bin/.")
		return

	_vosk = ClassDB.instantiate("VoskSpeechRecognizer")
	_vosk.set_log_level(-1)
	_vosk.result.connect(_on_result)
	_vosk.partial_result.connect(_on_partial_result)
	_vosk.error.connect(_on_error)

	var model_path: String
	if OS.get_name() == "Android":
		model_path = ProjectSettings.globalize_path(MODEL_PATH_ANDROID_DEST)
		_extract_model_to_userdir()
	else:
		model_path = ProjectSettings.globalize_path(MODEL_PATH_DESKTOP)

	if not DirAccess.dir_exists_absolute(model_path):
		_set_status("Model not found at:\n%s\n\nDownload a model at https://alphacephei.com/vosk/models "
				+ "and place it under res://model/." % model_path)
		return

	if not _vosk.load_model(model_path):
		_set_status("Failed to load model from:\n" + model_path)
		return

	_setup_audio_capture()
	_vosk.start(SAMPLE_RATE)
	_set_status("Listening…")


func _set_status(text: String) -> void:
	if _status_label:
		_status_label.text = text


func _extract_model_to_userdir() -> void:
	var dest_dir := ProjectSettings.globalize_path(MODEL_PATH_ANDROID_DEST)
	if DirAccess.dir_exists_absolute(dest_dir):
		return  # already extracted
	_copy_dir_recursive(MODEL_PATH_DESKTOP, MODEL_PATH_ANDROID_DEST)


func _copy_dir_recursive(src_res: String, dst_user: String) -> void:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(dst_user))
	var da := DirAccess.open(src_res)
	if da == null:
		_set_status("Error: cannot open model directory: " + src_res)
		return
	da.list_dir_begin()
	var name := da.get_next()
	while name != "":
		if da.current_is_dir():
			_copy_dir_recursive(src_res + "/" + name, dst_user + "/" + name)
		else:
			var bytes := FileAccess.get_file_as_bytes(src_res + "/" + name)
			var f := FileAccess.open(dst_user + "/" + name, FileAccess.WRITE)
			if f:
				f.store_buffer(bytes)
				f.close()
		name = da.get_next()
	da.list_dir_end()


func _setup_audio_capture() -> void:
	var bus_idx := AudioServer.get_bus_index(CAPTURE_BUS_NAME)
	if bus_idx < 0:
		_set_status("Error: audio bus '%s' not found.\nCreate it in AudioServer with an AudioEffectCapture." % CAPTURE_BUS_NAME)
		return

	for i in AudioServer.get_bus_effect_count(bus_idx):
		var effect := AudioServer.get_bus_effect(bus_idx, i)
		if effect is AudioEffectCapture:
			_capture_effect = effect
			break

	if _capture_effect == null:
		_set_status("Error: no AudioEffectCapture found on bus '%s'." % CAPTURE_BUS_NAME)
		return

	_audio_player = AudioStreamPlayer.new()
	_audio_player.stream = AudioStreamMicrophone.new()
	_audio_player.bus = CAPTURE_BUS_NAME
	_audio_player.autoplay = true
	add_child(_audio_player)

	# Poll captured audio ~20 times per second (every 50 ms).
	_poll_timer = Timer.new()
	_poll_timer.wait_time = 0.05
	_poll_timer.autostart = true
	_poll_timer.timeout.connect(_poll_audio)
	add_child(_poll_timer)


func _poll_audio() -> void:
	if _capture_effect == null:
		return

	var frames := _capture_effect.get_frames_available()
	if frames <= 0:
		return

	# AudioEffectCapture gives stereo float32 frames at the project mix rate.
	# Vosk needs mono int16 at SAMPLE_RATE — we mix channels and convert.
	var mix_rate := float(AudioServer.get_mix_rate())
	var stereo_data := _capture_effect.get_buffer(frames)

	var ratio := mix_rate / SAMPLE_RATE
	var out_frames := int(float(frames) / ratio)
	var pcm := PackedByteArray()
	pcm.resize(out_frames * 2)  # 2 bytes per int16 sample

	for i in out_frames:
		var src_idx := int(float(i) * ratio)
		var s := stereo_data[src_idx]                   # Vector2 (L, R)
		var mono := clampf((s.x + s.y) * 0.5, -1.0, 1.0)
		var sample := int(mono * 32767.0)
		pcm.encode_s16(i * 2, sample)

	_vosk.accept_waveform(pcm)


func _on_result(text: String) -> void:
	# Emitted from the recognition thread — defer UI writes to the main thread.
	var json := JSON.new()
	if json.parse(text) == OK:
		var spoken: String = json.get_data().get("text", "").strip_edges()
		if not spoken.is_empty():
			call_deferred("_show_result", spoken)


func _show_result(spoken: String) -> void:
	_partial_label.text = ""
	_results_label.append_text(spoken + "\n")


func _on_partial_result(text: String) -> void:
	# Emitted from the recognition thread — defer UI writes to the main thread.
	var json := JSON.new()
	if json.parse(text) == OK:
		call_deferred("_show_partial", json.get_data().get("partial", ""))


func _show_partial(partial: String) -> void:
	_partial_label.text = partial


func _on_error(message: String) -> void:
	call_deferred("_set_status", "Error: " + message)


func _exit_tree() -> void:
	if _vosk:
		_vosk.stop()
