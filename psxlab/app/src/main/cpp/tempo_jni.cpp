#include <jni.h>
#include "SoundTouch.h"
#include <vector>
#include <algorithm>
#include <exception>

struct TempoContext {
    soundtouch::SoundTouch dsp;
    std::vector<float> buffer;
    float tempo=1;
    TempoContext(){dsp.setSampleRate(44100);dsp.setChannels(2);dsp.setSetting(SETTING_USE_QUICKSEEK,0);dsp.setSetting(SETTING_SEQUENCE_MS,30);dsp.setSetting(SETTING_SEEKWINDOW_MS,20);dsp.setSetting(SETTING_OVERLAP_MS,10);}
};
static void error(JNIEnv* env,const std::exception& e){env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),e.what());}
extern "C" JNIEXPORT jlong JNICALL Java_dev_psxlab_NativeTempo_create(JNIEnv* env,jclass){try{return reinterpret_cast<jlong>(new TempoContext());}catch(const std::exception& e){error(env,e);return 0;}}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_NativeTempo_put(JNIEnv* env,jclass,jlong ptr,jshortArray input,jint count,jfloat tempo){
    try{auto* c=reinterpret_cast<TempoContext*>(ptr);c->buffer.resize(count);auto* pcm=env->GetShortArrayElements(input,nullptr);if(!pcm)return;for(int i=0;i<count;i++)c->buffer[i]=pcm[i]/32768.f;env->ReleaseShortArrayElements(input,pcm,JNI_ABORT);if(c->tempo!=tempo){c->tempo=tempo;c->dsp.setTempo(tempo);}c->dsp.putSamples(c->buffer.data(),count/2);}catch(const std::exception& e){error(env,e);}
}
extern "C" JNIEXPORT jint JNICALL Java_dev_psxlab_NativeTempo_read(JNIEnv* env,jclass,jlong ptr,jshortArray output){
    try{auto* c=reinterpret_cast<TempoContext*>(ptr);int capacity=env->GetArrayLength(output);c->buffer.resize(capacity);int frames=c->dsp.receiveSamples(c->buffer.data(),capacity/2);if(frames==0)return 0;auto* pcm=env->GetShortArrayElements(output,nullptr);if(!pcm)return 0;for(int i=0;i<frames*2;i++)pcm[i]=static_cast<short>(std::clamp(c->buffer[i]*32768.f,-32768.f,32767.f));env->ReleaseShortArrayElements(output,pcm,0);return frames*2;}catch(const std::exception& e){error(env,e);return 0;}
}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_NativeTempo_flush(JNIEnv* env,jclass,jlong ptr){try{reinterpret_cast<TempoContext*>(ptr)->dsp.flush();}catch(const std::exception& e){error(env,e);}}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_NativeTempo_destroy(JNIEnv*,jclass,jlong ptr){delete reinterpret_cast<TempoContext*>(ptr);}
