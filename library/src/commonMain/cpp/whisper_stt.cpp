#include "whisper_stt.h"

#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <mutex>
#include <string>
#endif

#include "whisper.h"

static whisper_context* g_ctx = nullptr;
static whisper_state* g_state = nullptr;

#ifdef _WIN32
static SRWLOCK g_lock = SRWLOCK_INIT;
static char g_last[16384] = { 0 };
#else
static std::mutex g_mu;
static std::string g_last;
#endif

struct whisper_lock_guard {
#ifdef _WIN32
    whisper_lock_guard() { AcquireSRWLockExclusive(&g_lock); }
    ~whisper_lock_guard() { ReleaseSRWLockExclusive(&g_lock); }
#else
    std::lock_guard<std::mutex> guard{ g_mu };
#endif
};

static void whisper_last_clear() {
#ifdef _WIN32
    g_last[0] = '\0';
#else
    g_last.clear();
#endif
}

static void whisper_last_set(const char* message) {
#ifdef _WIN32
    if (!message) {
        g_last[0] = '\0';
        return;
    }
    std::snprintf(g_last, sizeof(g_last), "%s", message);
#else
    g_last = message ? message : "";
#endif
}

static void whisper_last_append(const char* message) {
    if (!message || !message[0]) return;
#ifdef _WIN32
    const size_t used = std::strlen(g_last);
    if (used >= sizeof(g_last) - 1) return;
    std::snprintf(g_last + used, sizeof(g_last) - used, "%s", message);
#else
    g_last += message;
#endif
}

static const char* whisper_last_c_str() {
#ifdef _WIN32
    return g_last;
#else
    return g_last.c_str();
#endif
}

int whisper_stt_init(const char* model_path) {
    whisper_lock_guard lock;
    if (g_ctx && g_state) return 1;

    whisper_context_params cparams = whisper_context_default_params();
    // Windows desktop has been unstable with whisper.cpp's default GPU-first init.
    // Keep the change narrowly scoped to Windows and fall back to conservative CPU params.
#ifdef _WIN32
    cparams.use_gpu = false;
    cparams.flash_attn = false;
    cparams.dtw_token_timestamps = false;
    cparams.dtw_n_top = -1;
    cparams.dtw_mem_size = 0;
#endif

    try {
        whisper_context* ctx = whisper_init_from_file_with_params_no_state(model_path, cparams);
        if (!ctx) {
            g_ctx = nullptr;
            g_state = nullptr;
            return 0;
        }

        whisper_state* state = whisper_init_state(ctx);
        if (!state) {
            whisper_free(ctx);
            g_ctx = nullptr;
            g_state = nullptr;
            return 0;
        }

        g_ctx = ctx;
        g_state = state;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "whisper_stt_init failed for '%s': %s\n", model_path ? model_path : "(null)", ex.what());
        g_ctx = nullptr;
        g_state = nullptr;
    } catch (...) {
        std::fprintf(stderr, "whisper_stt_init failed for '%s': unknown native exception\n", model_path ? model_path : "(null)");
        g_ctx = nullptr;
        g_state = nullptr;
    }

    return g_ctx ? 1 : 0;
}

// Minimal WAV loader: only PCM16 mono 16kHz
static bool load_wav_pcm16_mono_16k(const char* path, std::vector<float>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;

    auto read_u32 = [&](uint32_t& v) {
        return std::fread(&v, 4, 1, f) == 1;
    };
    auto read_u16 = [&](uint16_t& v) {
        return std::fread(&v, 2, 1, f) == 1;
    };

    char riff[4];
    if (std::fread(riff, 1, 4, f) != 4) { std::fclose(f); return false; }
    uint32_t riffSize;
    if (!read_u32(riffSize)) { std::fclose(f); return false; }
    char wave[4];
    if (std::fread(wave, 1, 4, f) != 4) { std::fclose(f); return false; }

    if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
        std::fclose(f);
        return false;
    }

    bool fmtFound = false, dataFound = false;
    uint16_t audioFormat = 0, numChannels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0, dataSize = 0;
    long dataPos = 0;

    while (!fmtFound || !dataFound) {
        char id[4];
        if (std::fread(id, 1, 4, f) != 4) break;

        uint32_t size;
        if (!read_u32(size)) break;

        if (std::memcmp(id, "fmt ", 4) == 0) {
            fmtFound = true;

            read_u16(audioFormat);
            read_u16(numChannels);
            read_u32(sampleRate);

            uint32_t byteRate;
            read_u32(byteRate);

            uint16_t blockAlign;
            read_u16(blockAlign);

            read_u16(bitsPerSample);

            // Skip any extra fmt bytes
            if (size > 16) {
                std::fseek(f, (long)(size - 16), SEEK_CUR);
            }
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataFound = true;
            dataSize = size;
            dataPos = std::ftell(f);
            std::fseek(f, (long)size, SEEK_CUR);
        } else {
            std::fseek(f, (long)size, SEEK_CUR);
        }
    }

    if (!fmtFound || !dataFound) { std::fclose(f); return false; }

    // PCM = 1, mono, 16kHz, 16-bit
    if (audioFormat != 1) { std::fclose(f); return false; }
    if (numChannels != 1) { std::fclose(f); return false; }
    if (sampleRate != 16000) { std::fclose(f); return false; }
    if (bitsPerSample != 16) { std::fclose(f); return false; }

    std::fseek(f, dataPos, SEEK_SET);

    const int n = (int)(dataSize / 2);
    out.resize(n);

    for (int i = 0; i < n; i++) {
        int16_t s;
        if (std::fread(&s, 2, 1, f) != 1) { std::fclose(f); return false; }
        out[i] = (float)s / 32768.0f;
    }

    std::fclose(f);
    return true;
}

const char* whisper_stt_transcribe_wav(const char* wav_path, const char* language, const char* initial_prompt) {
    whisper_lock_guard lock;
    whisper_last_clear();

    if (!g_ctx || !g_state) {
        whisper_last_set("ERROR: Whisper not initialized");
        return whisper_last_c_str();
    }

    std::vector<float> pcmf;
    if (!load_wav_pcm16_mono_16k(wav_path, pcmf)) {
        whisper_last_set("ERROR: WAV must be PCM16 mono 16kHz");
        return whisper_last_c_str();
    }

    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.print_progress = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    params.translate = false;

    if (language && language[0]) {
        params.language = language;
    }

    if (initial_prompt && initial_prompt[0]) {
        params.initial_prompt = initial_prompt;
    }

    try {
        if (whisper_full_with_state(g_ctx, g_state, params, pcmf.data(), (int)pcmf.size()) != 0) {
            whisper_last_set("ERROR: whisper_full failed");
            return whisper_last_c_str();
        }
    } catch (const std::exception& ex) {
        char buffer[1024] = { 0 };
        std::snprintf(buffer, sizeof(buffer), "ERROR: whisper_full exception: %s", ex.what());
        whisper_last_set(buffer);
        return whisper_last_c_str();
    } catch (...) {
        whisper_last_set("ERROR: whisper_full exception");
        return whisper_last_c_str();
    }

    const int n = whisper_full_n_segments_from_state(g_state);
    for (int i = 0; i < n; i++) {
        const char* seg = whisper_full_get_segment_text_from_state(g_state, i);
        whisper_last_append(seg);
    }

    return whisper_last_c_str();
}

void whisper_stt_release(void) {
    whisper_lock_guard lock;
    if (g_state) {
        whisper_free_state(g_state);
        g_state = nullptr;
    }
    if (g_ctx) {
        whisper_free(g_ctx);
        g_ctx = nullptr;
    }
}
