// SPDX-License-Identifier: GPL-3.0-or-later
// Independently authored host for the vendored MIT rcheevos client.
#include "achievements.h"
#include "rc_client.h"
#include "rc_hash.h"
#include "rc_consoles.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <dlfcn.h>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <sstream>
#include <cstring>
#include <android/log.h>
namespace {
std::recursive_mutex mutex;
std::mutex jobs_mutex;
std::condition_variable jobs_ready;
std::deque<std::function<void()>> jobs;
JavaVM* vm=nullptr;jclass transport=nullptr;
jmethodID request_method=nullptr,store_method=nullptr,restore_method=nullptr;
jmethodID option_method=nullptr,option_value_method=nullptr,notify_method=nullptr;
jmethodID event_method=nullptr,sound_method=nullptr;
rc_client_t* client=nullptr;
rc_client_async_handle_t* loading=nullptr;
uint32_t(*read_memory)(uint32_t,uint8_t*,uint32_t)=nullptr;
std::string game_hash;
bool game_running=false;
std::map<std::string,bool> settings{{"notifications",true},{"leaderboardNotifications",true},{"overlays",true},{"lbOverlays",true},{"soundEffects",true},{"encoreMode",false},{"spectatorMode",false},{"unofficialTestMode",false}};
std::map<std::string,int> number_settings{{"notificationsDuration",5},{"leaderboardsDuration",10},{"notificationPosition",1},{"overlayPosition",8}};
std::set<uint32_t> primed;
struct Env {
  JNIEnv* value=nullptr;bool attached=false;
  Env(){if(!vm)return;attached=vm->GetEnv(reinterpret_cast<void**>(&value),JNI_VERSION_1_6)!=JNI_OK;if(attached&&vm->AttachCurrentThread(&value,nullptr)!=JNI_OK)value=nullptr;}
  ~Env(){if(attached&&value)vm->DetachCurrentThread();}
};
std::string Text(JNIEnv* env,jstring text){if(!text)return {};const char* value=env->GetStringUTFChars(text,nullptr);if(!value)return {};std::string out(value);env->ReleaseStringUTFChars(text,value);return out;}
std::pair<std::string,std::string> Pair(JNIEnv* env,jobjectArray array){if(!array||env->GetArrayLength(array)!=2)return {};
  auto first=static_cast<jstring>(env->GetObjectArrayElement(array,0)),second=static_cast<jstring>(env->GetObjectArrayElement(array,1));
  auto result=std::make_pair(Text(env,first),Text(env,second));env->DeleteLocalRef(first);env->DeleteLocalRef(second);return result;}
void Store(const std::string& name,const std::string& token){Env env;if(!env.value)return;
  auto user=env.value->NewStringUTF(name.c_str()),secret=env.value->NewStringUTF(token.c_str());
  env.value->CallStaticVoidMethod(transport,store_method,user,secret);env.value->DeleteLocalRef(user);env.value->DeleteLocalRef(secret);
  if(env.value->ExceptionCheck())env.value->ExceptionClear();}
void Queue(std::function<void()> job){{std::lock_guard lock(jobs_mutex);jobs.push_back(std::move(job));}jobs_ready.notify_one();}
void Worker(){for(;;){std::function<void()> job;
  {std::unique_lock lock(jobs_mutex);jobs_ready.wait_for(lock,std::chrono::milliseconds(100),[]{return !jobs.empty();});if(!jobs.empty()){job=std::move(jobs.front());jobs.pop_front();}}
  if(job)job();
  {std::lock_guard lock(mutex);if(client)rc_client_idle(client);}
}}
void Server(const rc_api_request_t* request,rc_client_server_callback_t callback,void* opaque,rc_client_t*) {
  const std::string url=request->url?request->url:"",post=request->post_data?request->post_data:"",type=request->content_type?request->content_type:"";
  Queue([url,post,type,callback,opaque]{Env env;std::pair<std::string,std::string> response{"-2",""};
    if(env.value){auto address=env.value->NewStringUTF(url.c_str()),body=env.value->NewStringUTF(post.c_str()),content=env.value->NewStringUTF(type.c_str());
      auto result=static_cast<jobjectArray>(env.value->CallStaticObjectMethod(transport,request_method,address,body,content));
      if(env.value->ExceptionCheck())env.value->ExceptionClear();else response=Pair(env.value,result);
      if(result)env.value->DeleteLocalRef(result);env.value->DeleteLocalRef(address);env.value->DeleteLocalRef(body);env.value->DeleteLocalRef(content);}
    int status=-2;try{status=std::stoi(response.first);}catch(...){}
    rc_api_server_response_t server{response.second.c_str(),response.second.size(),status};
    std::lock_guard lock(mutex);callback(&server,opaque);
  });
}
uint32_t Read(uint32_t address,uint8_t* buffer,uint32_t count,rc_client_t*){return read_memory?read_memory(address,buffer,count):0;}
rc_clock_t Time(const rc_client_t*){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void Loaded(int,const char*,rc_client_t*,void*){loading=nullptr;}
void Load(){if(!client||!game_running||game_hash.empty()||!rc_client_get_user_info(client))return;
  if(loading){rc_client_abort_async(client,loading);loading=nullptr;}
  loading=rc_client_begin_load_game(client,game_hash.c_str(),Loaded,nullptr);
}
void Logged(int result,const char*,rc_client_t*,void*){if(result==RC_OK){const auto* user=rc_client_get_user_info(client);if(user)Store(user->username,user->token);Load();}}
void Overlay(int kind,int id,const char* text,bool visible){Env env;if(!env.value||!transport)return;
  auto value=env.value->NewStringUTF(text?text:"");env.value->CallStaticVoidMethod(transport,event_method,kind,id,value,jboolean(visible));env.value->DeleteLocalRef(value);if(env.value->ExceptionCheck())env.value->ExceptionClear();}
void Event(const rc_client_event_t* event,rc_client_t*) {
  if(event->achievement){if(event->type==RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW)primed.insert(event->achievement->id);
    if(event->type==RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE||event->type==RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED)primed.erase(event->achievement->id);
    if(event->type==RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW&&settings["overlays"])Overlay(1,event->achievement->id,event->achievement->title,true);
    if(event->type==RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE||event->type==RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED)Overlay(1,event->achievement->id,"",false);
    if(event->type==RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED){Env env;if(env.value){auto title=env.value->NewStringUTF(event->achievement->title);
      env.value->CallStaticVoidMethod(transport,notify_method,title,int(event->achievement->points),number_settings["notificationsDuration"]);env.value->DeleteLocalRef(title);if(env.value->ExceptionCheck())env.value->ExceptionClear();}}}
  if(event->leaderboard&&settings["leaderboardNotifications"]&&(event->type==RC_CLIENT_EVENT_LEADERBOARD_STARTED||event->type==RC_CLIENT_EVENT_LEADERBOARD_FAILED||event->type==RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED)){
    const std::string text=std::string(event->type==RC_CLIENT_EVENT_LEADERBOARD_STARTED?"Leaderboard started: ":event->type==RC_CLIENT_EVENT_LEADERBOARD_FAILED?"Leaderboard failed: ":"Leaderboard submitted: ")+event->leaderboard->title;Overlay(0,event->leaderboard->id,text.c_str(),true);}
  if(event->leaderboard_tracker){const auto* tracker=event->leaderboard_tracker;
    if(event->type==RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE)Overlay(2,tracker->id,"",false);
    else if(settings["lbOverlays"]&&(event->type==RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW||event->type==RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE))Overlay(2,tracker->id,tracker->display,true);}
}
std::string Quote(const char* text){std::string out="\"";for(unsigned char c:std::string(text?text:"")){
  if(c=='"'||c=='\\'){out+='\\';out+=c;}else if(c<32){char buffer[7];snprintf(buffer,sizeof(buffer),"\\u%04x",c);out+=buffer;}else out+=c;}return out+'"';}
}
void SwanAchievements::Initialize(JNIEnv* env,uint32_t(*reader)(uint32_t,uint8_t*,uint32_t)) {
  std::lock_guard lock(mutex);read_memory=reader;if(client)return;
  env->GetJavaVM(&vm);auto cls=env->FindClass("kr/co/iefriends/pcsx2/SwanAchievementsTransport");if(!cls){env->ExceptionClear();return;}
  transport=static_cast<jclass>(env->NewGlobalRef(cls));env->DeleteLocalRef(cls);
  request_method=env->GetStaticMethodID(transport,"request","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;");
  store_method=env->GetStaticMethodID(transport,"store","(Ljava/lang/String;Ljava/lang/String;)V");restore_method=env->GetStaticMethodID(transport,"restore","()[Ljava/lang/String;");
  option_method=env->GetStaticMethodID(transport,"option","(Ljava/lang/String;I)V");option_value_method=env->GetStaticMethodID(transport,"optionValue","(Ljava/lang/String;I)I");notify_method=env->GetStaticMethodID(transport,"notifyUnlock","(Ljava/lang/String;II)V");
  event_method=env->GetStaticMethodID(transport,"event","(IILjava/lang/String;Z)V");sound_method=env->GetStaticMethodID(transport,"unlockSound","(Ljava/lang/String;)V");
  if(env->ExceptionCheck()){env->ExceptionClear();return;}
  client=rc_client_create(Read,Server);if(!client)return;
  rc_client_set_hardcore_enabled(client,0);rc_client_set_allow_background_memory_reads(client,0);
  rc_client_set_get_time_millisecs_function(client,Time);rc_client_set_event_handler(client,Event);
  for(auto& item:settings){auto name=env->NewStringUTF(item.first.c_str());item.second=env->CallStaticIntMethod(transport,option_value_method,name,int(item.second))!=0;env->DeleteLocalRef(name);}
  for(auto& item:number_settings){auto name=env->NewStringUTF(item.first.c_str());item.second=env->CallStaticIntMethod(transport,option_value_method,name,item.second);env->DeleteLocalRef(name);}
  rc_client_set_encore_mode_enabled(client,settings["encoreMode"]);rc_client_set_spectator_mode_enabled(client,settings["spectatorMode"]);rc_client_set_unofficial_enabled(client,settings["unofficialTestMode"]);
  std::thread(Worker).detach();
  auto array=static_cast<jobjectArray>(env->CallStaticObjectMethod(transport,restore_method));auto saved=Pair(env,array);if(array)env->DeleteLocalRef(array);
  if(env->ExceptionCheck())env->ExceptionClear();
  if(!saved.first.empty()&&!saved.second.empty())rc_client_begin_login_with_token(client,saved.first.c_str(),saved.second.c_str(),Logged,nullptr);
}
std::string SwanAchievements::Hash(std::string path){
  static std::mutex probe_mutex;std::lock_guard lock(probe_mutex);
  static void* core=dlopen("libswanstation.so",RTLD_NOW|RTLD_LOCAL);if(!core||path.empty())return {};
  rc_hash_iterator_t iterator{};rc_hash_initialize_iterator(&iterator,path.c_str(),nullptr,0);
  iterator.callbacks.error_message=[](const char* message,const rc_hash_iterator_t*){__android_log_write(ANDROID_LOG_WARN,"SwanHash",message);};
  auto& cd=iterator.callbacks.cdreader;
  cd.open_track=reinterpret_cast<decltype(cd.open_track)>(dlsym(core,"swan_hash_open"));
  cd.read_sector=reinterpret_cast<decltype(cd.read_sector)>(dlsym(core,"swan_hash_read"));
  cd.close_track=reinterpret_cast<decltype(cd.close_track)>(dlsym(core,"swan_hash_close"));
  cd.first_track_sector=reinterpret_cast<decltype(cd.first_track_sector)>(dlsym(core,"swan_hash_first"));cd.open_track_iterator=nullptr;
  char hash[33]{};const bool ok=cd.open_track&&cd.read_sector&&cd.close_track&&cd.first_track_sector&&rc_hash_generate(hash,RC_CONSOLE_PLAYSTATION,&iterator);
  rc_hash_destroy_iterator(&iterator);return ok?hash:std::string();
}
std::string SwanAchievements::Login(std::string user,std::string password){
  struct Result {std::promise<std::string> value;};auto result=new Result;auto future=result->value.get_future();
  {std::lock_guard lock(mutex);if(!client){delete result;return "RetroAchievements client is not initialized.";}
    rc_client_begin_login_with_password(client,user.c_str(),password.c_str(),[](int code,const char* error,rc_client_t* client,void* userdata){
      auto result=static_cast<Result*>(userdata);Logged(code,error,client,nullptr);result->value.set_value(code==RC_OK?"":error?error:"Login failed.");delete result;},result);}
  std::fill(password.begin(),password.end(),'\0');
  if(future.wait_for(std::chrono::seconds(25))!=std::future_status::ready)return "Login is still pending; check the account panel shortly.";
  return future.get();
}
void SwanAchievements::Logout(){std::lock_guard lock(mutex);if(client){if(loading)rc_client_abort_async(client,loading);loading=nullptr;rc_client_logout(client);}Store("","");primed.clear();Overlay(3,0,"",false);}
void SwanAchievements::Game(std::string path){auto hash=Hash(std::move(path));std::lock_guard lock(mutex);game_hash=std::move(hash);game_running=true;primed.clear();Load();}
void SwanAchievements::Unload(){std::lock_guard lock(mutex);game_running=false;if(client){if(loading)rc_client_abort_async(client,loading);loading=nullptr;rc_client_unload_game(client);}primed.clear();Overlay(3,0,"",false);}
void SwanAchievements::Frame(){std::lock_guard lock(mutex);if(client&&game_running)rc_client_do_frame(client);}
void SwanAchievements::Reset(){std::lock_guard lock(mutex);if(client)rc_client_reset(client);primed.clear();Overlay(3,0,"",false);}
std::string SwanAchievements::Presence(){std::lock_guard lock(mutex);char text[2048]{};if(client)rc_client_get_rich_presence_message(client,text,sizeof(text));return text;}
void SwanAchievements::Option(std::string key,bool enabled){std::lock_guard lock(mutex);if(!settings.count(key))return;settings[key]=enabled;
  Env env;if(env.value&&transport){auto name=env.value->NewStringUTF(key.c_str());env.value->CallStaticVoidMethod(transport,option_method,name,int(enabled));env.value->DeleteLocalRef(name);if(env.value->ExceptionCheck())env.value->ExceptionClear();}
  if(!client)return;
  if((key=="overlays"||key=="lbOverlays")&&!enabled)Overlay(3,0,"",false);
  if(key=="encoreMode")rc_client_set_encore_mode_enabled(client,enabled);
  else if(key=="spectatorMode")rc_client_set_spectator_mode_enabled(client,enabled);
  else if(key=="unofficialTestMode")rc_client_set_unofficial_enabled(client,enabled);}
void SwanAchievements::OptionInt(std::string key,int value){std::lock_guard lock(mutex);if(!number_settings.count(key))return;
  const int bounded=key=="notificationsDuration"||key=="leaderboardsDuration"?std::clamp(value,3,30):key=="overlayPosition"?std::clamp(value,0,8):std::clamp(value,1,9);number_settings[key]=bounded;
  Env env;if(env.value&&transport){auto name=env.value->NewStringUTF(key.c_str());env.value->CallStaticVoidMethod(transport,option_method,name,bounded);env.value->DeleteLocalRef(name);if(env.value->ExceptionCheck())env.value->ExceptionClear();}}
void SwanAchievements::UnlockSound(std::string path){Env env;if(env.value&&transport){auto value=env.value->NewStringUTF(path.c_str());env.value->CallStaticVoidMethod(transport,sound_method,value);env.value->DeleteLocalRef(value);if(env.value->ExceptionCheck())env.value->ExceptionClear();}}
std::string SwanAchievements::JSON(){std::lock_guard lock(mutex);std::ostringstream out;const auto* user=client?rc_client_get_user_info(client):nullptr;
  out<<"{\"loggedIn\":"<<(user?"true":"false")<<",\"hardcore\":false,\"userName\":"<<Quote(user?user->username:"")<<",\"score\":"<<(user?user->score:0)<<",\"softcoreScore\":"<<(user?user->score_softcore:0)<<",\"avatarUrl\":"<<Quote(user?user->avatar_url:"");
  for(const auto& item:settings)out<<','<<Quote(item.first.c_str())<<':'<<(item.second?"true":"false");
  for(const auto& item:number_settings)out<<','<<Quote(item.first.c_str())<<':'<<item.second;
  const auto* game=client?rc_client_get_game_info(client):nullptr;
  out<<",\"gameId\":"<<(game?game->id:0)<<",\"gameTitle\":"<<Quote(game?game->title:"")<<",\"items\":[";
  auto* list=client?rc_client_create_achievement_list(client,RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE):nullptr;
  bool first=true;std::set<uint32_t> seen;
  if(list){for(uint32_t b=0;b<list->num_buckets;b++){const auto& bucket=list->buckets[b];for(uint32_t i=0;i<bucket.num_achievements;i++){
    const auto* a=bucket.achievements[i];if(!seen.insert(a->id).second)continue;if(!first)out<<',';first=false;
    out<<"{\"id\":"<<a->id<<",\"title\":"<<Quote(a->title)<<",\"description\":"<<Quote(a->description)<<",\"points\":"<<a->points<<",\"unlocked\":"<<((a->unlocked&RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE)?"true":"false")
      <<",\"unlockedMask\":"<<int(a->unlocked)<<",\"measuredProgress\":"<<Quote(a->measured_progress)<<",\"iconUrl\":"<<Quote(a->unlocked?a->badge_url:a->badge_locked_url)
      <<",\"subsetId\":"<<bucket.subset_id<<",\"primed\":"<<(primed.count(a->id)?"true":"false")<<",\"type\":"<<int(a->type)<<'}';}}
    rc_client_destroy_achievement_list(list);}
  out<<"],\"subsets\":[";
  auto* subsets=client?rc_client_create_subset_list(client):nullptr;
  if(subsets){for(uint32_t i=0;i<subsets->num_subsets;i++){if(i)out<<',';const auto* subset=subsets->subsets[i];out<<"{\"id\":"<<subset->id<<",\"title\":"<<Quote(subset->title)<<",\"numAchievements\":"<<subset->num_achievements<<'}';}rc_client_destroy_subset_list(subsets);}
  out<<"]}";return out.str();
}
