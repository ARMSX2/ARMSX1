// SPDX-License-Identifier: GPL-3.0-or-later
#include "../native/swanstation/frame_pacer.h"
#include "../native/swanstation/audio_rate.h"
#include "../native/swanstation/frame_slots.h"
#include "../native/swanstation/presentation_feedback.h"
#include "../native/swanstation/presentation_order.h"
#include "../native/swanstation/frame_queue.h"
#include "../native/swanstation/present_recovery.h"
#include <future>
#include <cassert>
#include <iostream>
#include <limits>
int main() {
  using namespace SwanFramePacing;
  assert(std::abs(RefreshRatio(59.94,60,true)-60/59.94)<1e-10);
  assert(std::abs(RefreshRatio(59.94,120,true)-60/59.94)<1e-10);
  assert(RefreshRatio(50,60,true)==1 && RefreshRatio(60,90,true)==1);
  assert(RefreshRatio(60,120,false)==1);
  assert(RefreshRatio(0,60,true)==1 && RefreshRatio(60,std::numeric_limits<double>::quiet_NaN(),true)==1);
  Scheduler scheduler;
  Options options{true,true,true,true,2};
  int64_t now=1000000000;
  auto first=scheduler.Next(now,59.94,60,1,true,true,1,now,options);
  assert(first.period==16666666 && first.ratio>1);
  assert(first.start>now && first.start<first.present && first.present<first.finish);
  scheduler.Observe(8000000);
  auto second=scheduler.Next(first.finish,59.94,60,1,true,true,1,first.finish,options);
  assert(second.start<=second.present-8000000);
  const auto overrun=second.finish+4000000;
  auto recover=scheduler.Next(overrun,59.94,60,1,true,true,1,0,options);
  assert(recover.finish==second.finish+recover.period); // short overruns must not discard emulated time
  auto late=scheduler.Next(second.finish+500000000,59.94,60,1,true,true,1,0,options);
  assert(late.finish>second.finish+500000000); // no catch-up burst
  auto fast=scheduler.Next(late.finish,60,120,2,true,false,1,0,options);
  assert(fast.start==0 && fast.ratio==1 && fast.period==8333333);
  assert(scheduler.Next(now,60,60,1,false,true,1,now,options).period==0);
  auto sub=scheduler.Next(now,60,120,1,true,true,2,now,options);
  assert(sub.start==0 && sub.display>now && sub.display<sub.finish);
  assert(sub.present==sub.display-2000000);
  Options legacy;
  assert(legacy.optimal&&!legacy.match_refresh&&!legacy.pre_sleep);
  legacy.optimal=false;
  assert(scheduler.Next(now,60,60,1,true,true,1,now,legacy).present==0);
  Scheduler without_vsync;
  options.vsync=false;
  auto no_vsync=without_vsync.Next(now,60,60,1,true,true,1,now,options);
  assert(no_vsync.start<=no_vsync.present-4000000);
  assert(no_vsync.display==0);
  Scheduler headroom_small,headroom_large;
  Options small{true,true,false,false,2},large=small;large.buffer_ms=8;
  const auto target_small=headroom_small.Next(now,60,60,1,true,true,1,now,small);
  const auto target_large=headroom_large.Next(now,60,60,1,true,true,1,now,large);
  assert(target_small.display==target_large.display);
  assert(target_small.present-target_large.present==6000000);
  assert(SafeTimestamp(target_small.display,now)==uint64_t(target_small.display));
  assert(SafeTimestamp(now+300000000,now)==0);
  assert(SafeTimestamp(now-300000000,now)==0);
  assert(SafeTimestamp(0,now)==0);
  // An early ready buffer needs the same target regardless of CPU headroom.
  const int64_t tick=16666666;
  auto compositor=[&](int64_t ready,int64_t requested){return ((std::max(ready,requested)+tick-1)/tick)*tick;};
  assert(compositor(target_small.present,target_small.display)==compositor(target_large.present,target_large.display));
  // Exact-boundary requests slip a refresh with small positive phase error.
  // A not-before request in the preceding half-interval still selects the
  // intended slot, for both signs of estimation error and multiple headrooms.
  const int64_t intended=tick*100;
  for(const int64_t error:{-200000,0,200000}) {
    const auto estimate=intended+error;
    for(const int64_t headroom:{2000000,8000000}) {
      const auto ready=estimate-headroom;
      const auto request=DisplaySlotTimestamp(estimate,ready,tick);
      assert(compositor(ready,int64_t(request))==intended);
    }
  }
  assert(compositor(intended-2000000,intended+200000)==intended+tick);
  assert(DisplaySlotTimestamp(intended,intended-2000000,0)==uint64_t(intended));
  assert(DisplaySlotTimestamp(0,now,tick)==0);
  Feedback feedback;
  feedback.Reset(1000);
  feedback.Record(1,900,900,10); // stale pre-resume sample
  feedback.Record(2,1010,1000,10);
  feedback.Record(3,1020,1010,10);
  feedback.Record(3,1020,1010,10); // duplicate driver sample
  feedback.Record(4,1040,1020,10); // an extra unrequested refresh
  auto recorded=feedback.Read();
  assert(recorded.frames==3 && recorded.gaps==2 && recorded.maximum==20 && recorded.late==1);
  feedback.Reset(2000);
  feedback.Record(4,1040,1020,10);
  feedback.Record(5,2010,2000,10);
  feedback.Record(6,2030,2020,10); // requested PAL pulldown is not a missed deadline
  assert(feedback.Read().frames==2 && feedback.Read().late==0 && feedback.Read().maximum==20);
  feedback.Record(10,2070,2060,10); // unavailable IDs must not report 40 ns jank
  recorded=feedback.Read();
  assert(recorded.missing==3 && recorded.gaps==1 && recorded.maximum==20);
  feedback.Record(11,2080,2070,10);
  assert(feedback.Read().gaps==2 && feedback.Read().sum==30);
  feedback.Record(12,2080,2080,10); // different buffers can share a scanout
  assert(feedback.Read().coalesced==1 && feedback.Read().last==2080);
  feedback.Record(11,2080,2070,10); // out-of-order repeated history
  assert(feedback.Read().frames==5);
  feedback.Reset(3000);
  feedback.Record(0xffffffffu,3010,3000,10);
  feedback.Record(0,3020,3010,10);
  assert(feedback.Read().gaps==1 && feedback.Read().missing==0);

  // A frame needs 6 ms of GPU time, with only 2 ms between the submission
  // deadline and scanout. Waiting before submit misses; rendering during the
  // available idle budget makes the slot. Exercise the production ordering.
  int64_t cpu=0,gpu_done=0,displayed=0;
  assert(PresentPreparedFrame([&]{gpu_done=cpu+6000000;return true;},
    [&]{cpu=std::max(cpu,tick-2000000);},
    [&]{displayed=compositor(std::max(cpu,gpu_done),tick);return true;}));
  assert(displayed==tick);
  const auto old_gpu_done=tick-2000000+6000000;
  assert(compositor(old_gpu_done,tick)==tick*2);
  bool waited=false,presented=false;
  assert(!PresentPreparedFrame([]{return false;},[&]{waited=true;},[&]{presented=true;return true;}));
  assert(!waited&&!presented); // failed submissions never reach presentation
  assert(ChoosePresentMode(true,true,true,true)==PresentMode::Fifo);
  assert(ChoosePresentMode(true,false,true,true)==PresentMode::Mailbox);
  assert(ChoosePresentMode(true,false,false,true)==PresentMode::Fifo); // never tear with VSync on
  assert(ChoosePresentMode(false,false,true,true)==PresentMode::Mailbox);
  assert(ChoosePresentMode(false,false,false,true)==PresentMode::Immediate);
  assert(ChoosePresentMode(false,false,false,false)==PresentMode::Fifo);
  assert(MatchedCadence(60,120)&&MatchedCadence(60,60));
  assert(!MatchedCadence(59.817333,60)&&!MatchedCadence(50,60)&&!MatchedCadence(60,0));
  PresentRecovery recovery;
  const int64_t period=16000000;
  PresentRequest request{period,period-2000000,period,2,true};
  recovery.Reset();assert(!recovery.Skip(request,1000000));
  recovery.Submitted(1,request,request.deadline);
  recovery.Observe(1,period*2);
  assert(recovery.ExtraMargin()>0&&recovery.ExtraMargin()<=period/4-2000000);
  const auto adjusted=recovery.ExtraMargin();
  request.target+=period;request.deadline+=period;
  recovery.Submitted(2,request,request.deadline+2000000);
  recovery.Observe(2,request.target+period);
  assert(recovery.ExtraMargin()==adjusted); // a late CPU submit must not inflate margin
  request.target+=period;request.deadline+=period;
  assert(recovery.Skip(request,request.deadline)); // two late feedback samples arm one recovery
  request.target+=period;request.deadline+=period;
  assert(!recovery.Skip(request,request.target+period)); // always present after a recovery
  recovery.Reset();
  request={period,period-2000000,period,8,true};
  recovery.Policy(request);recovery.Submitted(1,request,request.deadline);recovery.Observe(1,period*2);
  assert(recovery.ExtraMargin()==0); // preserve a user's already-large margin
  recovery.Reset();request={period,period-2000000,period,2,true};
  // A single missed FIFO refresh otherwise leaves every following frame one
  // refresh late. Skipping presentation drains the backlog without skipping
  // any of the 60 emulation steps.
  int64_t presenter=0;unsigned simulated=0,presents=0;
  for(int i=1;i<=60;++i) {
    ++simulated;request.target=i*period;request.deadline=request.target-2000000;
    presenter=std::max(presenter,request.deadline);
    if(recovery.Skip(request,presenter))continue;
    ++presents;presenter=request.target+(i==1?period:0);
    recovery.Presented(request,presenter);
  }
  assert(simulated==60&&presents==59&&recovery.Drops()==1&&presenter==period*60);
  recovery.Reset();bool skipped=false;unsigned skipped_count=0;
  for(int i=1;i<=240;++i) {
    request.target=i*period;request.deadline=request.target-2000000;
    const bool next=recovery.Skip(request,request.target+period);
    assert(!next||!skipped);skipped=next;skipped_count+=next?1:0;
  }
  assert(skipped_count<=30); // cooldown bounds repeated overload recovery
  request.enabled=false;assert(!recovery.Skip(request,request.target+period*100));
  assert(recovery.ExtraMargin()==0);
  request.enabled=true;request.allow_drop=false;
  assert(!recovery.Skip(request,request.target+period*100)); // no drop for multi-refresh cadence
  recovery.Reset(period*1000);recovery.Observe(123,period);assert(recovery.LastActual()==0);
  recovery.Reset();request={period,period-2000000,period,2,true};
  for(unsigned id=1;id<=100;++id) {
    request.target=id*period;request.deadline=request.target-2000000;
    recovery.Policy(request);recovery.Submitted(id,request,request.deadline);
    recovery.Observe(id,request.target+period);
  }
  assert(recovery.ExtraMargin()==period/4-2000000); // growth is bounded under persistent misses
  request.target+=period;request.deadline+=period;
  assert(recovery.Skip(request,request.deadline));
  recovery.Observe(100,period*200); // delayed pre-recovery feedback cannot re-arm it
  request.target+=period*16;request.deadline+=period*16;
  assert(!recovery.Skip(request,request.deadline));
  request.target+=period;request.deadline+=period;
  assert(!recovery.Skip(request,request.deadline));

  // Rate conversion must retain phase across arbitrary libretro batch boundaries.
  std::vector<int16_t> input(44100*2);
  for(size_t i=0;i<input.size();++i)input[i]=int16_t(i%30000);
  SwanAudio::RefreshResampler whole,split;
  const auto expected=whole.Process(input.data(),44100,60/59.94);
  std::vector<int16_t> actual;
  for(size_t offset=0;offset<44100;) {
    size_t count=std::min(size_t(137),44100-offset);
    const auto& part=split.Process(input.data()+offset*2,count,60/59.94);
    actual.insert(actual.end(),part.begin(),part.end());offset+=count;
  }
  assert(actual==expected && std::abs(int(actual.size()/2)-44056)<=1);
  whole.Reset();assert(whole.Process(input.data(),44100,1)==input);
  whole.Reset();assert(whole.Process(input.data(),44100,0.99).size()>input.size());
  // Accumulated conversion remains bounded over a minute, not just one batch.
  whole.Reset();size_t samples=0;
  for(int i=0;i<60;++i)samples+=whole.Process(input.data(),44100,60/59.94).size()/2;
  assert(std::abs(double(samples)-60*44100/(60/59.94))<=1);

  SwanFrames::Slots slots;slots.Reset(2);
  slots.Submitted();slots.Advance();assert(slots.Pending(0)&&!slots.Pending(1));
  slots.Submitted();slots.Advance();assert(slots.Current()==0&&slots.Pending(0));
  slots.Retired(0);assert(!slots.Pending(0)&&slots.Pending(1));
  slots.Reset(3);assert(slots.Count()==3&&!slots.Pending(1));
  slots.Reset(0);assert(slots.Count()==1);
  for(unsigned capacity:{1u,2u,3u}) {
    SwanRender::FrameQueue queued;queued.Reset(capacity);
    std::vector<std::shared_ptr<SwanRender::FrameQueue::Ticket>> tickets;
    for(unsigned i=0;i<capacity;++i)tickets.push_back(queued.Acquire());
    auto blocked=std::async(std::launch::async,[&]{return queued.Acquire();});
    assert(blocked.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
    tickets.pop_back();auto admitted=blocked.get();
    assert(queued.Pending()==capacity&&queued.Peak()==capacity);
    admitted.reset();tickets.clear();assert(queued.Pending()==0);
    queued.Reset(capacity);
  }

  // Reproduce Thor's fast CPU/Vulkan path: rendering takes 2.5ms, present
  // returns asynchronously, and the OS wakes slightly after a timer deadline.
  // A full-minute trace with nonmatching clocks catches cumulative phase loss
  // that single-deadline tests cannot expose (the old code ran 59.817 at 58.8,
  // and PAL 50 at 30, despite abundant rendering headroom).
  for(double game:{60.0,59.817333,50.0})for(bool latency:{false,true}) {
    Scheduler traced;Options traced_options{true,true,false,latency,2};
    int64_t clock=1000000000,begin=clock;
    const double tick=1e9/60;
    for(int frame=0;frame<3600;++frame) {
      const auto anchor=int64_t(std::floor(clock/tick)*tick);
      const auto plan=traced.Next(clock,game,60,1,true,true,1,anchor,traced_options);
      clock=std::max(clock,plan.start)+2500000;
      clock=std::max(clock,plan.present)+350000;
      clock=std::max(clock,plan.finish)+70000;
      traced.Observe(2500000);
    }
    const double actual=3600e9/double(clock-begin);
    assert(std::abs(actual-game)/game<0.001);
  }
  std::cout<<"Pacing deadlines, refresh matching, latency scheduling, streaming audio and frame-slot ownership passed\n";
}
