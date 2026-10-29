// SPDX-License-Identifier: GPL-3.0-or-later
// Independent frontend/backend split for the pinned SwanStation renderers.
// Guest MMIO, FIFO decoding, timing and PGXP lookups remain on the CPU thread.
// Hardware resource management, vertex upload, draw execution, VRAM operations
// and shader/pipeline preparation run against a separate backend object.
#include "gpu_pipeline.h"
#include "frame_queue.h"
#include "render_task.h"
#include "core/gpu_hw.h"
#include <atomic>
#include <array>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
SwanGpuPipeline::Dispatch dispatch = nullptr;
thread_local bool backend_scope = false;
std::atomic<unsigned> live{0};
std::atomic<bool> failed{false};
std::atomic<bool> queue_frames{false};
SwanRender::FrameQueue frame_queue;
SwanGpuPipeline::DisplayGeometry geometry;
struct Scope {
  bool old = backend_scope;
  Scope() { backend_scope = true; }
  ~Scope() { backend_scope = old; }
};
struct Work {
  SwanRender::Task<512> run;
  std::vector<unsigned char> payload;
  size_t pool_index=size_t(-1);
};

// The CPU owner acquires packets and the backend returns accepted packets.
// Rejected packets return to a CPU-only stack, so the free ring remains SPSC
// even while a dispatch fails concurrently with normal backend completions.
// The existing dispatcher still owns ordering, byte limits and completion.
class PacketPool {
  static constexpr size_t capacity=512,ring_size=capacity+1;
  std::array<Work,capacity> packets;
  std::array<size_t,ring_size> free;
  alignas(64) std::atomic<size_t> read{0};
  alignas(64) std::atomic<size_t> write{capacity};
  std::array<size_t,capacity> rejected;
  size_t rejected_count=0;
  SwanGpuPipeline::PacketMetrics metrics;
  static void Clear(Work* work) {
    work->run.Reset();
    // Bound idle payload retention to 8 MiB across the pool. Large one-off
    // VRAM uploads must not permanently enlarge every reused command slot.
    if(work->payload.capacity()>16*1024)std::vector<unsigned char>().swap(work->payload);
    else work->payload.clear();
  }
public:
  PacketPool() {for(size_t i=0;i<capacity;++i){packets[i].pool_index=i;free[i]=i;}}
  Work* Acquire() {
    if(rejected_count){++metrics.pooled;return &packets[rejected[--rejected_count]];}
    const size_t cursor=read.load(std::memory_order_relaxed);
    if(cursor!=write.load(std::memory_order_acquire)) {
      auto* packet=&packets[free[cursor]];
      read.store((cursor+1)%ring_size,std::memory_order_release);
      ++metrics.pooled;return packet;
    }
    ++metrics.heap_fallbacks;return new Work;
  }
  void Recycle(Work* work) {
    const size_t index=work->pool_index;
    if(index==size_t(-1)){delete work;return;}
    Clear(work);
    const size_t cursor=write.load(std::memory_order_relaxed);
    free[cursor]=index;
    write.store((cursor+1)%ring_size,std::memory_order_release);
  }
  void Reject(Work* work) {
    const size_t index=work->pool_index;
    if(index==size_t(-1)){delete work;return;}
    Clear(work);rejected[rejected_count++]=index;
  }
  void Payload(Work* work,const void* data,size_t size) {
    if(size>work->payload.capacity())++metrics.payload_growths;
    work->payload.resize(size);
    if(size)std::memcpy(work->payload.data(),data,size);
  }
  SwanGpuPipeline::PacketMetrics Metrics() const {return metrics;}
  void ResetMetrics(){metrics={};}
};
PacketPool packet_pool;
class PacketLease {
  Work* pointer=nullptr;
public:
  PacketLease()=default;
  explicit PacketLease(Work* work):pointer(work) {}
  ~PacketLease(){if(pointer)packet_pool.Reject(pointer);}
  PacketLease(const PacketLease&)=delete;
  PacketLease& operator=(const PacketLease&)=delete;
  PacketLease(PacketLease&& other) noexcept:pointer(std::exchange(other.pointer,nullptr)) {}
  Work* Get() const {return pointer;}
  void Release(){pointer=nullptr;}
};
void Execute(void* pointer) noexcept {
  auto* work=static_cast<Work*>(pointer);
  Scope scope;
  try { work->run(); } catch (...) { failed.store(true); }
  packet_pool.Recycle(work);
}
template<class F> void Submit(F&& run,bool wait,size_t bytes=1,PacketLease prepared={}) {
  if(backend_scope) { run(); return; }
  PacketLease work=prepared.Get()?std::move(prepared):PacketLease(packet_pool.Acquire());
  work.Get()->run=SwanRender::Task<512>(std::forward<F>(run));
  if(!dispatch || !dispatch(Execute,work.Get(),wait,bytes))
    throw std::runtime_error("Hardware backend dispatcher is unavailable");
  work.Release();
}

// A protected pointer-to-member accessor is standard C++: no layout casts,
// copied GPU objects, private/public macros or changes to upstream headers.
struct Access : GPU_HW {
#define FIELD(name) static auto name##_member() { return &Access::name; }
  FIELD(m_GPUSTAT) FIELD(m_draw_mode) FIELD(m_drawing_area) FIELD(m_drawing_offset)
  FIELD(m_crtc_state) FIELD(m_console_is_pal) FIELD(m_force_progressive_scan) FIELD(m_force_ntsc_timings)
  FIELD(m_batch) FIELD(m_batch_ubo_data) FIELD(m_vram_dirty_rect) FIELD(m_current_depth) FIELD(m_last_depth_z)
  FIELD(m_resolution_scale) FIELD(m_multisamples) FIELD(m_max_resolution_scale) FIELD(m_max_multisamples)
  FIELD(m_render_api) FIELD(m_true_color) FIELD(bits) FIELD(m_texture_filtering) FIELD(m_downsample_mode)
  FIELD(m_using_uv_limits) FIELD(m_pgxp_depth_buffer) FIELD(m_shader_precompile_mode)
  FIELD(m_vram_ptr) FIELD(m_batch_start_vertex_ptr) FIELD(m_batch_base_vertex)
#undef FIELD
#define METHOD(name) static auto name##_method() { return &Access::name; }
  METHOD(ReadVRAM) METHOD(FillVRAM) METHOD(UpdateVRAM) METHOD(CopyVRAM)
  METHOD(ClearDisplay) METHOD(UpdateDisplay) METHOD(UpdateVRAMReadTexture)
  METHOD(UpdateDepthBufferFromMaskBit) METHOD(ClearDepthBuffer) METHOD(SetScissorFromDrawingArea)
  METHOD(MapBatchVertexPointer) METHOD(UnmapBatchVertexPointer) METHOD(UploadUniformBuffer) METHOD(DrawBatchVertices)
#undef METHOD
};

class QueuedHardware final : public GPU_HW {
  std::unique_ptr<GPU_HW> backend;
  std::vector<BatchVertex> staging;
  uint32_t pending_vertex_count=0;
  bool ready=false;
  // Upstream Rectangle has custom copy operations. Store its four values
  // explicitly so every snapshot field has a plain-data copy contract.
  struct Bounds {
    uint32_t left,top,right,bottom;
    static Bounds From(const Common::Rectangle<uint32_t>& r) {return {r.left,r.top,r.right,r.bottom};}
    Common::Rectangle<uint32_t> Rectangle() const {return {left,top,right,bottom};}
  };
  struct State {
    GPUSTAT stat;
    DrawMode draw;
    Bounds area;
    DrawingOffset offset;
    CRTCState crtc;
    bool pal,progressive,ntsc;
    BatchConfig batch;
    BatchUBOData uniforms;
    Bounds dirty;
    int32_t depth;
    float last_z;
    void Apply(GPU_HW& target) const {
#define SET(name,value) target.*Access::name##_member()=value
      SET(m_GPUSTAT,stat); SET(m_draw_mode,draw); SET(m_drawing_area,area.Rectangle()); SET(m_drawing_offset,offset);
      SET(m_crtc_state,crtc); SET(m_console_is_pal,pal); SET(m_force_progressive_scan,progressive); SET(m_force_ntsc_timings,ntsc);
      SET(m_batch,batch); SET(m_batch_ubo_data,uniforms); SET(m_vram_dirty_rect,dirty.Rectangle());
      SET(m_current_depth,depth); SET(m_last_depth_z,last_z);
#undef SET
    }
  };
  static_assert(std::is_trivially_copyable_v<State>, "Queued render state must not borrow CPU objects");
  static_assert(std::is_trivially_copyable_v<BatchVertex>, "Vertex packets must own plain data");
  State Capture() const {
    return {m_GPUSTAT,m_draw_mode,Bounds::From(m_drawing_area),m_drawing_offset,m_crtc_state,
      m_console_is_pal,m_force_progressive_scan,m_force_ntsc_timings,m_batch,m_batch_ubo_data,
      Bounds::From(m_vram_dirty_rect),m_current_depth,m_last_depth_z};
  }
  template<class F> void Send(F function,bool wait=false,size_t bytes=1,PacketLease packet={}) {
    const auto state=Capture();
    auto* target=backend.get();
    auto command=[state,target,function=std::move(function)]() mutable {
      state.Apply(*target); function(*target);
    };
    static_assert(sizeof(command)<=512 && std::is_nothrow_move_constructible_v<decltype(command)>,
                  "Common hardware commands must fit inline storage");
    Submit(std::move(command),wait,bytes+sizeof(State),std::move(packet));
  }
  void CopyConfiguration() {
#define COPY(name) name=backend.get()->*Access::name##_member()
    COPY(m_resolution_scale); COPY(m_multisamples); COPY(m_max_resolution_scale); COPY(m_max_multisamples);
    COPY(m_render_api); COPY(m_true_color); COPY(bits); COPY(m_texture_filtering); COPY(m_downsample_mode);
    COPY(m_using_uv_limits); COPY(m_pgxp_depth_buffer); COPY(m_shader_precompile_mode); COPY(m_batch_ubo_data);
#undef COPY
    m_batch_ubo_dirty=true;
  }
public:
  explicit QueuedHardware(std::unique_ptr<GPU_HW> value):backend(std::move(value)) {}
  ~QueuedHardware() override {
    if(ready) { FlushRender(); SwanGpuPipeline::Sync(); --live; }
    auto* target=backend.release();
    if(target) Submit([target]{delete target;},true);
  }
  bool Initialize(HostDisplay* display) override {
    if(!GPU::Initialize(display)) return false;
    bool ok=false;
    Send([&](GPU_HW& target){ok=target.Initialize(display);},true);
    if(!ok || failed.load()) return false;
    CopyConfiguration();
    UpdateSoftwareRenderer(false);
    ready=true; ++live;
    return true;
  }
  void Reset(bool clear) override {
    if(!ready) return;
    FlushRender(); SwanGpuPipeline::Sync();
    GPU_HW::Reset(clear);
    Send([=](GPU_HW& target){target.Reset(clear);},true);
    m_batch_ubo_dirty=true;
  }
  bool DoState(StateWrapper& sw,HostDisplayTexture**,bool update) override {
    // Disk states retain the exact upstream format. Internal GPU-texture states
    // use the safe RAM path here, avoiding cross-owner texture objects.
    FlushRender(); SwanGpuPipeline::Sync();
    const bool result=GPU_HW::DoState(sw,nullptr,update);
    SwanGpuPipeline::Sync(); return result;
  }
  void UpdateSettings() override {
    FlushRender(); SwanGpuPipeline::Sync();
    GPU::UpdateSettings();
    Send([](GPU_HW& target){target.UpdateSettings();},true);
    CopyConfiguration();
    UpdateSoftwareRenderer(true);
    m_draw_mode.SetTexturePageChanged(); m_draw_mode.texture_window_changed=true;
    m_drawing_area_changed=true;
  }
  void ResetGraphicsAPIState() override {
    Submit([this]{backend->ResetGraphicsAPIState();},!SwanGpuPipeline::QueuesFrames());
  }
  void RestoreGraphicsAPIState() override {
    Submit([this]{backend->RestoreGraphicsAPIState();},!SwanGpuPipeline::QueuesFrames());
    m_batch_ubo_dirty=true;
  }
protected:
  void ReadVRAM(uint32_t x,uint32_t y,uint32_t w,uint32_t h) override {
    FlushRender();
    if(m_sw_renderer) { ReadSoftwareRendererVRAM(x,y,w,h); return; }
    Send([=](GPU_HW& target){
      (target.*Access::ReadVRAM_method())(x,y,w,h);
      std::memcpy(m_vram_shadow.data(),target.*Access::m_vram_ptr_member(),sizeof(uint16_t)*VRAM_WIDTH*VRAM_HEIGHT);
    },true);
    m_batch_ubo_dirty=true;
  }
  void FillVRAM(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t color) override {
    if(m_sw_renderer) FillSoftwareRendererVRAM(x,y,w,h,color);
    Send([=](GPU_HW& target){(target.*Access::FillVRAM_method())(x,y,w,h,color);});
    GPU_HW::FillVRAM(x,y,w,h,color);
    IncludeVRAMDirtyRectangle(GetVRAMTransferBounds(x,y,w,h)); m_batch_ubo_dirty=true;
  }
  void UpdateVRAM(uint32_t x,uint32_t y,uint32_t w,uint32_t h,const void* data,bool set,bool check) override {
    if(!w || !h || w>VRAM_WIDTH || h>VRAM_HEIGHT) throw std::runtime_error("Invalid VRAM packet");
    const auto bytes=size_t(w)*h*sizeof(uint16_t);
    if(m_sw_renderer) UpdateSoftwareRendererVRAM(x,y,w,h,data,set,check);
    PacketLease packet(packet_pool.Acquire());auto* work=packet.Get();
    packet_pool.Payload(work,data,bytes);
    Send([=](GPU_HW& target){(target.*Access::UpdateVRAM_method())(x,y,w,h,work->payload.data(),set,check);},false,bytes,std::move(packet));
    const auto bounds=GetVRAMTransferBounds(x,y,w,h);
    GPU_HW::UpdateVRAM(bounds.left,bounds.top,bounds.GetWidth(),bounds.GetHeight(),data,set,check);
    m_batch_ubo_dirty=true;
  }
  void CopyVRAM(uint32_t sx,uint32_t sy,uint32_t dx,uint32_t dy,uint32_t w,uint32_t h) override {
    if(m_sw_renderer) CopySoftwareRendererVRAM(sx,sy,dx,dy,w,h);
    Send([=](GPU_HW& target){(target.*Access::CopyVRAM_method())(sx,sy,dx,dy,w,h);});
    GPU_HW::CopyVRAM(sx,sy,dx,dy,w,h);
    IncludeVRAMDirtyRectangle(GetVRAMTransferBounds(dx,dy,w,h)); m_batch_ubo_dirty=true;
  }
  void ClearDisplay() override {
    if(ready) Send([](GPU_HW& target){(target.*Access::ClearDisplay_method())();},!SwanGpuPipeline::QueuesFrames());
  }
  void UpdateDisplay() override {
    if(!ready) return;
    geometry={m_crtc_state.display_width,m_crtc_state.display_height,GetDisplayAspectRatio()};
    FlushRender(); Send([](GPU_HW& target){(target.*Access::UpdateDisplay_method())();},!SwanGpuPipeline::QueuesFrames());
    m_batch_ubo_dirty=true;
  }
  void UpdateVRAMReadTexture() override {
    Send([](GPU_HW& target){(target.*Access::UpdateVRAMReadTexture_method())();});
    GPU_HW::UpdateVRAMReadTexture(); m_batch_ubo_dirty=true;
  }
  void UpdateDepthBufferFromMaskBit() override {
    Send([](GPU_HW& target){(target.*Access::UpdateDepthBufferFromMaskBit_method())();});
    m_batch_ubo_dirty=true;
  }
  void ClearDepthBuffer() override {
    Send([](GPU_HW& target){(target.*Access::ClearDepthBuffer_method())();});
    m_last_depth_z=1.0f; m_batch_ubo_dirty=true;
  }
  void SetScissorFromDrawingArea() override {
    Send([](GPU_HW& target){(target.*Access::SetScissorFromDrawingArea_method())();});
  }
  void MapBatchVertexPointer(uint32_t required) override {
    constexpr size_t capacity=VERTEX_BUFFER_SIZE/sizeof(BatchVertex);
    if(required>capacity) throw std::runtime_error("Oversized vertex packet");
    if(staging.empty()) staging.resize(capacity);
    m_batch_start_vertex_ptr=staging.data(); m_batch_current_vertex_ptr=staging.data();
    m_batch_end_vertex_ptr=staging.data()+staging.size(); m_batch_base_vertex=0;
  }
  void UnmapBatchVertexPointer(uint32_t count) override {
    if(count>staging.size()) throw std::runtime_error("Invalid vertex packet");
    // FlushRender calls DrawBatchVertices immediately after unmapping. Carry
    // the owned vertices in that draw packet instead of a separate queue job.
    pending_vertex_count=count;
    m_batch_start_vertex_ptr=m_batch_current_vertex_ptr=m_batch_end_vertex_ptr=nullptr;
  }
  void UploadUniformBuffer(const void* data,uint32_t size) override {
    // Batch uniforms are part of the immutable draw packet below. The real
    // upload must follow backend vertex mapping: Vulkan can roll its command
    // buffer there, invalidating an earlier uniform-buffer binding.
    if(size!=sizeof(m_batch_ubo_data)) throw std::runtime_error("Invalid batch uniform packet");
    if(data!=&m_batch_ubo_data)std::memcpy(&m_batch_ubo_data,data,size);
  }
  void DrawBatchVertices(BatchRenderMode mode,uint32_t,uint32_t count) override {
    const auto bytes=size_t(std::exchange(pending_vertex_count,0u))*sizeof(BatchVertex);
    PacketLease packet(packet_pool.Acquire());auto* work=packet.Get();
    packet_pool.Payload(work,staging.data(),bytes);
    Send([mode,count,work](GPU_HW& target){
      if(!work->payload.empty()) {
        const auto vertices=uint32_t(work->payload.size()/sizeof(BatchVertex));
        (target.*Access::MapBatchVertexPointer_method())(vertices);
        auto* mapped=target.*Access::m_batch_start_vertex_ptr_member();
        std::memcpy(mapped,work->payload.data(),work->payload.size());
        (target.*Access::UnmapBatchVertexPointer_method())(vertices);
      }
      // A second transparency pass reuses the first pass's uploaded vertices
      // and driver base offset, but gets its own immutable uniform snapshot.
      const auto& uniforms=target.*Access::m_batch_ubo_data_member();
      (target.*Access::UploadUniformBuffer_method())(&uniforms,sizeof(uniforms));
      (target.*Access::DrawBatchVertices_method())(mode,target.*Access::m_batch_base_vertex_member(),count);
    },false,bytes,std::move(packet));
  }
};
}

void SwanGpuPipeline::Configure(Dispatch value) { Sync(); queue_frames=false;frame_queue.Reset(2);geometry={};dispatch=value;failed=false;packet_pool.ResetMetrics(); }
bool SwanGpuPipeline::OnBackend() { return backend_scope; }
bool SwanGpuPipeline::Active() { return live.load()!=0; }
bool SwanGpuPipeline::Failed() { return failed.load(); }
SwanGpuPipeline::PacketMetrics SwanGpuPipeline::ReadPacketMetrics(){return packet_pool.Metrics();}
void SwanGpuPipeline::Sync() { if(dispatch && !backend_scope) Submit([]{},true); }
void SwanGpuPipeline::ConfigureFrames(unsigned capacity) {
  Sync();frame_queue.Reset(capacity?capacity:2);queue_frames=capacity&&dispatch&&Active();
}
bool SwanGpuPipeline::QueuesFrames() {return queue_frames.load()&&Active();}
unsigned SwanGpuPipeline::QueuedFrames() {return frame_queue.Pending();}
unsigned SwanGpuPipeline::PeakFrames() {return frame_queue.Peak();}
SwanGpuPipeline::DisplayGeometry SwanGpuPipeline::Geometry() {return geometry;}
void SwanGpuPipeline::Present(std::function<void()> render) {
  if(dispatch&&QueuesFrames()&&!backend_scope) {
    try {
      auto ticket=frame_queue.Acquire();
      Submit([render=std::move(render),ticket=std::move(ticket)]{render();},false);
    } catch(...) {failed=true;} // Fail closed; never throw through the core's C frame ABI.
  } else if(dispatch)Submit(std::move(render),true);else render();
}
std::unique_ptr<GPU> SwanGpuPipeline::Wrap(std::unique_ptr<GPU_HW> backend) {
  if(!dispatch) return backend;
  return std::make_unique<QueuedHardware>(std::move(backend));
}
