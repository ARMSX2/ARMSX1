#include <jni.h>
#include <string>
#include <vector>
#include "shader_chain.h"
namespace {
std::string text(JNIEnv* e,jstring value){if(!value)return {};const char* chars=e->GetStringUTFChars(value,nullptr);if(!chars)return {};std::string result(chars);e->ReleaseStringUTFChars(value,chars);return result;}
}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_ShaderChain_requestNative(JNIEnv* e,jclass,jboolean enabled,jstring preset){auto path=text(e,preset);armsx_shader_set_chain(enabled,path.c_str());}
extern "C" JNIEXPORT jboolean JNICALL Java_dev_psxlab_core_ShaderChain_activeNative(JNIEnv*,jclass){return psx_shader_active();}
extern "C" JNIEXPORT jstring JNICALL Java_dev_psxlab_core_ShaderChain_statusNative(JNIEnv* e,jclass){return e->NewStringUTF(psx_shader_status());}
// Overloaded JNI methods use their complete signatures.
extern "C" JNIEXPORT jstring JNICALL Java_dev_psxlab_core_ShaderChain_parametersNative__Ljava_lang_String_2(JNIEnv* e,jclass,jstring preset){auto path=text(e,preset);char* result=armsx_shader_preset_params(path.c_str());jstring output=e->NewStringUTF(result?result:"[]");armsx_shader_free_string(result);return output;}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_ShaderChain_parametersNative__Ljava_lang_String_2_3Ljava_lang_String_2_3F(JNIEnv* e,jclass,jstring preset,jobjectArray names,jfloatArray values){
    if(!names||!values||e->GetArrayLength(names)!=e->GetArrayLength(values)||e->GetArrayLength(names)>4096){e->ThrowNew(e->FindClass("java/lang/IllegalArgumentException"),"Invalid shader parameter arrays");return;}
    int count=e->GetArrayLength(names);std::vector<std::string> stored;stored.reserve(count);std::vector<const char*> pointers;std::vector<float> numbers(count);e->GetFloatArrayRegion(values,0,count,numbers.data());
    for(int i=0;i<count;i++){auto name=static_cast<jstring>(e->GetObjectArrayElement(names,i));stored.push_back(text(e,name));e->DeleteLocalRef(name);}for(auto& name:stored)pointers.push_back(name.c_str());auto path=text(e,preset);armsx_shader_queue_params(path.c_str(),pointers.data(),numbers.data(),count);
}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_ShaderChain_backendNative(JNIEnv*,jclass,jint mode){armsx_shader_note_backend(mode);}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_ShaderChain_phaseNative(JNIEnv*,jclass,jint total,jint current,jfloat fps,jfloat aspect){psx_shader_phase(total,current,fps,aspect);}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_ShaderChain_prepareNative(JNIEnv*,jclass){psx_shader_prepare();}
