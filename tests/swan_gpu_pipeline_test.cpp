// SPDX-License-Identifier: GPL-3.0-or-later
#include "../native/swanstation/gpu_pipeline.h"
#include "../native/swanstation/render_queue.h"
#include "../native/swanstation/frame_context.h"
#include "core/gpu_hw.h"
#include <iostream>
#include <stdexcept>

namespace {
SwanRender::Queue queue(64);
std::thread::id owner;
struct Sample {uint32_t color,stat,base; int mode,uniform,pass;};
std::vector<Sample> rendered;
int initialized=0,destroyed=0,resets=0,displays=0,vertex_uploads=0;
bool inject_failure=false;
bool reject_next=false;
std::vector<std::pair<void(*)(void*),void*>> delayed_packets;
void CheckOwner() {assert(std::this_thread::get_id()==owner);assert(SwanGpuPipeline::OnBackend());}
bool Dispatch(void(*run)(void*),void* value,bool wait,size_t bytes) {
  if(reject_next){reject_next=false;return false;}
  if(!queue.Active())return false;
  if(wait)queue.Call([=]{run(value);},bytes);else queue.Push([=]{run(value);},bytes);
  return true;
}
bool DelayedDispatch(void(*run)(void*),void* value,bool wait,size_t) {
  if(reject_next){reject_next=false;return false;}
  if(!wait){delayed_packets.emplace_back(run,value);return true;}
  queue.Call([=]{
    for(auto& packet:delayed_packets)packet.first(packet.second);
    delayed_packets.clear();run(value);
  });
  return true;
}
struct Driver : GPU_HW {
  std::vector<BatchVertex> mapped;
  int uniform=0;
  ~Driver() override {CheckOwner();++destroyed;}
  bool Initialize(HostDisplay*) override {CheckOwner();m_resolution_scale=3;++initialized;return true;}
  void Reset(bool clear) override {CheckOwner();GPU_HW::Reset(clear);if(clear)m_vram_shadow.fill(0);++resets;}
  void UpdateSettings() override {CheckOwner();m_resolution_scale=4;}
  void ResetGraphicsAPIState() override {CheckOwner();}
  void RestoreGraphicsAPIState() override {CheckOwner();}
  void ReadVRAM(uint32_t,uint32_t,uint32_t,uint32_t) override {CheckOwner();}
  void FillVRAM(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t value) override {
    CheckOwner();for(uint32_t row=y;row<y+h;++row)for(uint32_t col=x;col<x+w;++col)m_vram_shadow[row*VRAM_WIDTH+col]=uint16_t(value);
  }
  void UpdateVRAM(uint32_t x,uint32_t y,uint32_t w,uint32_t h,const void* input,bool,bool) override {
    CheckOwner();const auto* pixels=static_cast<const uint16_t*>(input);
    for(uint32_t row=0;row<h;++row)std::memcpy(&m_vram_shadow[(row+y)*VRAM_WIDTH+x],pixels+row*w,w*sizeof(uint16_t));
  }
  void CopyVRAM(uint32_t sx,uint32_t sy,uint32_t dx,uint32_t dy,uint32_t w,uint32_t h) override {
    CheckOwner();for(uint32_t row=0;row<h;++row)std::memmove(&m_vram_shadow[(row+dy)*VRAM_WIDTH+dx],&m_vram_shadow[(row+sy)*VRAM_WIDTH+sx],w*sizeof(uint16_t));
  }
  void ClearDisplay() override {CheckOwner();}
  void UpdateDisplay() override {CheckOwner();++displays;}
  void UpdateDepthBufferFromMaskBit() override {CheckOwner();}
  void ClearDepthBuffer() override {CheckOwner();}
  void SetScissorFromDrawingArea() override {CheckOwner();}
  void MapBatchVertexPointer(uint32_t count) override {
    CheckOwner();++vertex_uploads;mapped.resize(count+17);m_batch_base_vertex=17;uniform=-1;
    m_batch_start_vertex_ptr=mapped.data()+17;
  }
  void UnmapBatchVertexPointer(uint32_t) override {CheckOwner();}
  void UploadUniformBuffer(const void* data,uint32_t size) override {
    CheckOwner();assert(size==sizeof(BatchUBOData));uniform=static_cast<const BatchUBOData*>(data)->mode;
  }
  void DrawBatchVertices(BatchRenderMode pass,uint32_t base,uint32_t count) override {
    CheckOwner();assert(count==1);assert(base==17);
    if(inject_failure)throw std::runtime_error("simulated driver failure");
    rendered.push_back({mapped.at(base).color,m_GPUSTAT.bits,base,m_batch.mode,uniform,int(pass)});
  }
};
}
int main() {
  for(int session=0;session<5;++session) {
    assert(queue.Start([]{owner=std::this_thread::get_id();return true;}));
    SwanGpuPipeline::Configure(Dispatch);
    auto gpu=SwanGpuPipeline::Wrap(std::make_unique<Driver>());
    auto* front=static_cast<GPU_HW*>(gpu.get());
    assert(gpu->Initialize(nullptr));assert(SwanGpuPipeline::Active());
    assert(front->m_resolution_scale==3);
    gpu->Reset(true);

    // Hold the real worker. The producer must finish issuing both draws and
    // reuse its vertex/VRAM input memory before the backend can consume it.
    std::promise<void> entered,release;
    auto allowed=release.get_future().share();
    queue.Push([&]{entered.set_value();allowed.wait();});entered.get_future().wait();
    const auto before=rendered.size();
    const auto commands_before=queue.ReadMetrics().submitted;
    const auto uploads_before=vertex_uploads;
    front->Emit(71,9);front->Emit(98,11);front->Emit(99,11);
    assert(queue.ReadMetrics().submitted==commands_before+3);
    front->Emit(42,12,true);
    assert(queue.ReadMetrics().submitted==commands_before+5);
    // An empty batch neither uploads vertices nor queues a draw.
    front->MapBatchVertexPointer(0);front->FlushRender();
    assert(queue.ReadMetrics().submitted==commands_before+5);
    std::vector<uint16_t> pixels{1,2,3,4};
    gpu->UpdateVRAM(4,3,4,1,pixels.data(),false,false);
    pixels.assign(4,99);
    release.set_value();
    gpu->ReadVRAM(4,3,4,1); // A read is a completion barrier.
    assert(rendered.size()==before+5);
    assert(vertex_uploads==uploads_before+4);
    assert(rendered[before].color==71 && rendered[before].stat==71 && rendered[before].mode==9 && rendered[before].uniform==9);
    assert(rendered[before+1].color==98 && rendered[before+1].stat==98 && rendered[before+1].mode==11 && rendered[before+1].uniform==11);
    assert(rendered[before+2].color==99 && rendered[before+2].uniform==11);
    assert(rendered[before+3].color==42 && rendered[before+3].uniform==12);
    assert(rendered[before+4].color==42 && rendered[before+4].uniform==112);
    assert(rendered[before+3].pass==int(GPU_HW::BatchRenderMode::OnlyOpaque));
    assert(rendered[before+4].pass==int(GPU_HW::BatchRenderMode::OnlyTransparent));
    assert(front->m_vram_ptr[3*GPU::VRAM_WIDTH+7]==4);
    gpu->FillVRAM(0,0,2,1,21);
    gpu->CopyVRAM(0,0,0,1,2,1);
    StateWrapper state;
    HostDisplayTexture* texture=nullptr;
    assert(gpu->DoState(state,&texture,false));
    assert(state.vram[GPU::VRAM_WIDTH]==21);
    gpu->FillVRAM(0,1,2,1,39);
    state.reading=true;assert(gpu->DoState(state,&texture,true));
    gpu->ReadVRAM(0,1,2,1);
    assert(front->m_vram_ptr[GPU::VRAM_WIDTH]==21);
    gpu->UpdateSettings();assert(front->m_resolution_scale==4);
    gpu->ResetGraphicsAPIState();gpu->RestoreGraphicsAPIState();
    gpu->ClearDisplay();gpu->UpdateDisplay();
    SwanGpuPipeline::Present([]{CheckOwner();});
    gpu.reset(); // All accepted work drains before backend destruction.
    assert(!SwanGpuPipeline::Active());assert(!SwanGpuPipeline::Failed());
    SwanGpuPipeline::Configure(nullptr);queue.Stop();
  }
  assert(initialized==5 && destroyed==5 && resets>=10 && displays>=10);
  // Whole-frame overlap, including the real end-of-frame state/display calls.
  // The first presentation is deliberately blocked. The CPU must still finish
  // a second frame, then stop at the configured frame bound rather than either
  // draining every frame or building an unlimited video backlog.
  assert(queue.Start([]{owner=std::this_thread::get_id();return true;}));
  SwanGpuPipeline::Configure(Dispatch);
  {
    auto gpu=SwanGpuPipeline::Wrap(std::make_unique<Driver>());assert(gpu->Initialize(nullptr));
    SwanGpuPipeline::ConfigureFrames(2);assert(SwanGpuPipeline::QueuesFrames());
    std::promise<void> entered,release,two_queued;
    auto allowed=release.get_future().share();auto two_ready=two_queued.get_future();
    std::vector<int> presented;
    auto producer=std::async(std::launch::async,[&]{
      auto* front=static_cast<GPU_HW*>(gpu.get());
      for(int frame=1;frame<=3;++frame) {
        gpu->RestoreGraphicsAPIState();front->Emit(unsigned(frame),frame);
        gpu->ResetGraphicsAPIState();gpu->UpdateDisplay();
        const auto geometry=SwanGpuPipeline::Geometry();
        assert(geometry.width==320&&geometry.height==240&&geometry.aspect>1);
        SwanGpuPipeline::Present([&,frame]{CheckOwner();if(frame==1){entered.set_value();allowed.wait();}presented.push_back(frame);});
        if(frame==2)two_queued.set_value();
      }
      StateWrapper saved;assert(gpu->DoState(saved,nullptr,false)); // drains all queued frames before readback
      gpu->FillVRAM(0,0,1,1,77);
      saved.reading=true;assert(gpu->DoState(saved,nullptr,true));
      gpu->ReadVRAM(0,0,1,1);assert(front->m_vram_ptr[0]==saved.vram[0]);
    });
    entered.get_future().wait();
    assert(two_ready.wait_for(std::chrono::seconds(2))==std::future_status::ready);
    assert(producer.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
    assert(SwanGpuPipeline::QueuedFrames()==2&&SwanGpuPipeline::PeakFrames()==2);
    release.set_value();producer.get();SwanGpuPipeline::Sync();
    assert((presented==std::vector<int>{1,2,3}));assert(SwanGpuPipeline::QueuedFrames()==0);
    SwanGpuPipeline::Present([]{throw std::runtime_error("presentation failure");});
    SwanGpuPipeline::Sync();assert(SwanGpuPipeline::Failed()&&SwanGpuPipeline::QueuedFrames()==0);
    // Dispatch rejection releases its reservation without throwing through the
    // C frame ABI. The caller sees the failure flag and shuts down safely.
    reject_next=true;SwanGpuPipeline::Present([]{});
    assert(SwanGpuPipeline::Failed()&&SwanGpuPipeline::QueuedFrames()==0);
    SwanGpuPipeline::ConfigureFrames(0);
  }
  SwanGpuPipeline::Configure(nullptr);queue.Stop();

  // Destruction must drain a still-running presentation before freeing its GPU.
  assert(queue.Start([]{owner=std::this_thread::get_id();return true;}));SwanGpuPipeline::Configure(Dispatch);
  {
    auto gpu=SwanGpuPipeline::Wrap(std::make_unique<Driver>());assert(gpu->Initialize(nullptr));SwanGpuPipeline::ConfigureFrames(1);
    std::promise<void> entered,release;auto allowed=release.get_future().share();
    SwanGpuPipeline::Present([&]{entered.set_value();allowed.wait();});entered.get_future().wait();
    auto destroying=std::async(std::launch::async,[&]{gpu.reset();});
    assert(destroying.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
    release.set_value();destroying.get();assert(SwanGpuPipeline::QueuedFrames()==0&&!SwanGpuPipeline::QueuesFrames());
  }
  SwanGpuPipeline::Configure(nullptr);queue.Stop();

  // State reads/loads and reset wait for a frame that is still presenting.
  assert(queue.Start([]{owner=std::this_thread::get_id();return true;}));SwanGpuPipeline::Configure(Dispatch);
  {
    auto gpu=SwanGpuPipeline::Wrap(std::make_unique<Driver>());assert(gpu->Initialize(nullptr));SwanGpuPipeline::ConfigureFrames(2);
    StateWrapper state;assert(gpu->DoState(state,nullptr,false));
    for(int operation=0;operation<3;++operation) {
      std::promise<void> entered,release;auto allowed=release.get_future().share();
      SwanGpuPipeline::Present([&]{entered.set_value();allowed.wait();});entered.get_future().wait();
      auto changing=std::async(std::launch::async,[&]{
        if(operation==2){gpu->Reset(true);return true;}
        state.reading=operation==1;return gpu->DoState(state,nullptr,true);
      });
      assert(changing.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
      release.set_value();assert(changing.get());assert(SwanGpuPipeline::QueuedFrames()==0);
    }
  }
  SwanGpuPipeline::Configure(nullptr);queue.Stop();

  SwanRender::FrameContext scope_target;scope_target.present=10;
  auto copy=scope_target;copy.valid=true;copy.present=20;
  {SwanRender::FrameScope scope(scope_target,copy);copy.present=30;assert(scope_target.present==20);}
  assert(!scope_target.valid&&scope_target.present==10); // owned value and scope restoration
  assert(queue.Start([]{owner=std::this_thread::get_id();return true;}));
  SwanGpuPipeline::Configure(Dispatch);
  auto gpu=SwanGpuPipeline::Wrap(std::make_unique<Driver>());assert(gpu->Initialize(nullptr));
  inject_failure=true;static_cast<GPU_HW*>(gpu.get())->Emit(1,1);SwanGpuPipeline::Sync();
  assert(SwanGpuPipeline::Failed());
  gpu.reset();SwanGpuPipeline::Configure(nullptr);queue.Stop();
  assert(!SwanGpuPipeline::Failed());
  const auto cpu=std::this_thread::get_id();
  SwanGpuPipeline::Present([&]{assert(std::this_thread::get_id()==cpu);});

  // An artificial dispatcher can retain more packets than the normal bounded
  // host queue. Pool exhaustion must fall back safely, then recover all slots.
  assert(queue.Start([]{owner=std::this_thread::get_id();return true;}));
  SwanGpuPipeline::Configure(DelayedDispatch);
  gpu=SwanGpuPipeline::Wrap(std::make_unique<Driver>());assert(gpu->Initialize(nullptr));
  for(int i=0;i<700;++i)gpu->FillVRAM(0,0,1,1,uint32_t(i));
  assert(SwanGpuPipeline::ReadPacketMetrics().heap_fallbacks>0);
  SwanGpuPipeline::Sync();assert(delayed_packets.empty());
  gpu->ReadVRAM(0,0,1,1);assert(static_cast<GPU_HW*>(gpu.get())->m_vram_ptr[0]==699);
  gpu.reset();SwanGpuPipeline::Configure(Dispatch);
  gpu=SwanGpuPipeline::Wrap(std::make_unique<Driver>());assert(gpu->Initialize(nullptr));
  // Rejected payload packets never reach the backend, and their leased slots
  // are available on the next CPU submission without a cross-thread free race.
  const uint16_t pixel=33;
  for(int i=0;i<30;++i) {
    reject_next=true;
    try{gpu->UpdateVRAM(0,0,1,1,&pixel,false,false);assert(false);}
    catch(const std::runtime_error&){}
  }
  for(int i=0;i<5000;++i)static_cast<GPU_HW*>(gpu.get())->Emit(15,2);
  SwanGpuPipeline::Sync();
  auto packet_metrics=SwanGpuPipeline::ReadPacketMetrics();
  assert(packet_metrics.pooled>=5000 && packet_metrics.heap_fallbacks==0);
  assert(packet_metrics.payload_growths<=512); // Reuse, not an allocation per draw.
  // Oversized payload storage is retired safely; a later tiny packet must not
  // borrow it, retain it indefinitely, or corrupt the VRAM round trip.
  std::vector<uint16_t> large_pixels(GPU::VRAM_WIDTH*GPU::VRAM_HEIGHT,19);
  gpu->UpdateVRAM(0,0,GPU::VRAM_WIDTH,GPU::VRAM_HEIGHT,large_pixels.data(),false,false);
  large_pixels.assign(large_pixels.size(),0);
  gpu->ReadVRAM(0,0,1,1);assert(static_cast<GPU_HW*>(gpu.get())->m_vram_ptr[0]==19);
  gpu.reset();SwanGpuPipeline::Configure(nullptr);queue.Stop();
  std::cout<<"Backend ownership, bounded whole-frame overlap, read/state barriers, failure and repeated lifecycle passed\n";
}
