// SPDX-License-Identifier: GPL-3.0-or-later
// Independently authored libretro/Android binding; frontend notices stay separate.
#include "runtime.h"
#include <jni.h>
#include <string>
#include <algorithm>
#include "vulkan_host.h"
#include "achievements.h"
#include "shader_api.h"
#include "performance.h"
#include <cmath>
namespace {
std::string Utf(JNIEnv* env,jstring value) {
    if (!value) return {};
    const char* text=env->GetStringUTFChars(value,nullptr);
    if (!text) return {};
    std::string result(text);env->ReleaseStringUTFChars(value,text);return result;
}
jstring String(JNIEnv* env,const std::string& value) {return env->NewStringUTF(value.c_str());}
jbyteArray Bytes(JNIEnv* env,const std::vector<unsigned char>& value) {
    auto result=env->NewByteArray(value.size());
    if(result && !value.empty())env->SetByteArrayRegion(result,0,value.size(),reinterpret_cast<const jbyte*>(value.data()));
    return result;
}
std::vector<std::string> Strings(JNIEnv* env,jobjectArray values) {
    std::vector<std::string> result;if(!values)return result;
    for(jsize i=0;i<env->GetArrayLength(values);i++){
        auto value=static_cast<jstring>(env->GetObjectArrayElement(values,i));result.push_back(Utf(env,value));env->DeleteLocalRef(value);
    }
    return result;
}
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_configureSwanSession(JNIEnv* env,jclass,jstring identity,jstring original) {
    SwanHost::ConfigureSession(Utf(env,identity),Utf(env,original));
}
extern "C" JNIEXPORT jstring JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getSwanStatePath(JNIEnv* env,jclass,jint slot) {
    return String(env,SwanHost::StatePath(slot));
}
extern "C" JNIEXPORT jstring JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getSwanCardPath(JNIEnv* env,jclass,jint slot) {
    return String(env,SwanHost::CardPath(slot));
}
extern "C" JNIEXPORT jint JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getSwanGuestFrame(JNIEnv*,jclass) {return SwanHost::GuestFrame();}
extern "C" JNIEXPORT jint JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getSwanMemoryByte(JNIEnv*,jclass,jint address) {return SwanHost::MemoryByte(address);}
extern "C" JNIEXPORT jbyteArray JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getSwanMemorySnapshot(JNIEnv* env,jclass) {return Bytes(env,SwanHost::MemorySnapshot());}
extern "C" JNIEXPORT jbyteArray JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getSwanCompanionSnapshot(JNIEnv* env,jclass) {return Bytes(env,SwanHost::CompanionSnapshot());}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setSwanCtrHudMoved(JNIEnv*,jclass,jboolean enabled){SwanHost::CtrHudMoved(enabled);}
extern "C" JNIEXPORT jstring JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getSwanCtrHudStatus(JNIEnv* env,jclass){return String(env,SwanHost::CtrHudStatus());}
extern "C" JNIEXPORT jboolean JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_initializeSwanBackend(JNIEnv* env,jclass cls, jstring root, jstring bios, jstring config) {
    return SwanHost::Initialize(env,cls,Utf(env,root),Utf(env,bios),Utf(env,config));
}
extern "C" JNIEXPORT jlong JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getAudioNonZeroSampleCount(JNIEnv*,jclass) {
    return SwanHost::AudioNonZero();
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setShaderChain(JNIEnv* env,jclass cls, jboolean enabled, jstring presetPath) {
    SwanShaders::Select(enabled,Utf(env,presetPath));
}
extern "C" JNIEXPORT jstring JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_shaderPresetParams(JNIEnv* env,jclass cls, jstring presetPath) {
    return String(env,SwanShaders::ParametersJSON(Utf(env,presetPath)));
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setShaderChainParams(JNIEnv* env,jclass cls, jstring presetPath, jobjectArray names, jfloatArray values) {
    if(!values)return;auto keys=Strings(env,names);const auto count=std::min<size_t>(keys.size(),env->GetArrayLength(values));
    std::vector<float> numbers(count);if(count)env->GetFloatArrayRegion(values,0,count,numbers.data());
    std::map<std::string,float> parameters;for(size_t i=0;i<count;i++)if(std::isfinite(numbers[i]))parameters[keys[i]]=numbers[i];
    SwanShaders::SetParameters(Utf(env,presetPath),std::move(parameters));
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_saveScreenshot(JNIEnv* env,jclass cls, jstring pngPath) {
    SwanHost::Screenshot(Utf(env,pngPath));
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setAdpfEnabled(JNIEnv* env,jclass cls, jboolean enabled) {
    SwanPerformance::Enable(enabled);
}
extern "C" JNIEXPORT jfloat JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getFPS(JNIEnv* env,jclass cls) {
    return SwanHost::FPS();
}
extern "C" JNIEXPORT jfloat JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getNominalFrameRate(JNIEnv* env,jclass cls) {
    return SwanHost::NominalFPS();
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setStatisticsEnabled(JNIEnv* env,jclass cls, jboolean enabled) {
    SwanHost::StatisticsEnabled(enabled);
}
extern "C" JNIEXPORT jint JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getStatistics(JNIEnv* env,jclass cls, jdoubleArray out) {
    if(!out)return 0;const auto stats=SwanHost::Statistics();const auto count=std::min<size_t>(stats.size(),env->GetArrayLength(out));
    if(count)env->SetDoubleArrayRegion(out,0,count,stats.data());return count;
}
extern "C" JNIEXPORT jstring JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getAchievementsJSON(JNIEnv* env,jclass cls) {
    return String(env,SwanAchievements::JSON());
}
extern "C" JNIEXPORT jstring JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getAchievementsHashForPath(JNIEnv* env,jclass cls, jstring imagePath) {
    return String(env,SwanAchievements::Hash(Utf(env,imagePath)));
}
extern "C" JNIEXPORT jstring JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getRichPresence(JNIEnv* env,jclass cls) {
    return String(env,SwanAchievements::Presence());
}
extern "C" JNIEXPORT jstring JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_loginAchievements(JNIEnv* env,jclass cls, jstring username, jstring password) {
    return String(env,SwanAchievements::Login(Utf(env,username),Utf(env,password)));
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_logoutAchievements(JNIEnv* env,jclass cls) {
    SwanAchievements::Logout();
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setHardcoreMode(JNIEnv* env,jclass cls, jboolean enabled) {
    SwanHost::Unsupported("setHardcoreMode");
}
extern "C" JNIEXPORT jboolean JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_isHardcoreMode(JNIEnv* env,jclass cls) {
    SwanHost::Unsupported("isHardcoreMode"); return 0;
}
extern "C" JNIEXPORT jboolean JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_isHardcorePersisted(JNIEnv* env,jclass cls) {
    SwanHost::Unsupported("isHardcorePersisted"); return 0;
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setAchievementsOption(JNIEnv* env,jclass cls, jstring key, jboolean enabled) {
    SwanAchievements::Option(Utf(env,key),enabled);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setAchievementsOptionInt(JNIEnv* env,jclass cls, jstring key, jint value) {
    SwanAchievements::OptionInt(Utf(env,key),value);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setAchievementsUnlockSound(JNIEnv* env,jclass cls, jstring path) {
    SwanAchievements::UnlockSound(Utf(env,path));
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setAchievementsHostOverride(JNIEnv* env,jclass cls, jstring host) {
    SwanHost::Unsupported("setAchievementsHostOverride");
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_clearAchievementsHostOverride(JNIEnv* env,jclass cls) {
    SwanHost::Unsupported("clearAchievementsHostOverride");
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setCustomVulkanDriver(JNIEnv* env,jclass cls, jstring driverDir, jstring driverName, jstring redirectDir, jstring hookLibDir) {
    if(!SwanHost::CustomDriver(Utf(env,driverDir),Utf(env,driverName),Utf(env,redirectDir),Utf(env,hookLibDir)))
        SwanHost::Unsupported("custom driver change while running (restart required)");
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setGlDriver(JNIEnv* env,jclass cls, jstring driver) {
    SwanHost::GlDriver(Utf(env,driver));
}
extern "C" JNIEXPORT jstring JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getGlDriver(JNIEnv* env,jclass cls) {
    return String(env,SwanHost::GlDriver());
}
extern "C" JNIEXPORT jstring JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getActiveRenderer(JNIEnv* env,jclass cls) {
    return String(env,SwanHost::Renderer());
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setDisplayAspect(JNIEnv* env,jclass cls, jint mode) {
    SwanHost::DisplayAspect(mode);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setDisplayAspectCustom(JNIEnv* env,jclass cls, jfloat ratio) {
    SwanHost::DisplayCustom(ratio);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setIntegerScaling(JNIEnv* env,jclass cls, jboolean enabled) {
    SwanHost::DisplayInteger(enabled);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setStretchMode(JNIEnv* env,jclass cls, jboolean enabled) {
    SwanHost::DisplayStretch(enabled);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setPgxpEnabled(JNIEnv* env,jclass cls, jboolean enabled) {
    SwanHost::Option("swanstation_GPU_PGXPEnable",enabled?"true":"false");
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setWidescreenHack(JNIEnv* env,jclass cls, jboolean enabled) {
    SwanHost::Option("swanstation_GPU_WidescreenHack",enabled?"true":"false");
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setDeinterlaceMode(JNIEnv* env,jclass cls, jint mode) {
    SwanHost::Option("swanstation_GPU_DisableInterlacing",mode==1?"true":"false");
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setOverscanCrop(JNIEnv* env,jclass cls, jint mode) {
    SwanHost::Option("swanstation_Display_CropMode",mode==1?"Overscan":mode==2?"Borders":"None");
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setDisplayRotation(JNIEnv* env,jclass cls, jint degrees) {
    SwanHost::DisplayRotation(degrees);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setGlVideoOptions(JNIEnv* env,jclass cls, jint textureFilter, jint downsample, jint lineDetect) {
    SwanHost::Option("swanstation_GPU_TextureFilter",textureFilter==1?"Bilinear":textureFilter==2?"xBR":"Nearest");
    SwanHost::Option("swanstation_GPU_DownsampleMode",downsample>0?"Box":"Disabled");
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setPs1TextureOptions(JNIEnv* env,jclass cls, jboolean dump, jboolean replace, jstring dir) {
    SwanHost::Textures(dump,replace,Utf(env,dir));
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setPadButton(JNIEnv* env,jclass cls, jint index, jint range, jboolean iskeypressed) {
    SwanHost::Button(0,index,range,iskeypressed);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setPadAnalog(JNIEnv* env,jclass cls, jint stick, jint x, jint y) {
    SwanHost::Analog(0,stick,x,y);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setPadButtonForPlayer(JNIEnv* env,jclass cls, jint player, jint index, jint range, jboolean iskeypressed) {
    SwanHost::Button(player,index,range,iskeypressed);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setPadAnalogForPlayer(JNIEnv* env,jclass cls, jint player, jint stick, jint x, jint y) {
    SwanHost::Analog(player,stick,x,y);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setMultitapEnabled(JNIEnv* env,jclass cls, jboolean enabled) {
    SwanHost::Option("swanstation_ControllerPorts_MultitapMode",enabled?"Port1Only":"Disabled");
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setRewind(JNIEnv* env,jclass cls, jboolean enabled, jint seconds, jint frequency) {
    SwanHost::Rewind(enabled,seconds,frequency);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setRunahead(JNIEnv* env,jclass cls, jint frames) {
    SwanHost::Option("swanstation_Main_RunaheadFrameCount",std::to_string(std::clamp<int>(frames,0,10)));
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setRewindActive(JNIEnv* env,jclass cls, jboolean active) {
    SwanHost::RewindActive(active);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_rewindStepBack(JNIEnv* env,jclass cls) {
    SwanHost::RewindStep();
}
extern "C" JNIEXPORT jlong JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_rewindSnapshotBytes(JNIEnv* env,jclass cls) {
    return SwanHost::RewindSnapshotBytes();
}
extern "C" JNIEXPORT jlong JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_rewindBytesHeld(JNIEnv* env,jclass cls) {
    return SwanHost::RewindBytesHeld();
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_resetKeyStatus(JNIEnv* env,jclass cls) {
    SwanHost::ResetButtons();
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setPortraitRenderTop(JNIEnv* env,jclass cls, jboolean top) {
    SwanHost::PortraitTop(top);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setPortraitRenderTopInset(JNIEnv* env,jclass cls, jint pixels) {
    SwanHost::PortraitInset(pixels);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setAffinityMode(JNIEnv* env,jclass cls, jint mode) {
    SwanPerformance::Affinity(mode);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_onNativeSurfaceCreated(JNIEnv* env,jclass cls,jlong owner) {
    SwanHost::SurfaceCreated(owner);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_onNativeSurfaceChanged(JNIEnv* env,jclass cls, jobject surface, jint w, jint h,jlong owner) {
    SwanHost::Surface(env,surface,owner);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_onNativeSurfaceDestroyed(JNIEnv* env,jclass cls,jlong owner) {
    SwanHost::Surface(env,nullptr,owner);
}
extern "C" JNIEXPORT jboolean JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_runVMThread(JNIEnv* env,jclass cls, jstring path) {
    const auto token=SwanHost::PrepareRun();if(!token)return false;
    const bool result=SwanHost::Run(Utf(env,path),token);SwanHost::ReleaseRun(token);return result;
}
extern "C" JNIEXPORT jlong JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_prepareVMRun(JNIEnv*,jclass) {
    return SwanHost::PrepareRun();
}
extern "C" JNIEXPORT jboolean JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_runVMThreadForSession(JNIEnv* env,jclass,jstring path,jlong token) {
    return SwanHost::Run(Utf(env,path),token);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_releaseVMRun(JNIEnv*,jclass,jlong token) {
    SwanHost::ReleaseRun(token);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_shutdownVMRun(JNIEnv*,jclass,jlong token) {
    SwanHost::StopRun(token);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setGpuHostHint(JNIEnv*,jclass,jstring) {
    // Hardware discovery belongs to the core's own graphics backend.
}
extern "C" JNIEXPORT jstring JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getDiscSerialForPath(JNIEnv* env,jclass,jstring path) {
    return String(env,SwanHost::DiscSerial(Utf(env,path)));
}
extern "C" JNIEXPORT jstring JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getGameSerial(JNIEnv* env,jclass) {return String(env,SwanHost::GameSerial());}
extern "C" JNIEXPORT jlongArray JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getSwanTextureMetrics(JNIEnv* env,jclass) {
    const auto data=SwanHost::TextureMetrics();auto out=env->NewLongArray(data.size());if(out&&!data.empty())env->SetLongArrayRegion(out,0,data.size(),reinterpret_cast<const jlong*>(data.data()));return out;}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_pause(JNIEnv* env,jclass cls) {
    SwanHost::Pause(true);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_resume(JNIEnv* env,jclass cls) {
    SwanHost::Pause(false);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setAudioSuspended(JNIEnv* env,jclass cls, jboolean suspended) {
    SwanHost::AudioSuspend(suspended);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setPresentationSuspended(JNIEnv* env,jclass cls, jboolean suspended) {
    SwanHost::PresentationSuspend(suspended);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_resetGame(JNIEnv* env,jclass cls) {
    SwanHost::Reset();
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setFastForward(JNIEnv* env,jclass cls, jboolean enabled) {
    SwanHost::FastForward(enabled);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_setSpeedLimits(JNIEnv* env,jclass cls, jboolean frameLimit, jint speedPercent, jint fpsLimit, jfloat fastForwardSpeed, jint frameSkip) {
    SwanHost::Speed(frameLimit,speedPercent,fastForwardSpeed);
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_enqueueLaunchArgument(JNIEnv* env,jclass cls, jstring argument) {
    SwanHost::Unsupported("enqueueLaunchArgument");
}
extern "C" JNIEXPORT void JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_shutdown(JNIEnv* env,jclass cls) {
    SwanHost::Stop();
}
extern "C" JNIEXPORT jboolean JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_hasActiveVM(JNIEnv* env,jclass cls) {
    return SwanHost::Active();
}
extern "C" JNIEXPORT jboolean JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_saveStateToSlot(JNIEnv* env,jclass cls, jint slot) {
    return SwanHost::Save(slot);
}
extern "C" JNIEXPORT jboolean JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_loadStateFromSlot(JNIEnv* env,jclass cls, jint slot) {
    return SwanHost::Load(slot);
}
extern "C" JNIEXPORT jint JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_loadStateFromSlotChecked(JNIEnv* env,jclass cls, jint slot, jboolean ignoreCardDivergence) {
    return SwanHost::LoadChecked(slot,ignoreCardDivergence);
}
extern "C" JNIEXPORT jstring JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getGamePathSlot(JNIEnv* env,jclass cls, jint slot) {
    return String(env,SwanHost::StateGame(slot));
}
extern "C" JNIEXPORT jbyteArray JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getImageSlot(JNIEnv* env,jclass cls, jint slot) {
    return Bytes(env,SwanHost::Preview(SwanHost::StatePath(slot)));
}
extern "C" JNIEXPORT jbyteArray JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getSaveStateImage(JNIEnv* env,jclass cls, jstring path) {
    return Bytes(env,SwanHost::Preview(Utf(env,path)));
}
extern "C" JNIEXPORT jboolean JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_changeDisc(JNIEnv* env,jclass cls, jstring path) {
    return SwanHost::Disc(Utf(env,path));
}
extern "C" JNIEXPORT jint JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_getPresentedFrameCount(JNIEnv* env,jclass cls) {
    return SwanHost::Frames();
}
extern "C" JNIEXPORT jint JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_cheatsLoad(JNIEnv* env,jclass cls, jstring path) {
    return SwanHost::CheatsLoad(Utf(env,path));
}
extern "C" JNIEXPORT jstring JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_cheatsList(JNIEnv* env,jclass cls) {
    return String(env,SwanHost::CheatsList());
}
extern "C" JNIEXPORT jint JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_cheatsApply(JNIEnv* env,jclass cls, jboolean master, jobjectArray enabledNames) {
    return SwanHost::CheatsApply(master,Strings(env,enabledNames));
}
extern "C" JNIEXPORT jint JNICALL Java_kr_co_iefriends_pcsx2_NativeApp_cheatsArmedCount(JNIEnv* env,jclass cls) {
    return SwanHost::CheatsArmed();
}
