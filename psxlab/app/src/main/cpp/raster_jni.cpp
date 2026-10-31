#include "raster_core.h"
extern "C" JNIEXPORT jlong JNICALL Java_dev_psxlab_core_NativeRasterizer_draw(JNIEnv* env,jclass,jintArray vram,jintArray packet,jintArray state){
 if(env->GetArrayLength(vram)!=1024*512||env->GetArrayLength(state)<7||env->GetArrayLength(packet)<2||env->GetArrayLength(packet)>12){env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),"Invalid native raster buffers");return 0;}
 jint p[12]={},e[7];env->GetIntArrayRegion(packet,0,env->GetArrayLength(packet),p);env->GetIntArrayRegion(state,0,7,e);if(env->ExceptionCheck())return 0;
 auto* ram=static_cast<jint*>(env->GetPrimitiveArrayCritical(vram,nullptr));if(!ram)return 0;
 Raster r{ram,e};int op=uint32_t(p[0])>>24;if(op<0x40)r.polygon(p,op);else if(op<0x60)r.line(p,op);else r.rectangle(p,op);
 env->ReleasePrimitiveArrayCritical(vram,ram,0);return r.pixels;
}

extern "C" JNIEXPORT jlong JNICALL Java_dev_psxlab_core_NativeRasterizer_drawBatch(JNIEnv* env,jclass,jintArray vram,jintArray commands){
 const int length=env->GetArrayLength(commands);
 if(env->GetArrayLength(vram)!=1024*512||length<=0||length>512){env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),"Invalid raster batch");return 0;}
 std::vector<jint> words(length);env->GetIntArrayRegion(commands,0,length,words.data());if(env->ExceptionCheck())return 0;
 for(int at=0;at<length;){int n=words[at++];if(n<2||n>12||at+n+7>length){env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),"Truncated raster batch");return 0;}
  int op=uint32_t(words[at])>>24,expected=0;
  if(op>=0x20&&op<0x40){int vertices=(op&8)?4:3;expected=(op&16)?vertices*(2+((op>>2)&1)):1+vertices*(1+((op>>2)&1));}
  else if(op>=0x40&&op<0x60)expected=(op&16)?4:3;
  else if(op>=0x60&&op<0x80)expected=2+((op>>2)&1)+((op&24)==0?1:0);
  if(n!=expected){env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),"Invalid raster packet length");return 0;}at+=n+7;
 }
 auto* ram=static_cast<jint*>(env->GetPrimitiveArrayCritical(vram,nullptr));if(!ram)return 0;i64 total=0;
 for(int at=0;at<length;){int n=words[at++];const int* p=words.data()+at;at+=n;Raster raster{ram,words.data()+at};at+=7;int op=uint32_t(p[0])>>24;
  if(op<0x40)raster.polygon(p,op);else if(op<0x60)raster.line(p,op);else raster.rectangle(p,op);total+=raster.pixels;
 }
 env->ReleasePrimitiveArrayCritical(vram,ram,0);return total;
}
