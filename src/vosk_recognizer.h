#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

// Forward-declare the opaque Vosk C types so we don't pull in vosk_api.h here.
struct VoskModel;
struct VoskRecognizer;

using namespace godot;

class VoskSpeechRecognizer : public RefCounted {
    GDCLASS(VoskSpeechRecognizer, RefCounted)

protected:
    static void _bind_methods();

public:
    VoskSpeechRecognizer();
    ~VoskSpeechRecognizer() override;

    // Load a Vosk model from the given filesystem path.
    // Returns true on success.
    bool load_model(const String &p_path);

    // Start a recognizer at the given sample rate (e.g. 16000.0).
    // Must be called after load_model().
    void start(float p_sample_rate);

    // Stop recognition and emit the final result signal.
    void stop();

    // Push raw PCM 16-bit little-endian mono audio bytes for recognition.
    void accept_waveform(const PackedByteArray &p_data);

    // Restrict recognition to a JSON array of phrases, e.g. '["hello world","[unk]"]'.
    // Pass an empty string to use the full vocabulary.
    void set_grammar(const String &p_grammar);

    // Discard accumulated audio and restart from silence.
    void reset();

    // Set Kaldi log verbosity (-1 = silent, 0 = default, >0 = verbose).
    void set_log_level(int p_level);

private:
    void _thread_func();

    VoskModel      *_model      = nullptr;
    VoskRecognizer *_recognizer = nullptr;

    std::thread             _thread;
    std::mutex              _mutex;
    std::condition_variable _cv;
    std::queue<PackedByteArray> _audio_queue;
    bool _running         = false;
    bool _stop_requested  = false;
};
