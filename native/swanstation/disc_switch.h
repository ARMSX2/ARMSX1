// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace SwanDisc {
enum class Result { Unchanged, Changed, Invalid, FailedRestored, FailedUnrestored };
struct Callbacks {
  bool (*eject)(bool);
  bool (*is_ejected)();
  bool (*select)(unsigned);
  unsigned (*count)();
  int (*mounted_index)();
};
// Emulation-thread only. PBP subimages use indices, never synthetic file paths.
inline Result Switch(const Callbacks& disk,int requested) {
  if(!disk.eject||!disk.is_ejected||!disk.select||!disk.count||!disk.mounted_index)
    return Result::Invalid;
  const unsigned count=disk.count();const int old=disk.mounted_index();
  if(count<2||count>5||requested<0||unsigned(requested)>=count||old<0||unsigned(old)>=count||disk.is_ejected())
    return Result::Invalid;
  if(old==requested)return Result::Unchanged;
  if(!disk.eject(true))return Result::FailedRestored;
  if(disk.select(unsigned(requested))&&disk.eject(false)&&disk.mounted_index()==requested)
    return Result::Changed;
  // Verify actual mounted media: the pinned PBP reader can report success
  // after falling back to an old subimage when a requested disc is corrupt.
  if(!disk.is_ejected()&&!disk.eject(true))return Result::FailedUnrestored;
  if(!disk.select(unsigned(old))||!disk.eject(false)||disk.mounted_index()!=old)
    return Result::FailedUnrestored;
  return Result::FailedRestored;
}
}
