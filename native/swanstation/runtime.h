// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <jni.h>
#include <string>
#include <vector>
namespace SwanHost {
bool Initialize(JNIEnv*, jclass, std::string root, std::string bios, std::string config);
uint64_t PrepareRun();
bool Run(std::string path, uint64_t token);
void ReleaseRun(uint64_t token);
void StopRun(uint64_t token);
void Stop();
void Pause(bool);
void SurfaceCreated(uint64_t owner);
void Surface(JNIEnv*, jobject, uint64_t owner);
void PresentationSuspend(bool);
bool Active();
float FPS();
float NominalFPS();
int Frames();
int64_t AudioNonZero();
void Button(int player, int key, int range, bool down);
void Analog(int player, int stick, int x, int y);
void ResetButtons();
void Option(std::string key, std::string value);
void Reset();
void AudioSuspend(bool);
void FastForward(bool);
void Speed(bool limit, int percent, float fast);
void ConfigureSession(std::string identity, std::string originalPath);
std::string DiscSerial(std::string path);
std::string GameSerial();
std::string CardPath(int slot);
int CheatsLoad(std::string path);
std::string CheatsList();
int CheatsApply(bool master, std::vector<std::string> names);
int CheatsArmed();
void Rewind(bool enabled, int seconds, int frequency);
void RewindActive(bool enabled);
void RewindStep();
int64_t RewindSnapshotBytes();
int64_t RewindBytesHeld();
void StatisticsEnabled(bool enabled);
std::vector<double> Statistics();
int GuestFrame();
int MemoryByte(int address);
std::vector<unsigned char> MemorySnapshot();
std::vector<unsigned char> CompanionSnapshot();
void CtrHudMoved(bool enabled);
std::string CtrHudStatus();
bool Save(int slot);
bool Load(int slot);
int LoadChecked(int slot, bool ignoreCardDivergence);
std::string StateGame(int slot);
std::vector<unsigned char> Preview(std::string path);
std::string StatePath(int slot);
void Screenshot(std::string path);
bool Disc(std::string path);
std::string EmbeddedDiscs(uint64_t session);
bool EmbeddedDisc(int index,uint64_t session);
std::string Renderer();
std::string DiagnosticsSnapshot();
void GlDriver(std::string driver);
std::string GlDriver();
bool CustomDriver(std::string directory,std::string name,std::string redirect,std::string hooks);
bool DriverVariables(std::string variables);
void Multiplayer(unsigned mode);
void ControllerTypes(unsigned first,unsigned second);
void DisplayAspect(int mode);
void DisplayCustom(float ratio);
void DisplayStretch(bool value);
void DisplayInteger(bool value);
void DisplayRotation(int degrees);
void Textures(bool dump,bool replace,std::string directory);
std::vector<int64_t> TextureMetrics();
void PortraitTop(bool value);
void PortraitInset(int pixels);
void Unsupported(std::string feature);
}
