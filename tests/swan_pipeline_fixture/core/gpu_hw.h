// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A deterministic driver double for testing the real gpu_pipeline.cpp on a PC.
// The Android build always uses the pinned core's actual GPU_HW header.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
namespace SwanGpuPipeline { bool OnBackend(); }
namespace Common { template<class T> struct Rectangle {
  T left{},top{},right{},bottom{};
  T GetWidth() const {return right-left;} T GetHeight() const {return bottom-top;}
}; }
struct HostDisplay {};
struct HostDisplayTexture {};
struct StateWrapper {bool reading=false; std::vector<uint16_t> vram;};
class GPU {
public:
  static constexpr uint32_t VRAM_WIDTH=1024,VRAM_HEIGHT=512;
  struct GPUSTAT {uint32_t bits=0;};
  struct DrawMode {bool texture_window_changed=false; void SetTexturePageChanged() {}};
  struct DrawingOffset {int x=0,y=0;};
  struct CRTCState {int field=0;unsigned display_width=320,display_height=240;};
  float GetDisplayAspectRatio() const {return 4.f/3;}
  GPUSTAT m_GPUSTAT;
  DrawMode m_draw_mode;
  Common::Rectangle<uint32_t> m_drawing_area;
  DrawingOffset m_drawing_offset;
  CRTCState m_crtc_state;
  bool m_console_is_pal=false,m_force_progressive_scan=false,m_force_ntsc_timings=false;
  bool m_drawing_area_changed=false;
  uint16_t* m_vram_ptr=nullptr;
  virtual ~GPU()=default;
  virtual bool Initialize(HostDisplay*) {return true;}
  virtual void Reset(bool) {}
  virtual bool DoState(StateWrapper&,HostDisplayTexture**,bool)=0;
  virtual void UpdateSettings() {}
  virtual void ResetGraphicsAPIState()=0;
  virtual void RestoreGraphicsAPIState()=0;
  virtual void ReadVRAM(uint32_t,uint32_t,uint32_t,uint32_t)=0;
  virtual void FillVRAM(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t)=0;
  virtual void UpdateVRAM(uint32_t,uint32_t,uint32_t,uint32_t,const void*,bool,bool)=0;
  virtual void CopyVRAM(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t)=0;
  virtual void ClearDisplay()=0;
  virtual void UpdateDisplay()=0;
};
class GPU_HW : public GPU {
public:
  static constexpr size_t VERTEX_BUFFER_SIZE=4*1024*1024;
  struct BatchVertex {uint32_t color=0;};
  struct BatchConfig {int mode=0;};
  struct BatchUBOData {int mode=0;};
  enum class BatchRenderMode {Opaque,OnlyOpaque,OnlyTransparent};
  std::array<uint16_t,VRAM_WIDTH*VRAM_HEIGHT> m_vram_shadow{};
  std::unique_ptr<int> m_sw_renderer;
  BatchVertex *m_batch_start_vertex_ptr=nullptr,*m_batch_current_vertex_ptr=nullptr,*m_batch_end_vertex_ptr=nullptr;
  uint32_t m_batch_base_vertex=0;
  BatchConfig m_batch;
  BatchUBOData m_batch_ubo_data;
  Common::Rectangle<uint32_t> m_vram_dirty_rect;
  int32_t m_current_depth=1;
  float m_last_depth_z=1;
  uint32_t m_resolution_scale=1,m_multisamples=1,m_max_resolution_scale=16,m_max_multisamples=4;
  uint32_t m_render_api=0,m_texture_filtering=0,m_downsample_mode=0,m_shader_precompile_mode=0;
  uint8_t bits=0;
  bool m_true_color=true,m_using_uv_limits=false,m_pgxp_depth_buffer=false,m_batch_ubo_dirty=true;
  GPU_HW() {m_vram_ptr=m_vram_shadow.data();}
  bool DoState(StateWrapper& sw,HostDisplayTexture** tex,bool update) override {
    assert(!tex); // Internal states must use owned RAM, never cross-owner textures.
    if(sw.reading) {Reset(true); UpdateVRAM(0,0,VRAM_WIDTH,VRAM_HEIGHT,sw.vram.data(),false,false);}
    else {ReadVRAM(0,0,VRAM_WIDTH,VRAM_HEIGHT); sw.vram.assign(m_vram_ptr,m_vram_ptr+VRAM_WIDTH*VRAM_HEIGHT);}
    if(update)UpdateDisplay();
    return true;
  }
  void Reset(bool) override {m_batch={};m_batch_ubo_data={};m_current_depth=1;}
  void FillVRAM(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t) override {}
  void UpdateVRAM(uint32_t,uint32_t,uint32_t,uint32_t,const void*,bool,bool check) override {if(check)++m_current_depth;}
  void CopyVRAM(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t) override {}
  virtual void UpdateVRAMReadTexture() {}
  void UpdateSoftwareRenderer(bool) {}
  void ReadSoftwareRendererVRAM(uint32_t,uint32_t,uint32_t,uint32_t) {}
  void FillSoftwareRendererVRAM(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t) {}
  void UpdateSoftwareRendererVRAM(uint32_t,uint32_t,uint32_t,uint32_t,const void*,bool,bool) {}
  void CopySoftwareRendererVRAM(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t) {}
  Common::Rectangle<uint32_t> GetVRAMTransferBounds(uint32_t x,uint32_t y,uint32_t w,uint32_t h) {return {x,y,x+w,y+h};}
  void IncludeVRAMDirtyRectangle(Common::Rectangle<uint32_t> value) {m_vram_dirty_rect=value;}
  virtual void UpdateDepthBufferFromMaskBit()=0;
  virtual void ClearDepthBuffer()=0;
  virtual void SetScissorFromDrawingArea()=0;
  virtual void MapBatchVertexPointer(uint32_t)=0;
  virtual void UnmapBatchVertexPointer(uint32_t)=0;
  virtual void UploadUniformBuffer(const void*,uint32_t)=0;
  virtual void DrawBatchVertices(BatchRenderMode,uint32_t,uint32_t)=0;
  void FlushRender() {
    if(!m_batch_current_vertex_ptr)return;
    const auto count=uint32_t(m_batch_current_vertex_ptr-m_batch_start_vertex_ptr);
    UnmapBatchVertexPointer(count);
    if(!count)return;
    if(m_batch_ubo_dirty) {
      UploadUniformBuffer(&m_batch_ubo_data,sizeof(m_batch_ubo_data));m_batch_ubo_dirty=false;
    }
    DrawBatchVertices(BatchRenderMode::Opaque,m_batch_base_vertex,count);
  }
  void Emit(uint32_t color,int mode,bool two_pass=false) {
    MapBatchVertexPointer(1);
    m_GPUSTAT.bits=color; m_batch.mode=mode;
    m_batch_ubo_dirty |= m_batch_ubo_data.mode!=mode; m_batch_ubo_data.mode=mode;
    m_batch_current_vertex_ptr->color=color; ++m_batch_current_vertex_ptr;
    if(!two_pass) {FlushRender();return;}
    // Mirror the pinned core's two-pass FlushRender sequence: upload once,
    // then draw both passes with different uniforms and the same vertex base.
    UnmapBatchVertexPointer(1);
    UploadUniformBuffer(&m_batch_ubo_data,sizeof(m_batch_ubo_data));
    DrawBatchVertices(BatchRenderMode::OnlyOpaque,m_batch_base_vertex,1);
    m_batch_ubo_data.mode=mode+100;
    UploadUniformBuffer(&m_batch_ubo_data,sizeof(m_batch_ubo_data));
    DrawBatchVertices(BatchRenderMode::OnlyTransparent,m_batch_base_vertex,1);
  }
};
