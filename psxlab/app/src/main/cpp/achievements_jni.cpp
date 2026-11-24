// SPDX-License-Identifier: GPL-3.0-or-later
// RetroAchievements host for PSX Lab sessions, built on the vendored MIT rcheevos rc_client.
// Softcore only. Java side: dev.psxlab.Achievements.
//
// Threads: the VM thread calls Machine/Frame/SessionBegin/SessionEnd, the UI polls Json/Presence,
// an IO thread drives Login, and one detached worker performs every HTTP request. rcheevos is
// built with RC_NO_THREADS, so every rc_client call is serialised by `mutex`. Events raised by
// rc_client are queued and delivered to Java after the lock is released.
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"
#include <jni.h>
#include <android/log.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
constexpr const char* kTag = "PSXLabAchievements";
constexpr uint32_t kRamBytes = 0x200000, kScratchBytes = 0x400, kNativeScratchOffset = 0x280000;

// Disabling is immediate; re-enabling mid-session cannot credit memory that ran untracked.
class TrackingPolicy {
 public:
  void Enabled(bool value) { if (enabled_ != value) ++generation_; enabled_ = value; if (!value) eligible_ = false; }
  void Begin() { running_ = true; eligible_ = enabled_; }
  void End() { running_ = eligible_ = false; }
  bool Enabled() const { return enabled_; }
  bool Active() const { return enabled_ && running_ && eligible_; }
  bool RestartRequired() const { return enabled_ && running_ && !eligible_; }
  uint64_t Generation() const { return generation_; }
  bool CanDispatch(uint64_t generation) const { return enabled_ && generation == generation_; }
 private:
  bool enabled_ = true, running_ = false, eligible_ = false;
  uint64_t generation_ = 0;
};

bool IsSubmission(std::string_view data) {
  size_t at = 0;
  while ((at = data.find("r=", at)) != std::string_view::npos) {
    if (at == 0 || data[at - 1] == '&' || data[at - 1] == '?') {
      const size_t start = at + 2, end = data.find('&', start);
      const auto action = data.substr(start, end == std::string_view::npos ? end : end - start);
      return action == "awardachievement" || action == "submitlbentry" || action == "ping" || action == "startsession";
    }
    at += 2;
  }
  return false;
}

std::recursive_mutex mutex;
std::mutex jobs_mutex, pending_mutex;
std::condition_variable jobs_ready;
std::deque<std::function<void()>> jobs;
std::deque<std::function<void(JNIEnv*)>> pending;  // Java callbacks, delivered without `mutex`
JavaVM* vm = nullptr;
jclass bridge = nullptr;
jmethodID request_method, store_method, restore_method, option_method, option_value_method;
jmethodID unlocked_method, event_method, sound_method, sector_method, log_method;
rc_client_t* client = nullptr;
rc_client_async_handle_t* loading = nullptr;
std::string game_hash;
bool game_running = false, cheats_blocked = false;
TrackingPolicy tracking;
std::set<uint32_t> primed;
const uint8_t* frame_ram = nullptr;
const uint8_t* frame_scratch = nullptr;
std::vector<uint8_t> copied_ram(kRamBytes), copied_scratch(kScratchBytes);
std::map<std::string, bool> settings{{"enabled", true}, {"notifications", true}, {"leaderboardNotifications", true},
    {"overlays", true}, {"lbOverlays", true}, {"soundEffects", true}, {"encoreMode", false},
    {"spectatorMode", false}, {"unofficialTestMode", false}};
std::map<std::string, int> number_settings{{"notificationsDuration", 5}, {"leaderboardsDuration", 10},
    {"notificationPosition", 1}, {"overlayPosition", 8}};

struct Env {
  JNIEnv* value = nullptr;
  bool attached = false;
  Env() {
    if (!vm) return;
    if (vm->GetEnv(reinterpret_cast<void**>(&value), JNI_VERSION_1_6) == JNI_OK) return;
    if (vm->AttachCurrentThread(&value, nullptr) == JNI_OK) attached = true; else value = nullptr;
  }
  ~Env() { if (attached) vm->DetachCurrentThread(); }
};

void Clear(JNIEnv* env) { if (env->ExceptionCheck()) env->ExceptionClear(); }
std::string Text(JNIEnv* env, jstring text) {
  if (!text) return {};
  const char* value = env->GetStringUTFChars(text, nullptr);
  if (!value) { Clear(env); return {}; }
  std::string out(value);
  env->ReleaseStringUTFChars(text, value);
  return out;
}
jstring Java(JNIEnv* env, const std::string& text) { return env->NewStringUTF(text.c_str()); }
std::pair<std::string, std::string> Pair(JNIEnv* env, jobjectArray array) {
  if (!array || env->GetArrayLength(array) != 2) return {};
  auto first = static_cast<jstring>(env->GetObjectArrayElement(array, 0));
  auto second = static_cast<jstring>(env->GetObjectArrayElement(array, 1));
  auto result = std::make_pair(Text(env, first), Text(env, second));
  if (first) env->DeleteLocalRef(first);
  if (second) env->DeleteLocalRef(second);
  return result;
}

void Post(std::function<void(JNIEnv*)> call) { std::lock_guard lock(pending_mutex); pending.push_back(std::move(call)); }
void Flush(JNIEnv* env) {
  if (!env) return;
  for (;;) {
    std::function<void(JNIEnv*)> call;
    { std::lock_guard lock(pending_mutex); if (pending.empty()) return; call = std::move(pending.front()); pending.pop_front(); }
    call(env);
    Clear(env);
  }
}
void Store(const std::string& name, const std::string& token) {
  Post([name, token](JNIEnv* env) {
    jstring user = Java(env, name), secret = Java(env, token);
    env->CallStaticVoidMethod(bridge, store_method, user, secret);
    env->DeleteLocalRef(user); env->DeleteLocalRef(secret);
  });
}
void Overlay(int kind, int id, const std::string& text, bool visible) {
  Post([kind, id, text, visible](JNIEnv* env) {
    jstring value = Java(env, text);
    env->CallStaticVoidMethod(bridge, event_method, kind, id, value, jboolean(visible));
    env->DeleteLocalRef(value);
  });
}

// One line in session.log (Java's stdout is teed there; native stdout is not) and in logcat.
void Note(const std::string& line) {
  __android_log_write(ANDROID_LOG_INFO, kTag, line.c_str());
  Post([line](JNIEnv* env) { jstring text = Java(env, line); env->CallStaticVoidMethod(bridge, log_method, text); env->DeleteLocalRef(text); });
}

void Queue(std::function<void()> job) {
  { std::lock_guard lock(jobs_mutex); jobs.push_back(std::move(job)); }
  jobs_ready.notify_one();
}
void Worker() {
  Env env;
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock lock(jobs_mutex);
      jobs_ready.wait_for(lock, std::chrono::milliseconds(100), [] { return !jobs.empty(); });
      if (!jobs.empty()) { job = std::move(jobs.front()); jobs.pop_front(); }
    }
    if (job) job();
    { std::lock_guard lock(mutex); if (client) rc_client_idle(client); }
    Flush(env.value);
  }
}

void Server(const rc_api_request_t* request, rc_client_server_callback_t callback, void* opaque, rc_client_t*) {
  const std::string url = request->url ? request->url : "", post = request->post_data ? request->post_data : "";
  const std::string type = request->content_type ? request->content_type : "";
  const bool submission = IsSubmission(post) || IsSubmission(url);
  uint64_t generation; bool admitted;
  { std::lock_guard lock(mutex); generation = tracking.Generation(); admitted = tracking.Active(); }
  Queue([url, post, type, callback, opaque, submission, generation, admitted] {
    std::pair<std::string, std::string> response{"-2", ""};
    bool allowed;
    { std::lock_guard lock(mutex); allowed = !submission || (admitted && tracking.CanDispatch(generation)); }
    if (!allowed) response = {"-1", ""};
    Env env;
    if (allowed && env.value) {
      jstring address = Java(env.value, url), body = Java(env.value, post), content = Java(env.value, type);
      auto result = static_cast<jobjectArray>(env.value->CallStaticObjectMethod(bridge, request_method, address, body, content));
      if (env.value->ExceptionCheck()) env.value->ExceptionClear(); else response = Pair(env.value, result);
      if (result) env.value->DeleteLocalRef(result);
      env.value->DeleteLocalRef(address); env.value->DeleteLocalRef(body); env.value->DeleteLocalRef(content);
    }
    int status = -2;
    try { status = std::stoi(response.first); } catch (...) {}
    rc_api_server_response_t server{response.second.c_str(), response.second.size(), status};
    std::lock_guard lock(mutex);
    callback(&server, opaque);
  });
}

// RA's PlayStation map: main RAM at 0, the data scratchpad directly after it at 0x200000.
uint32_t Read(uint32_t address, uint8_t* buffer, uint32_t count, rc_client_t*) {
  if (!frame_ram || !frame_scratch) return 0;
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t at = address + i;
    if (at < kRamBytes) buffer[i] = frame_ram[at];
    else if (at - kRamBytes < kScratchBytes) buffer[i] = frame_scratch[at - kRamBytes];
    else return i;
  }
  return count;
}
rc_clock_t Time(const rc_client_t*) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void Log(const char* message, const rc_client_t*) { __android_log_write(ANDROID_LOG_INFO, kTag, message); }

void Loaded(int result, const char* error, rc_client_t*, void*) {
  loading = nullptr;
  const auto* game = client ? rc_client_get_game_info(client) : nullptr;
  Note("load result=" + std::to_string(result) + " game=" + std::to_string(game ? game->id : 0u) +
       (result == RC_OK ? std::string() : std::string(" error=") + (error ? error : "unknown")));
}
void Load() {
  if (!client || !tracking.Active() || !game_running || cheats_blocked || game_hash.empty() || !rc_client_get_user_info(client)) return;
  if (loading) { rc_client_abort_async(client, loading); loading = nullptr; }
  Note("identifying game hash=" + game_hash);
  loading = rc_client_begin_load_game(client, game_hash.c_str(), Loaded, nullptr);
}
void Logged(int result, const char* error, rc_client_t*, void*) {
  if (result == RC_OK) {
    const auto* user = rc_client_get_user_info(client);
    if (user) { Store(user->username, user->token); Note(std::string("logged in as ") + user->username); }
    Load();
  } else {
    Note(std::string("login failed: ") + (error ? error : "unknown"));
  }
}
void Unload() {
  game_running = false;
  if (client) {
    if (loading) rc_client_abort_async(client, loading);
    loading = nullptr;
    rc_client_unload_game(client);
  }
  primed.clear();
  Overlay(3, 0, "", false);
}

void Event(const rc_client_event_t* event, rc_client_t*) {
  if (!tracking.Active()) return;
  if (event->achievement) {
    const auto* a = event->achievement;
    if (event->type == RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW) primed.insert(a->id);
    if (event->type == RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE || event->type == RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED) primed.erase(a->id);
    if (event->type == RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW && settings["overlays"]) Overlay(1, a->id, a->title, true);
    if (event->type == RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE || event->type == RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED) Overlay(1, a->id, "", false);
    if (event->type == RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED) {
      Note("unlocked id=" + std::to_string(a->id) + " points=" + std::to_string(a->points));
      const std::string title = a->title;
      const int points = int(a->points), seconds = number_settings["notificationsDuration"];
      Post([title, points, seconds](JNIEnv* env) {
        jstring value = Java(env, title);
        env->CallStaticVoidMethod(bridge, unlocked_method, value, points, seconds);
        env->DeleteLocalRef(value);
      });
    }
  }
  if (event->leaderboard && settings["leaderboardNotifications"] &&
      (event->type == RC_CLIENT_EVENT_LEADERBOARD_STARTED || event->type == RC_CLIENT_EVENT_LEADERBOARD_FAILED ||
       event->type == RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED)) {
    const char* prefix = event->type == RC_CLIENT_EVENT_LEADERBOARD_STARTED ? "Leaderboard started: "
        : event->type == RC_CLIENT_EVENT_LEADERBOARD_FAILED ? "Leaderboard failed: " : "Leaderboard submitted: ";
    Overlay(0, event->leaderboard->id, std::string(prefix) + event->leaderboard->title, true);
  }
  if (event->leaderboard_tracker) {
    const auto* tracker = event->leaderboard_tracker;
    if (event->type == RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE) Overlay(2, tracker->id, "", false);
    else if (settings["lbOverlays"] && (event->type == RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW || event->type == RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE))
      Overlay(2, tracker->id, tracker->display, true);
  }
  if (event->type == RC_CLIENT_EVENT_SERVER_ERROR && event->server_error)
    Note(std::string("server error ") + (event->server_error->api ? event->server_error->api : "") + ": " +
         (event->server_error->error_message ? event->server_error->error_message : ""));
}

std::string Quote(const char* text) {
  std::string out = "\"";
  for (unsigned char c : std::string(text ? text : "")) {
    if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
    else if (c < 32) { char buffer[7]; std::snprintf(buffer, sizeof(buffer), "\\u%04x", c); out += buffer; }
    else out += char(c);
  }
  return out + '"';
}

// --- Disc hashing through PSX Lab's own SectorSource (ISO, BIN/CUE, CHD and PBP alike) ---------
thread_local JNIEnv* hash_env = nullptr;
thread_local jobject hash_source = nullptr;
int hash_track = 0;
void* RC_CCONV HashOpen(const char*, uint32_t, const rc_hash_iterator_t*) { return hash_source ? &hash_track : nullptr; }
uint32_t RC_CCONV HashFirstSector(void*) { return 0; }  // SectorSource LBA 0 is track 1 INDEX 01
void RC_CCONV HashClose(void*) {}
size_t RC_CCONV HashRead(void*, uint32_t sector, void* buffer, size_t bytes) {
  if (!hash_env || !hash_source || sector > 0x7fffffffu) return 0;
  auto raw = static_cast<jbyteArray>(hash_env->CallStaticObjectMethod(bridge, sector_method, hash_source, jint(sector)));
  if (hash_env->ExceptionCheck()) { hash_env->ExceptionClear(); return 0; }
  if (!raw) return 0;
  size_t copied = 0;
  if (hash_env->GetArrayLength(raw) >= 2352) {
    jbyte mode = 0;
    hash_env->GetByteArrayRegion(raw, 15, 1, &mode);
    const jsize offset = mode == 2 ? 24 : 16;  // Mode 2 Form 1 has an 8-byte subheader
    copied = std::min<size_t>(bytes, 2048);
    hash_env->GetByteArrayRegion(raw, offset, jsize(copied), static_cast<jbyte*>(buffer));
  }
  hash_env->DeleteLocalRef(raw);
  return copied;
}
std::string Hash(JNIEnv* env, jobject source) {
  if (!bridge || !source) return {};
  hash_env = env; hash_source = source;
  rc_hash_iterator_t iterator{};
  rc_hash_initialize_iterator(&iterator, "psxlab-disc.bin", nullptr, 0);
  iterator.callbacks.error_message = [](const char* message, const rc_hash_iterator_t*) { __android_log_write(ANDROID_LOG_WARN, kTag, message); };
  auto& cd = iterator.callbacks.cdreader;
  cd.open_track = nullptr;
  cd.open_track_iterator = HashOpen;
  cd.read_sector = HashRead;
  cd.close_track = HashClose;
  cd.first_track_sector = HashFirstSector;
  char hash[33]{};
  const bool ok = rc_hash_generate(hash, RC_CONSOLE_PLAYSTATION, &iterator);
  rc_hash_destroy_iterator(&iterator);
  hash_env = nullptr; hash_source = nullptr;
  return ok ? std::string(hash) : std::string();
}

void PersistOption(const std::string& key, int value) {
  Post([key, value](JNIEnv* env) {
    jstring name = Java(env, key);
    env->CallStaticVoidMethod(bridge, option_method, name, value);
    env->DeleteLocalRef(name);
  });
}

std::string Json() {
  std::lock_guard lock(mutex);
  std::ostringstream out;
  const auto* user = client ? rc_client_get_user_info(client) : nullptr;
  out << "{\"trackingActive\":" << (tracking.Active() && !cheats_blocked ? "true" : "false")
      << ",\"trackingRestartRequired\":" << (tracking.RestartRequired() ? "true" : "false")
      << ",\"cheatsBlocked\":" << (cheats_blocked ? "true" : "false");
  out << ",\"loggedIn\":" << (user ? "true" : "false") << ",\"hardcore\":false,\"userName\":" << Quote(user ? user->username : "")
      << ",\"score\":" << (user ? user->score : 0) << ",\"softcoreScore\":" << (user ? user->score_softcore : 0)
      << ",\"avatarUrl\":" << Quote(user ? user->avatar_url : "");
  for (const auto& item : settings) out << ',' << Quote(item.first.c_str()) << ':' << (item.second ? "true" : "false");
  for (const auto& item : number_settings) out << ',' << Quote(item.first.c_str()) << ':' << item.second;
  const auto* game = client && game_running ? rc_client_get_game_info(client) : nullptr;
  out << ",\"active\":" << (game && game->id ? "true" : "false") << ",\"gameId\":" << (game ? game->id : 0)
      << ",\"gameTitle\":" << Quote(game ? game->title : "") << ",\"gameHash\":" << Quote(game_hash.c_str()) << ",\"items\":[";
  auto* list = game ? rc_client_create_achievement_list(client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
                                                        RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE) : nullptr;
  bool first = true;
  std::set<uint32_t> seen;
  if (list) {
    for (uint32_t b = 0; b < list->num_buckets; b++) {
      const auto& bucket = list->buckets[b];
      for (uint32_t i = 0; i < bucket.num_achievements; i++) {
        const auto* a = bucket.achievements[i];
        if (!seen.insert(a->id).second) continue;
        if (!first) out << ',';
        first = false;
        out << "{\"id\":" << a->id << ",\"title\":" << Quote(a->title) << ",\"description\":" << Quote(a->description)
            << ",\"points\":" << a->points << ",\"unlocked\":" << ((a->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE) ? "true" : "false")
            << ",\"unlockedMask\":" << int(a->unlocked) << ",\"measuredProgress\":" << Quote(a->measured_progress)
            << ",\"iconUrl\":" << Quote(a->unlocked ? a->badge_url : a->badge_locked_url) << ",\"subsetId\":" << bucket.subset_id
            << ",\"primed\":" << (primed.count(a->id) ? "true" : "false") << ",\"type\":" << int(a->type) << '}';
      }
    }
    rc_client_destroy_achievement_list(list);
  }
  out << "],\"subsets\":[";
  auto* subsets = game ? rc_client_create_subset_list(client) : nullptr;
  if (subsets) {
    for (uint32_t i = 0; i < subsets->num_subsets; i++) {
      if (i) out << ',';
      const auto* subset = subsets->subsets[i];
      out << "{\"id\":" << subset->id << ",\"title\":" << Quote(subset->title) << ",\"numAchievements\":" << subset->num_achievements << '}';
    }
    rc_client_destroy_subset_list(subsets);
  }
  out << "]}";
  return out.str();
}

jmethodID Static(JNIEnv* env, const char* name, const char* signature) { return env->GetStaticMethodID(bridge, name, signature); }
}  // namespace

extern "C" {
JNIEXPORT jboolean JNICALL Java_dev_psxlab_Achievements_nativeInitialize(JNIEnv* env, jclass cls, jstring user_agent) {
  std::lock_guard lock(mutex);
  if (client) return JNI_TRUE;
  env->GetJavaVM(&vm);
  bridge = static_cast<jclass>(env->NewGlobalRef(cls));
  request_method = Static(env, "request", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;");
  store_method = Static(env, "store", "(Ljava/lang/String;Ljava/lang/String;)V");
  restore_method = Static(env, "restore", "()[Ljava/lang/String;");
  option_method = Static(env, "option", "(Ljava/lang/String;I)V");
  option_value_method = Static(env, "optionValue", "(Ljava/lang/String;I)I");
  unlocked_method = Static(env, "unlocked", "(Ljava/lang/String;II)V");
  event_method = Static(env, "event", "(IILjava/lang/String;Z)V");
  sound_method = Static(env, "unlockSound", "(Ljava/lang/String;)V");
  sector_method = Static(env, "sector", "(Ldev/psxlab/core/SectorSource;I)[B");
  log_method = Static(env, "log", "(Ljava/lang/String;)V");
  if (env->ExceptionCheck()) { env->ExceptionClear(); return JNI_FALSE; }
  client = rc_client_create(Read, Server);
  if (!client) return JNI_FALSE;
  rc_client_enable_logging(client, RC_CLIENT_LOG_LEVEL_WARN, Log);
  rc_client_set_hardcore_enabled(client, 0);
  rc_client_set_allow_background_memory_reads(client, 0);
  rc_client_set_get_time_millisecs_function(client, Time);
  rc_client_set_event_handler(client, Event);
  (void)user_agent;  // The Java transport sets the HTTP User-Agent.
  for (auto& item : settings) {
    jstring name = Java(env, item.first);
    item.second = env->CallStaticIntMethod(bridge, option_value_method, name, int(item.second)) != 0;
    env->DeleteLocalRef(name); Clear(env);
  }
  tracking.Enabled(settings["enabled"]);
  for (auto& item : number_settings) {
    jstring name = Java(env, item.first);
    item.second = env->CallStaticIntMethod(bridge, option_value_method, name, item.second);
    env->DeleteLocalRef(name); Clear(env);
  }
  rc_client_set_encore_mode_enabled(client, settings["encoreMode"]);
  rc_client_set_spectator_mode_enabled(client, settings["spectatorMode"]);
  rc_client_set_unofficial_enabled(client, settings["unofficialTestMode"]);
  std::thread(Worker).detach();
  auto array = static_cast<jobjectArray>(env->CallStaticObjectMethod(bridge, restore_method));
  Clear(env);
  auto saved = Pair(env, array);
  if (array) env->DeleteLocalRef(array);
  if (!saved.first.empty() && !saved.second.empty())
    rc_client_begin_login_with_token(client, saved.first.c_str(), saved.second.c_str(), Logged, nullptr);
  return JNI_TRUE;
}

JNIEXPORT jstring JNICALL Java_dev_psxlab_Achievements_nativeHash(JNIEnv* env, jclass, jobject source) {
  static std::mutex hash_mutex;
  std::lock_guard lock(hash_mutex);
  return Java(env, Hash(env, source));
}

JNIEXPORT jstring JNICALL Java_dev_psxlab_Achievements_nativeLogin(JNIEnv* env, jclass, jstring user, jstring password) {
  std::string name = Text(env, user), secret = Text(env, password);
  struct Result { std::promise<std::string> value; };
  auto* result = new Result;
  auto future = result->value.get_future();
  {
    std::lock_guard lock(mutex);
    if (!client) { delete result; return Java(env, "RetroAchievements is not initialized."); }
    rc_client_begin_login_with_password(client, name.c_str(), secret.c_str(), [](int code, const char* error, rc_client_t* c, void* userdata) {
      auto* result = static_cast<Result*>(userdata);
      Logged(code, error, c, nullptr);
      result->value.set_value(code == RC_OK ? "" : error ? error : "Login failed.");
      delete result;
    }, result);
  }
  std::fill(secret.begin(), secret.end(), '\0');
  if (future.wait_for(std::chrono::seconds(25)) != std::future_status::ready)
    return Java(env, "Login is still pending; check the account panel shortly.");
  Flush(env);
  return Java(env, future.get());
}

JNIEXPORT void JNICALL Java_dev_psxlab_Achievements_nativeLogout(JNIEnv* env, jclass) {
  {
    std::lock_guard lock(mutex);
    if (client) { Unload(); rc_client_logout(client); }
    game_running = false;
  }
  Store("", "");
  Flush(env);
}

JNIEXPORT void JNICALL Java_dev_psxlab_Achievements_nativeSessionBegin(JNIEnv*, jclass) {
  std::lock_guard lock(mutex);
  tracking.Begin();
  cheats_blocked = false;
}

JNIEXPORT void JNICALL Java_dev_psxlab_Achievements_nativeSessionEnd(JNIEnv* env, jclass) {
  {
    std::lock_guard lock(mutex);
    tracking.End();
    Unload();
    game_hash.clear();
    cheats_blocked = false;
  }
  Flush(env);
}

// A machine started: a fresh boot, a reset, or a save-state load (`restored`).
JNIEXPORT void JNICALL Java_dev_psxlab_Achievements_nativeMachine(JNIEnv* env, jclass, jstring hash_value, jboolean restored) {
  const std::string hash = Text(env, hash_value);
  {
    std::lock_guard lock(mutex);
    if (!client || !tracking.Active()) return;
    if (hash.empty()) { Unload(); game_hash.clear(); }
    else if (hash == game_hash && game_running) {
      // Same disc: keep the loaded set, but re-arm every trigger so a loaded state or a reset
      // cannot satisfy a condition from memory the player never reached in this run.
      rc_client_reset(client);
      primed.clear();
      Overlay(3, 0, "", false);
    } else {
      Unload();
      game_hash = hash;
      game_running = true;
      Load();
    }
    Note("machine hash=" + (hash.empty() ? std::string("none") : hash) + " restored=" + (restored ? "1" : "0") +
         " logged_in=" + (rc_client_get_user_info(client) ? "1" : "0"));
  }
  Flush(env);
}

JNIEXPORT void JNICALL Java_dev_psxlab_Achievements_nativeFrame(JNIEnv* env, jclass, jobject memory, jbyteArray ram,
                                                                  jbyteArray scratch, jboolean cheats) {
  {
    std::lock_guard lock(mutex);
    if (!client || !game_running || !tracking.Active()) goto deliver;
    if (cheats) {
      if (!cheats_blocked) {
        cheats_blocked = true;
        Unload();
        game_running = false;
        Overlay(0, 0, "RetroAchievements paused for this session: cheats are active", true);
      }
      goto deliver;
    }
    if (memory) {
      auto* base = static_cast<uint8_t*>(env->GetDirectBufferAddress(memory));
      const jlong capacity = env->GetDirectBufferCapacity(memory);
      if (!base || capacity < jlong(kNativeScratchOffset + kScratchBytes)) goto deliver;
      frame_ram = base;
      frame_scratch = base + kNativeScratchOffset;
    } else {
      if (!ram || !scratch || env->GetArrayLength(ram) < jsize(kRamBytes) || env->GetArrayLength(scratch) < jsize(kScratchBytes)) goto deliver;
      env->GetByteArrayRegion(ram, 0, kRamBytes, reinterpret_cast<jbyte*>(copied_ram.data()));
      env->GetByteArrayRegion(scratch, 0, kScratchBytes, reinterpret_cast<jbyte*>(copied_scratch.data()));
      frame_ram = copied_ram.data();
      frame_scratch = copied_scratch.data();
    }
    rc_client_do_frame(client);
    frame_ram = frame_scratch = nullptr;
  }
deliver:
  Flush(env);
}

JNIEXPORT jstring JNICALL Java_dev_psxlab_Achievements_nativeJson(JNIEnv* env, jclass) { return Java(env, Json()); }

JNIEXPORT jstring JNICALL Java_dev_psxlab_Achievements_nativePresence(JNIEnv* env, jclass) {
  char text[2048]{};
  {
    std::lock_guard lock(mutex);
    if (client && game_running && tracking.Active()) rc_client_get_rich_presence_message(client, text, sizeof(text));
  }
  return Java(env, text);
}

JNIEXPORT void JNICALL Java_dev_psxlab_Achievements_nativeOption(JNIEnv* env, jclass, jstring key_value, jboolean enabled) {
  const std::string key = Text(env, key_value);
  {
    std::lock_guard lock(mutex);
    if (!settings.count(key)) return;
    settings[key] = enabled;
    PersistOption(key, enabled ? 1 : 0);
    if (key == "enabled") {
      tracking.Enabled(enabled);
      if (!enabled) { Unload(); game_hash.clear(); }
    } else if (client) {
      if ((key == "overlays" || key == "lbOverlays") && !enabled) Overlay(3, 0, "", false);
      if (key == "encoreMode") rc_client_set_encore_mode_enabled(client, enabled);
      else if (key == "spectatorMode") rc_client_set_spectator_mode_enabled(client, enabled);
      else if (key == "unofficialTestMode") rc_client_set_unofficial_enabled(client, enabled);
    }
  }
  Flush(env);
}

JNIEXPORT void JNICALL Java_dev_psxlab_Achievements_nativeOptionInt(JNIEnv* env, jclass, jstring key_value, jint value) {
  const std::string key = Text(env, key_value);
  {
    std::lock_guard lock(mutex);
    if (!number_settings.count(key)) return;
    const int bounded = key == "notificationsDuration" || key == "leaderboardsDuration" ? std::clamp(int(value), 3, 30)
        : key == "overlayPosition" ? std::clamp(int(value), 0, 8) : std::clamp(int(value), 1, 9);
    number_settings[key] = bounded;
    PersistOption(key, bounded);
  }
  Flush(env);
}

JNIEXPORT void JNICALL Java_dev_psxlab_Achievements_nativeUnlockSound(JNIEnv* env, jclass, jstring path) {
  std::string value = Text(env, path);
  Post([value](JNIEnv* e) { jstring text = Java(e, value); e->CallStaticVoidMethod(bridge, sound_method, text); e->DeleteLocalRef(text); });
  Flush(env);
}
}  // extern "C"
