#include <jni.h>
#include <cstdint>
#include <cstdio>
#include <vector>
#define XXH_INLINE_ALL
#include "vendor/xxhash/xxhash.h"

// Hash PS1 words in little-endian order, independent of host array representation.
extern "C" JNIEXPORT jstring JNICALL Java_dev_psxlab_core_TexturePacks_hashNative(JNIEnv* env,jclass,jshortArray input){
    const jsize count=env->GetArrayLength(input);
    if(count<1||count>1024*512){env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),"Invalid texture upload size");return nullptr;}
    jshort* words=env->GetShortArrayElements(input,nullptr);if(!words)return nullptr;
    std::vector<uint8_t> bytes(size_t(count)*2);
    for(int i=0;i<count;i++){bytes[i*2]=uint8_t(words[i]);bytes[i*2+1]=uint8_t(uint16_t(words[i])>>8);}
    env->ReleaseShortArrayElements(input,words,JNI_ABORT);
    XXH128_hash_t hash=XXH3_128bits(bytes.data(),bytes.size());char name[33];
    std::snprintf(name,sizeof(name),"%016llX%016llX",static_cast<unsigned long long>(hash.high64),static_cast<unsigned long long>(hash.low64));
    return env->NewStringUTF(name);
}
