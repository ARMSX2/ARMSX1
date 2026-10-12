// SPDX-License-Identifier: GPL-3.0-or-later
// Public ABI extensions built against the pinned, GPL SwanStation source.
#include "libretro.h"
#include "common/cd_image.h"
#include "core/system.h"
#include "core/memory_card.h"
#include "core/pad.h"
#include "core/cheats.h"
#include "core/settings.h"
#include "core/texture_replacements.h"
#include "common/vulkan/context.h"
#include "xxhash.h"
#include "png.h"
#include <filesystem>
#include <set>
#include <algorithm>
#include <cstring>
#include <cctype>

namespace { bool texture_dump=false,texture_replace=false;std::string texture_directory;std::set<std::string> dumped;uint64_t dumped_bytes=0;
uint64_t texture_lookups=0,texture_writes=0,texture_hits=0;
}
void swan_replacement_loaded(){texture_hits++;}
extern "C" RETRO_API uint64_t swan_texture_metric(unsigned index){return index==0?texture_dump:index==1?g_settings.texture_replacements.enable_vram_write_replacements:index==2?texture_lookups:index==3?texture_writes:index==4?texture_hits:0;}
std::string swan_texture_directory(const std::string& game) {
  if(texture_directory.empty())return {};
  return texture_directory+(texture_replace?"/replacements":"/.disabled-replacements");
}
void swan_dump_vram(uint32_t width,uint32_t height,const void* data) {
  texture_lookups++;
  if(!texture_dump||!data||!width||!height||width>1024||height>512||dumped.size()>=512||dumped_bytes>=64*1024*1024)return;
  auto hash=XXH3_128bits(data,size_t(width)*height*2);char name[64];snprintf(name,sizeof(name),"vram-write-%016llx%016llx.png",(unsigned long long)hash.high64,(unsigned long long)hash.low64);
  if(!dumped.insert(name).second)return;
  const auto dir=texture_directory+"/dump";
  std::error_code error;std::filesystem::create_directories(dir,error);if(error)return;
  const auto path=dir+"/"+name;if(std::filesystem::exists(path,error))return;
  std::vector<uint32_t> rgba(size_t(width)*height);const auto* pixels=static_cast<const uint16_t*>(data);
  for(size_t i=0;i<rgba.size();i++){const auto p=pixels[i];auto expand=[](unsigned v){return (v<<3)|(v>>2);};
    rgba[i]=expand(p&31)|(expand((p>>5)&31)<<8)|(expand((p>>10)&31)<<16)|((p&0x8000)?0xff000000:0);}
  const auto temp=path+".writing";
  if(WritePng(temp.c_str(),width,height,4,rgba.data(),width*4)){std::filesystem::rename(temp,path,error);if(!error){dumped_bytes+=std::filesystem::file_size(path,error);texture_writes++;}}
  std::filesystem::remove(temp,error);
}
extern "C" RETRO_API void swan_texture_options(bool dump,bool replace,const char* directory) {
  texture_dump=dump;texture_replace=replace;texture_directory=directory?directory:"";dumped.clear();dumped_bytes=0;
  texture_lookups=texture_writes=texture_hits=0;
  if(!texture_directory.empty()&&(dump||replace)){std::error_code error;std::filesystem::create_directories(texture_directory+"/dump",error);std::filesystem::create_directories(texture_directory+"/replacements",error);}
  g_settings.texture_replacements.enable_vram_write_replacements=dump||replace;
  g_texture_replacements.Reload();
}
extern "C" RETRO_API void swan_vulkan_abort() { if(g_vulkan_context)Vulkan::Context::Destroy();Vulkan::ResetVulkanLibraryFunctionPointers(); }

extern "C" RETRO_API bool swan_disc_serial(const char* path, char* output, size_t capacity) {
  if (!path || !output || capacity < 11) return false;
  output[0] = 0;
  auto image = CDImage::Open(path, CDImage::OpenFlags::None, nullptr);
  if (!image) return false;
  const auto serial = System::GetGameCodeForImage(image.get(), false);
  // No filename guessing or hash masquerading as a region-specific retail serial.
  std::string compact;
  for (unsigned char c : serial) if (std::isalnum(c)) compact += std::toupper(c);
  if (compact.size() != 9 || !std::all_of(compact.begin(), compact.begin()+4, [](unsigned char c){return std::isalpha(c);}) ||
      !std::all_of(compact.begin()+4, compact.end(), [](unsigned char c){return std::isdigit(c);})) return false;
  compact.insert(4, "-");
  if (compact.size()+1 > capacity) return false;
  std::memcpy(output, compact.c_str(), compact.size()+1);
  return true;
}

extern "C" RETRO_API uint64_t swan_card_digest() {
  uint64_t hash = 14695981039346656037ull;
  for (unsigned slot=0; slot<2; ++slot) {
    const auto* card = g_pad.GetMemoryCard(slot);
    hash = (hash ^ (card ? slot+1 : 0)) * 1099511628211ull;
    if (card) for (uint8_t byte : card->GetData()) hash = (hash ^ byte) * 1099511628211ull;
  }
  return hash;
}

extern "C" RETRO_API bool swan_card_path(unsigned slot, char* output, size_t capacity) {
  if (slot >= 2 || !output || !capacity || !System::IsValid()) return false;
  const auto* card = g_pad.GetMemoryCard(slot);
  if (!card || card->GetFilename().size()+1 > capacity) return false;
  std::memcpy(output, card->GetFilename().c_str(), card->GetFilename().size()+1);
  return true;
}

extern "C" RETRO_API bool swan_validate_cheat(const char* text) {
  CheatCode code;
  return text && CheatList::ParseLibretroCheat(&code, text);
}
extern "C" RETRO_API uint32_t swan_guest_frame() { return System::GetFrameNumber(); }

// Disc reader ABI for the official rcheevos PlayStation content hash (including CHD).
struct SwanHashDisc { std::unique_ptr<CDImage> image; uint32_t start; };
extern "C" RETRO_API void* swan_hash_open(const char* path,uint32_t track) {
  auto image=CDImage::Open(path,CDImage::OpenFlags::None,nullptr);if(!image)return nullptr;
  const auto& tracks=image->GetTracks();if(tracks.empty())return nullptr;
  const CDImage::Track* chosen=nullptr;
  if(track==uint32_t(-1)){for(const auto& t:tracks)if(t.mode!=CDImage::TrackMode::Audio){chosen=&t;break;}}
  else if(track==uint32_t(-2))chosen=&tracks.back();
  else if(track==uint32_t(-3))chosen=&*std::max_element(tracks.begin(),tracks.end(),[](const auto& a,const auto& b){return a.length<b.length;});
  else for(const auto& t:tracks)if(t.track_number==track){chosen=&t;break;}
  if(!chosen)return nullptr;
  // rcheevos numbers sectors from 00:02:00; CDImage includes the 150-sector lead-in.
  const auto first=chosen->start_lba>=150?chosen->start_lba-150:0;
  return new SwanHashDisc{std::move(image),first};
}
extern "C" RETRO_API uint32_t swan_hash_first(void* handle){return static_cast<SwanHashDisc*>(handle)->start;}
extern "C" RETRO_API void swan_hash_close(void* handle){delete static_cast<SwanHashDisc*>(handle);}
extern "C" RETRO_API size_t swan_hash_read(void* handle,uint32_t sector,void* output,size_t bytes) {
  if(!handle||!output||bytes>1024*1024)return 0;
  auto& disc=*static_cast<SwanHashDisc*>(handle);size_t count=0;
  if(sector>UINT32_MAX-150||!disc.image->Seek(sector+150))return 0;
  while(count<bytes){uint8_t block[2352];if(disc.image->Read(CDImage::ReadMode::RawSector,1,block)!=1)break;
    const unsigned offset=block[15]==1?16:24;
    const size_t length=std::min<size_t>(2048,bytes-count);std::memcpy(static_cast<uint8_t*>(output)+count,block+offset,length);count+=length;}
  return count;
}
