// CPU frame generation for the software display (Java Canvas): see frame_generation.h.
#include <jni.h>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <thread>
#include "frame_generation.h"

namespace {
psxfg::CpuFrameGen* generator(jlong handle) { return reinterpret_cast<psxfg::CpuFrameGen*>(handle); }
void failure(JNIEnv* env, const std::exception& error) { env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), error.what()); }
// Motion search and interpolation cost grows with the picture; the CPU keeps up only to about 640x512.
constexpr int kMaxPixels = 640 * 512;
}

extern "C" JNIEXPORT jlong JNICALL Java_dev_psxlab_core_NativeFrameGeneration_createNative(JNIEnv* env, jclass) {
    try {
        auto* g = new psxfg::CpuFrameGen();
        // Leave the emulation and audio threads their cores: at most half the CPUs, up to four.
        g->threads = std::clamp(int(std::thread::hardware_concurrency()) / 2, 1, 4);
        return reinterpret_cast<jlong>(g);
    } catch (const std::exception& error) { failure(env, error); return 0; }
}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_NativeFrameGeneration_destroyNative(JNIEnv*, jclass, jlong handle) { delete generator(handle); }
extern "C" JNIEXPORT jboolean JNICALL Java_dev_psxlab_core_NativeFrameGeneration_captureNative(JNIEnv* env, jclass, jlong handle, jintArray pixels, jint w, jint h, jboolean estimate, jboolean reset) {
    try {
        if (w < 1 || h < 1 || w > 1024 || h > 1024 || env->GetArrayLength(pixels) < w * h) throw std::runtime_error("Invalid frame generation input");
        auto* g = generator(handle);
        if (reset) g->havePrevious = false;
        jint* data = env->GetIntArrayElements(pixels, nullptr);
        if (!data) return false;
        g->capture(reinterpret_cast<const uint32_t*>(data), w, h, w, estimate && w * h <= kMaxPixels);
        env->ReleaseIntArrayElements(pixels, data, JNI_ABORT);
        return g->canInterpolate();
    } catch (const std::exception& error) { failure(env, error); return false; }
}
extern "C" JNIEXPORT jboolean JNICALL Java_dev_psxlab_core_NativeFrameGeneration_interpolateNative(JNIEnv* env, jclass, jlong handle, jfloat t, jboolean nearest, jintArray out) {
    try {
        auto* g = generator(handle);
        if (!g->canInterpolate() || env->GetArrayLength(out) < g->d.w * g->d.h) return false;
        jint* data = env->GetIntArrayElements(out, nullptr);
        if (!data) return false;
        g->interpolate(t, nearest, reinterpret_cast<uint32_t*>(data), g->d.w);
        env->ReleaseIntArrayElements(out, data, 0);
        return true;
    } catch (const std::exception& error) { failure(env, error); return false; }
}
