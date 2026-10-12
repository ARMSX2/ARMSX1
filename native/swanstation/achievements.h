// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <jni.h>
#include <string>
#include <cstdint>
namespace SwanAchievements {
void Initialize(JNIEnv*,uint32_t(*reader)(uint32_t,uint8_t*,uint32_t));
std::string Hash(std::string path);
std::string Login(std::string user,std::string password);
void Logout();
void Game(std::string path);
void Unload();
void Frame();
void Reset();
std::string JSON();
std::string Presence();
void Option(std::string key,bool enabled);
void OptionInt(std::string key,int value);
void UnlockSound(std::string path);
}
