#include "vosk_recognizer.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <csignal>
#include <csetjmp>
#include <dlfcn.h>
// Android bionic NDK headers omit RTLD_DEEPBIND even though the kernel supports it.
#ifndef RTLD_DEEPBIND
#define RTLD_DEEPBIND 0x00008
#endif
#include <iostream>
#include <streambuf>
#include <string>

// Discards all output. Redirected to cerr/clog during vosk_model_new() so
// Kaldi's logging can't crash via Godot's null stdout rdbuf in ostream::sentry.
class NullStreambuf : public std::streambuf {
protected:
    int overflow(int c) override { return c; }
    std::streamsize xsputn(const char *, std::streamsize n) override { return n; }
};

// Kaldi calls abort() on any model-load error. We intercept SIGABRT with
// sigsetjmp/siglongjmp to surface it as an error signal instead of a crash.
static thread_local sigjmp_buf tl_vosk_recovery;
static thread_local bool       tl_vosk_in_load = false;

static void _vosk_sigabrt_handler(int /*sig*/) {
    if (tl_vosk_in_load) {
        siglongjmp(tl_vosk_recovery, 1);
    }
    // Not ours — restore default and re-raise so the process dies normally.
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}

// Resolves libvosk at runtime via dlopen (not a link-time NEEDED entry).
// Linux uses dlmopen(LM_ID_NEWLM) for full namespace isolation so Kaldi's
// allocations are insulated from Godot's global ::operator new override.
struct VoskLib {
    void *handle = nullptr;

    VoskModel*      (*model_new)(const char*)                                     = nullptr;
    void            (*model_free)(VoskModel*)                                      = nullptr;
    VoskRecognizer* (*recognizer_new)(VoskModel*, float)                           = nullptr;
    VoskRecognizer* (*recognizer_new_grm)(VoskModel*, float, const char*)          = nullptr;
    void            (*recognizer_free)(VoskRecognizer*)                            = nullptr;
    int             (*recognizer_accept_waveform)(VoskRecognizer*, const char*, int) = nullptr;
    int             (*recognizer_accept_waveform_s)(VoskRecognizer*, const short*, int) = nullptr;
    const char*     (*recognizer_result)(VoskRecognizer*)                          = nullptr;
    const char*     (*recognizer_partial_result)(VoskRecognizer*)                  = nullptr;
    const char*     (*recognizer_final_result)(VoskRecognizer*)                    = nullptr;
    void            (*recognizer_reset)(VoskRecognizer*)                           = nullptr;
    void            (*recognizer_set_grm)(VoskRecognizer*, const char*)            = nullptr;
    void            (*set_log_level)(int)                                          = nullptr;

    bool open(const std::string &path) {
#if defined(__linux__) && !defined(__ANDROID__)
        // Isolated linker namespace: Kaldi gets its own libstdc++/libc,
        // insulated from Godot's global ::operator new override.
        handle = dlmopen(LM_ID_NEWLM, path.c_str(), RTLD_NOW | RTLD_LOCAL);
#elif defined(__ANDROID__)
        // bionic already loaded libvosk.so as a NEEDED dep; get a handle.
        (void)path;
        handle = dlopen("libvosk.so", RTLD_NOLOAD | RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            handle = dlopen("libvosk.so", RTLD_NOW | RTLD_LOCAL);
        }
#else
        // macOS: RTLD_DEEPBIND unavailable; plain RTLD_LOCAL open.
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if (!handle) { return false; }
#define BIND(sym) sym = reinterpret_cast<decltype(sym)>(dlsym(handle, "vosk_" #sym))
        BIND(model_new);              BIND(model_free);
        BIND(recognizer_new);         BIND(recognizer_new_grm);
        BIND(recognizer_free);        BIND(recognizer_accept_waveform);
        BIND(recognizer_accept_waveform_s);
        BIND(recognizer_result);      BIND(recognizer_partial_result);
        BIND(recognizer_final_result); BIND(recognizer_reset);
        BIND(recognizer_set_grm);     BIND(set_log_level);
#undef BIND
        return model_new != nullptr;
    }

    void close() {
        if (handle) { dlclose(handle); handle = nullptr; }
    }
};

// Compute path to libvosk.so sitting next to our own .so
static std::string _vosk_lib_path() {
    Dl_info info{};
    dladdr(reinterpret_cast<void *>(&_vosk_lib_path), &info);
    std::string p = info.dli_fname ? info.dli_fname : "";
    auto slash = p.rfind('/');
    if (slash != std::string::npos) p.resize(slash + 1);
#if defined(__APPLE__)
    return p + "libvosk.dylib";
#else
    return p + "libvosk.so";
#endif
}

static VoskLib g_vosk;

bool vosk_dl_init() {
    if (g_vosk.handle) return true;
    return g_vosk.open(_vosk_lib_path());
}
void vosk_dl_shutdown() { g_vosk.close(); }

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

void VoskSpeechRecognizer::_bind_methods() {
    ClassDB::bind_method(D_METHOD("load_model", "path"),         &VoskSpeechRecognizer::load_model);
    ClassDB::bind_method(D_METHOD("start", "sample_rate"),       &VoskSpeechRecognizer::start);
    ClassDB::bind_method(D_METHOD("stop"),                        &VoskSpeechRecognizer::stop);
    ClassDB::bind_method(D_METHOD("accept_waveform", "data"),    &VoskSpeechRecognizer::accept_waveform);
    ClassDB::bind_method(D_METHOD("set_grammar", "grammar"),     &VoskSpeechRecognizer::set_grammar);
    ClassDB::bind_method(D_METHOD("reset"),                       &VoskSpeechRecognizer::reset);
    ClassDB::bind_method(D_METHOD("set_log_level", "level"),     &VoskSpeechRecognizer::set_log_level);

    ADD_SIGNAL(MethodInfo("result",         PropertyInfo(Variant::STRING, "text")));
    ADD_SIGNAL(MethodInfo("partial_result", PropertyInfo(Variant::STRING, "text")));
    ADD_SIGNAL(MethodInfo("error",          PropertyInfo(Variant::STRING, "message")));
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

VoskSpeechRecognizer::VoskSpeechRecognizer() {}

VoskSpeechRecognizer::~VoskSpeechRecognizer() {
    stop();
    if (_model) {
        if (g_vosk.model_free) g_vosk.model_free(_model);
        _model = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool VoskSpeechRecognizer::load_model(const String &p_path) {
    if (!g_vosk.handle) {
        emit_signal("error", String("Vosk: shared library not loaded (RTLD_DEEPBIND init failed)"));
        return false;
    }
    if (_model) {
        g_vosk.model_free(_model);
        _model = nullptr;
    }

    // Redirect cerr/clog to a null sink: Godot sets cout's rdbuf to null, and
    // cerr is tied to cout by default, causing a null deref in ostream::sentry.
    NullStreambuf null_buf;
    std::ostream  *old_cerr_tie  = std::cerr.tie(nullptr);
    std::ostream  *old_clog_tie  = std::clog.tie(nullptr);
    std::streambuf *old_cerr_buf = std::cerr.rdbuf(&null_buf);
    std::streambuf *old_clog_buf = std::clog.rdbuf(&null_buf);

    // Install a temporary SIGABRT handler so Kaldi's abort() doesn't kill us.
    struct sigaction sa_new {}, sa_old {};
    sa_new.sa_handler = _vosk_sigabrt_handler;
    sigemptyset(&sa_new.sa_mask);
    sa_new.sa_flags = 0;
    sigaction(SIGABRT, &sa_new, &sa_old);

    // RAII: restores signal handler and streams on all exit paths.
    struct LoadGuard {
        struct sigaction &sa_old;
        std::streambuf *cerr_buf, *clog_buf;
        std::ostream   *cerr_tie, *clog_tie;
        ~LoadGuard() {
            tl_vosk_in_load = false;
            sigaction(SIGABRT, &sa_old, nullptr);
            std::cerr.rdbuf(cerr_buf);  std::cerr.tie(cerr_tie);
            std::clog.rdbuf(clog_buf);  std::clog.tie(clog_tie);
        }
    } guard { sa_old, old_cerr_buf, old_clog_buf, old_cerr_tie, old_clog_tie };

    tl_vosk_in_load = true;

    if (sigsetjmp(tl_vosk_recovery, /*savemask=*/1) != 0) {
        emit_signal("error",
            String("Vosk: model loading failed — check that the model directory is valid: ") + p_path);
        return false;
    }

    // Kaldi can throw C++ exceptions through the extern "C" boundary of
    // vosk_model_new(); catch them to avoid std::terminate() triggering a
    // second abort that corrupts the C++ runtime's unwinding bookkeeping.
    try {
        _model = g_vosk.model_new(p_path.utf8().get_data());
    } catch (...) {
        emit_signal("error",
            String("Vosk: C++ exception thrown during model load: ") + p_path);
        return false;
    }

    if (!_model) {
        emit_signal("error", String("Vosk: failed to load model from: ") + p_path);
        return false;
    }
    return true;
}

void VoskSpeechRecognizer::start(float p_sample_rate) {
    if (!_model) {
        emit_signal("error", String("Cannot start: no model loaded."));
        return;
    }
    if (_running) {
        return;
    }

    if (_recognizer) {
        g_vosk.recognizer_free(_recognizer);
    }
    _recognizer = g_vosk.recognizer_new(_model, p_sample_rate);
    if (!_recognizer) {
        emit_signal("error", String("Failed to create Vosk recognizer."));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _stop_requested = false;
        _running = true;
        // Drain any leftover audio from a previous run.
        _audio_queue = {};
    }

    _thread = std::thread(&VoskSpeechRecognizer::_thread_func, this);
}

void VoskSpeechRecognizer::stop() {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_running) {
            return;
        }
        _stop_requested = true;
    }
    _cv.notify_all();

    if (_thread.joinable()) {
        _thread.join();
    }

    if (_recognizer) {
        const char *final_json = g_vosk.recognizer_final_result(_recognizer);
        if (final_json) {
            emit_signal("result", String(final_json));
        }
        g_vosk.recognizer_free(_recognizer);
        _recognizer = nullptr;
    }
    _running = false;
}

void VoskSpeechRecognizer::accept_waveform(const PackedByteArray &p_data) {
    if (!_running) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _audio_queue.push(p_data);
    }
    _cv.notify_one();
}

void VoskSpeechRecognizer::set_grammar(const String &p_grammar) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_recognizer) {
        g_vosk.recognizer_set_grm(_recognizer,
                p_grammar.is_empty() ? "[]" : p_grammar.utf8().get_data());
    }
}

void VoskSpeechRecognizer::reset() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_recognizer) {
        g_vosk.recognizer_reset(_recognizer);
    }
    _audio_queue = {};
}

void VoskSpeechRecognizer::set_log_level(int p_level) {
    if (g_vosk.set_log_level) g_vosk.set_log_level(p_level);
}

// ---------------------------------------------------------------------------
// Background thread
// ---------------------------------------------------------------------------

void VoskSpeechRecognizer::_thread_func() {
    while (true) {
        PackedByteArray chunk;

        {
            std::unique_lock<std::mutex> lock(_mutex);
            _cv.wait(lock, [this] {
                return !_audio_queue.empty() || _stop_requested;
            });

            if (_stop_requested && _audio_queue.empty()) {
                break;
            }

            chunk = _audio_queue.front();
            _audio_queue.pop();
        }

        if (chunk.is_empty()) {
            continue;
        }

        int finished = g_vosk.recognizer_accept_waveform(
                _recognizer,
                reinterpret_cast<const char *>(chunk.ptr()),
                static_cast<int>(chunk.size()));

        if (finished == 1) {
            // Full utterance — final result for this phrase.
            const char *res = g_vosk.recognizer_result(_recognizer);
            if (res) {
                emit_signal("result", String(res));
            }
        } else if (finished == 0) {
            // Still decoding — emit partial.
            const char *partial = g_vosk.recognizer_partial_result(_recognizer);
            if (partial) {
                emit_signal("partial_result", String(partial));
            }
        } else {
            // finished == -1: recognizer error — stop the loop.
            emit_signal("error", String("vosk_recognizer_accept_waveform returned an error."));
            break;
        }
    }
}
