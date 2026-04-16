#include <jni.h>
#include <cstdio>
#include <exception>
#include <string>
#include "whisper_stt.h"

extern "C" JNIEXPORT jboolean JNICALL
Java_com_llamatik_library_platform_WhisperBridge_initModel(JNIEnv* env, jobject, jstring path) {
    const char* cpath = env->GetStringUTFChars(path, nullptr);
    try {
        int ok = whisper_stt_init(cpath);
        env->ReleaseStringUTFChars(path, cpath);
        return ok ? JNI_TRUE : JNI_FALSE;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "WhisperBridge.initModel native exception: %s\n", ex.what());
        env->ReleaseStringUTFChars(path, cpath);
        return JNI_FALSE;
    } catch (...) {
        std::fprintf(stderr, "WhisperBridge.initModel native exception: unknown\n");
        env->ReleaseStringUTFChars(path, cpath);
        return JNI_FALSE;
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_llamatik_library_platform_WhisperBridge_transcribeWav(JNIEnv* env, jobject, jstring wavPath, jstring lang, jstring initialPrompt) {
    const char* cwav = env->GetStringUTFChars(wavPath, nullptr);
    const char* clang = lang ? env->GetStringUTFChars(lang, nullptr) : nullptr;
    const char* cprompt = initialPrompt ? env->GetStringUTFChars(initialPrompt, nullptr) : nullptr;

    const char* out = nullptr;
    try {
        out = whisper_stt_transcribe_wav(cwav, clang, cprompt);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "WhisperBridge.transcribeWav native exception: %s\n", ex.what());
    } catch (...) {
        std::fprintf(stderr, "WhisperBridge.transcribeWav native exception: unknown\n");
    }

    if (initialPrompt) env->ReleaseStringUTFChars(initialPrompt, cprompt);
    if (lang) env->ReleaseStringUTFChars(lang, clang);
    env->ReleaseStringUTFChars(wavPath, cwav);

    return env->NewStringUTF(out ? out : "ERROR: native transcription failed");
}

extern "C" JNIEXPORT void JNICALL
Java_com_llamatik_library_platform_WhisperBridge_release(JNIEnv*, jobject) {
    try {
        whisper_stt_release();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "WhisperBridge.release native exception: %s\n", ex.what());
    } catch (...) {
        std::fprintf(stderr, "WhisperBridge.release native exception: unknown\n");
    }
}
