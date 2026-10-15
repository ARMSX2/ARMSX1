// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

// Independent of Android so session isolation, bounds and timing can be host-tested.
// The journal keeps a bounded tail in memory. Disk checkpoints occur at session transitions
// and once per two seconds, not once per frame. Exports use the locked in-memory snapshot.
namespace SwanDiagnostics {
inline std::string Quote(std::string text) {
  for(char& c:text)if(static_cast<unsigned char>(c)<32)c=' ';
  std::ostringstream out;out<<std::quoted(text);return out.str();
}
class Journal {
 public:
  void Begin(const std::string& directory,const std::string& session,const std::string& build,
             const std::string& source) {
    std::lock_guard lock(mutex_);
    lines_.clear();bytes_=0;start_=Clock::now();ResetTimingLocked();state_="booting";
    context_.clear();details_.clear();core_count_=0;core_second_=last_error_second_=-1;session_=session;header_="ARMSX_SESSION schema=1 core=SwanStation session="+Quote(session)+
      " build="+Quote(build)+" source="+Quote(source.substr(0,4096))+"\n";
    path_=directory+"/swan-session.log";disk_ok_=true;
    std::error_code error;std::filesystem::create_directories(directory,error);
    if(error)disk_ok_=false;
    if(disk_ok_ && std::filesystem::exists(path_,error)) {
      std::filesystem::remove(directory+"/swan-session.previous.log",error);error.clear();
      std::filesystem::rename(path_,directory+"/swan-session.previous.log",error);
      if(error)disk_ok_=false;
    }
    AddLocked("session_started");FlushLocked();
  }
  void State(const std::string& state,const std::string& detail={}) {
    std::lock_guard lock(mutex_);if(session_.empty())return;
    if(count_)EmitLocked(details_.empty()?context_:details_);
    state_=state;if(!detail.empty())context_=detail;ResetTimingLocked();AddLocked("state="+state+" "+detail);FlushLocked();
  }
  void Core(bool error,const std::string& message) {
    std::lock_guard lock(mutex_);if(session_.empty())return;
    // Cap bursts as well as total bytes. Diagnostics must not amplify an error loop.
    const auto second=std::chrono::duration_cast<std::chrono::seconds>(Clock::now()-start_).count();
    if(second!=core_second_){core_second_=second;core_count_=0;}
    const bool first_error=error && last_error_second_!=second;
    if(++core_count_>64 && !first_error)return;
    if(first_error)last_error_second_=second;
    // Persist warnings/errors, not the core's potentially per-sector INFO/DEBUG trace.
    AddLocked(std::string(error?"core_error ":"core_warning ")+Quote(message.substr(0,1024)));
    if(first_error)FlushLocked();
  }
  // Emits at a bounded cadence even with the on-screen statistics switched off.
  void Frame(double frame,double emu,double present,double idle,double setup,
             double budget,const std::string& details) {
    std::lock_guard lock(mutex_);if(session_.empty())return;
    if(!details.empty())details_=details;
    count_++;frame_+=frame;emu_+=emu;present_+=present;idle_+=idle;setup_+=setup;
    worst_=std::max(worst_,frame);worst_emu_=std::max(worst_emu_,emu);
    if(frame>budget*1.25)late_++;
    if(frame_<2000)return;
    EmitLocked(details);ResetTimingLocked();FlushLocked();
  }

  std::string Snapshot() const {
    std::lock_guard lock(mutex_);return SnapshotLocked();
  }
 private:
  using Clock=std::chrono::steady_clock;
  static constexpr size_t Limit=64*1024;
  mutable std::mutex mutex_;
  std::deque<std::string> lines_;
  std::string header_,session_,state_,path_,context_,details_;
  int64_t core_second_=-1,last_error_second_=-1;unsigned core_count_=0;
  size_t bytes_=0;bool disk_ok_=true;
  Clock::time_point start_{};
  double frame_=0,emu_=0,present_=0,idle_=0,setup_=0,worst_=0,worst_emu_=0;
  unsigned count_=0,late_=0;
  void EmitLocked(const std::string& details) {
    std::ostringstream out;out<<std::fixed<<std::setprecision(3)
      <<"timing frames="<<count_<<" emulated_fps="<<1000.0*count_/frame_
      <<" frame_ms="<<frame_/count_<<"/"<<worst_
      <<" emulation_block_ms="<<emu_/count_<<"/"<<worst_emu_
      <<" present_callback_ms="<<present_/count_<<" pacer_wait_ms="<<idle_/count_
      <<" host_setup_ms="<<setup_/count_<<" late_frames="<<late_<<" "<<details
      <<" (CPU wall timing; emulation_block includes present callback; not GPU execution time)";
    AddLocked(out.str());
  }
  void ResetTimingLocked(){frame_=emu_=present_=idle_=setup_=worst_=worst_emu_=0;count_=late_=0;}
  void AddLocked(const std::string& text) {
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start_).count();
    std::string line="[elapsed_ms="+std::to_string(ms)+"] "+text.substr(0,4096)+"\n";
    bytes_+=line.size();lines_.push_back(std::move(line));
    while(bytes_>Limit && !lines_.empty()){bytes_-=lines_.front().size();lines_.pop_front();}
  }
  std::string SnapshotLocked() const {
    if(session_.empty())return {};
    std::string out=header_+"latest_state="+state_+" disk_checkpoint="+(disk_ok_?"available":"unavailable")+
      "\nlast_runtime_context="+context_+"\n";
    for(const auto& line:lines_)out+=line;
    return out;
  }
  void FlushLocked() {
    if(path_.empty() || !disk_ok_)return;
    // Atomic replacement keeps last session recoverable if a process dies during a write.
    const std::string temporary=path_+".pending";
    std::ofstream stream(temporary,std::ios::binary|std::ios::trunc);stream<<SnapshotLocked();
    stream.close();
    if(!stream){disk_ok_=false;return;}
    // filesystem::rename also replaces the destination on Windows host tests;
    // the Windows CRT's std::rename rejects an existing checkpoint.
    std::error_code error;std::filesystem::rename(temporary,path_,error);
    if(error)disk_ok_=false;
  }
};
}
