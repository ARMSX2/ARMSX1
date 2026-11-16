#include <jni.h>
#include "mdec_transform.h"

extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_NativeMdec_transform(JNIEnv* env,jclass,jintArray input,jintArray table,jintArray output){
    if(!input||!table||!output||env->GetArrayLength(input)!=64||env->GetArrayLength(table)!=64||env->GetArrayLength(output)!=64){env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),"MDEC IDCT requires three 64-word blocks");return;}
    int32_t coefficients[64],scale[64],result[64];
    env->GetIntArrayRegion(input,0,64,coefficients);env->GetIntArrayRegion(table,0,64,scale);if(env->ExceptionCheck())return;
    PsxMdec::transform(coefficients,scale,result);env->SetIntArrayRegion(output,0,64,result);
}
