// SPDX-License-Identifier: GPL-3.0-or-later
// Host scheduling only. No legacy engine thread/CPU assumptions.
#include "performance.h"
#include <atomic>
#include <dlfcn.h>
#include <sched.h>
#include <unistd.h>
#include <sys/resource.h>
#include <chrono>
#include <fstream>
#include <array>
#include <algorithm>
#include <android/log.h>
namespace {
std::atomic<bool> enabled{false};std::atomic<int> affinity{0};
using Manager=void;using Session=void;
Manager*(*manager)()=nullptr;Session*(*create)(Manager*,const int32_t*,size_t,int64_t)=nullptr;
int(*report)(Session*,int64_t)=nullptr;int(*target)(Session*,int64_t)=nullptr;void(*close_session)(Session*)=nullptr;
Session* session=nullptr;cpu_set_t original{};bool have_mask=false;int applied=-1;int64_t applied_target=0;
using Clock=std::chrono::steady_clock;Clock::time_point cpu_at;int64_t cpu_time=0;
int64_t Usage(){rusage usage{};if(getrusage(RUSAGE_SELF,&usage))return -1;
  return int64_t(usage.ru_utime.tv_sec+usage.ru_stime.tv_sec)*1000000+usage.ru_utime.tv_usec+usage.ru_stime.tv_usec;}
void Apply(){const int requested=affinity.load();if(requested==applied||!have_mask)return;cpu_set_t mask=original;
  if(requested==1||requested==3){std::array<int64_t,CPU_SETSIZE> frequencies{};int64_t maximum=0;
    for(int i=0;i<CPU_SETSIZE;i++)if(CPU_ISSET(i,&original)){std::ifstream file("/sys/devices/system/cpu/cpu"+std::to_string(i)+"/cpufreq/cpuinfo_max_freq");file>>frequencies[i];maximum=std::max(maximum,frequencies[i]);}
    if(maximum){CPU_ZERO(&mask);for(int i=0;i<CPU_SETSIZE;i++)if(CPU_ISSET(i,&original)&&frequencies[i]>0&&
      (requested==3?frequencies[i]==maximum:frequencies[i]>=maximum*8/10))CPU_SET(i,&mask);}
  }
  if(sched_setaffinity(0,sizeof(mask),&mask))__android_log_write(ANDROID_LOG_WARN,"SwanPerformance","Thread affinity unavailable; retaining scheduler mask.");
  applied=requested;
}
}
void SwanPerformance::Enable(bool value){enabled=value;}
void SwanPerformance::Affinity(int mode){affinity=mode==7?1:mode>=0&&mode<=3?mode:0;}
void SwanPerformance::Start(){have_mask=sched_getaffinity(0,sizeof(original),&original)==0;applied=-1;cpu_at=Clock::now();cpu_time=Usage();
  static void* library=dlopen("libandroid.so",RTLD_NOW|RTLD_LOCAL);
  if(library){manager=reinterpret_cast<decltype(manager)>(dlsym(library,"APerformanceHint_getManager"));create=reinterpret_cast<decltype(create)>(dlsym(library,"APerformanceHint_createSession"));
    report=reinterpret_cast<decltype(report)>(dlsym(library,"APerformanceHint_reportActualWorkDuration"));target=reinterpret_cast<decltype(target)>(dlsym(library,"APerformanceHint_updateTargetWorkDuration"));close_session=reinterpret_cast<decltype(close_session)>(dlsym(library,"APerformanceHint_closeSession"));}
  Apply();}
void SwanPerformance::Frame(int64_t work_ns,int64_t target_ns){Apply();
  if(!enabled){if(session&&close_session)close_session(session);session=nullptr;return;}
  if(!manager||!create||!report||!target||!close_session)return;
  target_ns=std::clamp<int64_t>(target_ns,1000000,100000000);
  if(!session){const int32_t tid=gettid();auto* owner=manager();if(owner)session=create(owner,&tid,1,target_ns);applied_target=target_ns;}
  if(!session)return;if(target_ns!=applied_target){target(session,target_ns);applied_target=target_ns;}
  report(session,std::max<int64_t>(1,work_ns));
}
void SwanPerformance::Close(){if(session&&close_session)close_session(session);session=nullptr;if(have_mask)sched_setaffinity(0,sizeof(original),&original);have_mask=false;}
double SwanPerformance::CpuPercent(){const auto now=Clock::now();const auto current=Usage();const auto elapsed=std::chrono::duration<double,std::micro>(now-cpu_at).count();
  const double value=cpu_time>=0&&current>=cpu_time&&elapsed>0?double(current-cpu_time)*100/elapsed:-1;cpu_at=now;cpu_time=current;return value;}
