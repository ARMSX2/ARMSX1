#include "vulkan_gpu.h"
#include "gpu_primitives.h"
#include "vulkan_gpu_shaders.h"
#include <atomic>
#include <cstring>
#include <stdexcept>

namespace {
void checkGpu(VkResult result,const char* what){if(result!=VK_SUCCESS)throw std::runtime_error(std::string(what)+" (Vulkan "+std::to_string(result)+")");}
struct Buffer {VkBuffer handle=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;void* mapped=nullptr;VkDeviceSize size=0;};
struct Image {VkImage handle=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;VkImageView view=VK_NULL_HANDLE;};
struct Parameters {
    int bounds[4]{},data[4]{},shape[4]{},display[4]{};
    int count=0,countPixels=0,serialMode=0,snapshot=0;
    int operation=0,status=0,replacementTextures=0,smoothImage=0;
};
static_assert(sizeof(Parameters)==96);
}

struct NativeVulkanGpu::Impl:GpuPrimitiveBuilder {
    static constexpr VkDeviceSize VRAM_BYTES=1024*512*4;
    static constexpr unsigned SLOTS=3;
    static constexpr unsigned HD_BATCH=2048;
    PsxVulkanContext context;VkDevice device;const int scale;
    const VkDeviceSize commandRange,streamCapacity;
    VkCommandPool pool=VK_NULL_HANDLE;VkDescriptorSetLayout setLayout=VK_NULL_HANDLE;VkDescriptorPool descriptors=VK_NULL_HANDLE;
    VkPipelineLayout layout=VK_NULL_HANDLE;VkSampler sampler=VK_NULL_HANDLE;
    VkPipeline nativeDraw=VK_NULL_HANDLE,nativeTransfer=VK_NULL_HANDLE,nativeScan=VK_NULL_HANDLE;
    VkPipeline hdDraw=VK_NULL_HANDLE,hdTransfer=VK_NULL_HANDLE,hdScan=VK_NULL_HANDLE,opaqueDraw=VK_NULL_HANDLE,orderedDraw=VK_NULL_HANDLE,nativeResolve=VK_NULL_HANDLE;
    VkRenderPass renderPass=VK_NULL_HANDLE;VkFramebuffer framebuffer=VK_NULL_HANDLE;
    Buffer ram,snapshot,counter,readback,uploadBuffer;
    Image hd,hdRead,output;
    int outputWidth=1,outputHeight=1;uint64_t contentRevision=1;
    RectList displayReads;bool displayKnown=false;
    struct Slot {Buffer stream;VkCommandBuffer command=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;VkDescriptorSet set=VK_NULL_HANDLE;VkDeviceSize used=0;bool recording=false;};
    std::array<Slot,SLOTS> slots{};unsigned currentSlot=0;VkDeviceSize alignment=256;
    std::array<Primitive,32> pending{};unsigned pendingCount=0;bool feedbackBatch=false;
    std::vector<Rect> reads,writes;
    std::array<uint64_t,32> dirtyTiles{};
    std::vector<Primitive> hdPending;std::array<uint64_t,32> nativeDirty{};
    std::atomic<int64_t> draws{0},feedbackDraws{0},readbacks{0},batches{0},copies{0},uploadBytes{0},scanouts{0},nativeDraws{0},hdDraws{0};
    std::string name;

    uint32_t memoryType(uint32_t bits,VkMemoryPropertyFlags flags){
        VkPhysicalDeviceMemoryProperties memory{};vkGetPhysicalDeviceMemoryProperties(context.physical,&memory);
        for(uint32_t i=0;i<memory.memoryTypeCount;i++)if((bits&(1u<<i))&&(memory.memoryTypes[i].propertyFlags&flags)==flags)return i;
        throw std::runtime_error("Native Vulkan memory type unavailable");
    }
    void destroy(Buffer& buffer){if(buffer.mapped)vkUnmapMemory(device,buffer.memory);if(buffer.handle)vkDestroyBuffer(device,buffer.handle,nullptr);if(buffer.memory)vkFreeMemory(device,buffer.memory,nullptr);buffer={};}
    void destroy(Image& image){if(image.view)vkDestroyImageView(device,image.view,nullptr);if(image.handle)vkDestroyImage(device,image.handle,nullptr);if(image.memory)vkFreeMemory(device,image.memory,nullptr);image={};}
    void buffer(Buffer& out,VkDeviceSize size,VkBufferUsageFlags usage,bool host=false){
        out.size=size;VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};create.size=size;create.usage=usage;create.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        checkGpu(vkCreateBuffer(device,&create,nullptr,&out.handle),"Create native Vulkan buffer");VkMemoryRequirements required{};vkGetBufferMemoryRequirements(device,out.handle,&required);
        VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};memory.allocationSize=required.size;memory.memoryTypeIndex=memoryType(required.memoryTypeBits,host?VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT:0);
        checkGpu(vkAllocateMemory(device,&memory,nullptr,&out.memory),"Allocate native Vulkan buffer");checkGpu(vkBindBufferMemory(device,out.handle,out.memory,0),"Bind native Vulkan buffer");
        if(host)checkGpu(vkMapMemory(device,out.memory,0,size,0,&out.mapped),"Map native Vulkan buffer");
    }
    void image(Image& out,VkFormat format,VkImageUsageFlags usage,int width=0,int height=0){
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};create.imageType=VK_IMAGE_TYPE_2D;create.format=format;create.extent={uint32_t(width?width:1024*scale),uint32_t(height?height:512*scale),1};create.mipLevels=create.arrayLayers=1;create.samples=VK_SAMPLE_COUNT_1_BIT;create.tiling=VK_IMAGE_TILING_OPTIMAL;create.usage=usage;create.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        checkGpu(vkCreateImage(device,&create,nullptr,&out.handle),"Create native Vulkan image");VkMemoryRequirements required{};vkGetImageMemoryRequirements(device,out.handle,&required);
        VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};memory.allocationSize=required.size;memory.memoryTypeIndex=memoryType(required.memoryTypeBits,0);
        checkGpu(vkAllocateMemory(device,&memory,nullptr,&out.memory),"Allocate native Vulkan image");checkGpu(vkBindImageMemory(device,out.handle,out.memory,0),"Bind native Vulkan image");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=out.handle;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=format;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};checkGpu(vkCreateImageView(device,&view,nullptr,&out.view),"Create native Vulkan image view");
    }
    VkShaderModule module(const uint32_t* code,size_t bytes){VkShaderModuleCreateInfo create{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};create.codeSize=bytes;create.pCode=code;VkShaderModule shader=VK_NULL_HANDLE;checkGpu(vkCreateShaderModule(device,&create,nullptr,&shader),"Create native Vulkan shader");return shader;}
    VkSpecializationInfo specialization(VkSpecializationMapEntry& entry){entry={0,0,sizeof(scale)};return{1,&entry,sizeof(scale),&scale};}
    template<size_t N> void compute(VkPipeline& result,const uint32_t(&code)[N]){
        VkShaderModule shader=module(code,sizeof(code));VkSpecializationMapEntry entry;auto constants=specialization(entry);
        VkComputePipelineCreateInfo create{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};create.layout=layout;create.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};create.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;create.stage.module=shader;create.stage.pName="main";create.stage.pSpecializationInfo=&constants;
        VkResult status=vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&create,nullptr,&result);vkDestroyShaderModule(device,shader,nullptr);checkGpu(status,"Create native Vulkan compute pipeline");
    }
    VkCommandBuffer begin(){
        Slot& slot=slots[currentSlot];if(slot.recording)return slot.command;
        checkGpu(vkWaitForFences(device,1,&slot.fence,VK_TRUE,UINT64_MAX),"Wait native Vulkan stream slot");
        checkGpu(vkResetCommandBuffer(slot.command,0),"Reset native Vulkan command buffer");slot.used=0;
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};info.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checkGpu(vkBeginCommandBuffer(slot.command,&info),"Begin native Vulkan commands");slot.recording=true;return slot.command;
    }
    void submit(){Slot& slot=slots[currentSlot];if(!slot.recording)return;checkGpu(vkEndCommandBuffer(slot.command),"End native Vulkan commands");checkGpu(vkResetFences(device,1,&slot.fence),"Reset native Vulkan fence");VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};info.commandBufferCount=1;info.pCommandBuffers=&slot.command;if(context.lockQueue)context.lockQueue();VkResult status=vkQueueSubmit(context.queue,1,&info,slot.fence);if(context.unlockQueue)context.unlockQueue();if(status!=VK_SUCCESS){vkDestroyFence(device,slot.fence,nullptr);slot.fence=VK_NULL_HANDLE;checkGpu(status,"Submit native Vulkan commands");}slot.recording=false;currentSlot=(currentSlot+1)%SLOTS;}
    void wait(){submit();for(auto& slot:slots)if(slot.fence)checkGpu(vkWaitForFences(device,1,&slot.fence,VK_TRUE,UINT64_MAX),"Wait native Vulkan work");}
    void barrier(){
        VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};memory.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;memory.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(begin(),VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&memory,0,nullptr,0,nullptr);
    }
    void hostReadBarrier(){VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};memory.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;memory.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(begin(),VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&memory,0,nullptr,0,nullptr);}
    void resizeOutput(int width,int height){
        if(width==outputWidth&&height==outputHeight)return;wait();if(context.retireImage)context.retireImage(context.owner,output.handle);
        destroy(output);image(output,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,width,height);outputWidth=width;outputHeight=height;
        VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE,output.view,VK_IMAGE_LAYOUT_GENERAL};for(auto& slot:slots){VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=slot.set;write.dstBinding=6;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pImageInfo=&imageInfo;vkUpdateDescriptorSets(device,1,&write,0,nullptr);}
        VkImageMemoryBarrier imageBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};imageBarrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;imageBarrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;imageBarrier.srcQueueFamilyIndex=imageBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;imageBarrier.image=output.handle;imageBarrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};imageBarrier.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;vkCmdPipelineBarrier(begin(),VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&imageBarrier);
    }
    uint32_t commands(const void* data,size_t bytes){
        begin();Slot* slot=&slots[currentSlot];VkDeviceSize at=(slot->used+alignment-1)/alignment*alignment;
        if(at+commandRange>streamCapacity){submit();begin();slot=&slots[currentSlot];at=0;}
        if(bytes>commandRange)throw std::runtime_error("Native Vulkan command packet exceeds stream slice");
        if(bytes)std::memcpy(static_cast<char*>(slot->stream.mapped)+at,data,bytes);slot->used=at+std::max<size_t>(bytes,1);return uint32_t(at);
    }
    void dispatch(VkPipeline pipeline,const Parameters& params,unsigned x,unsigned y,uint32_t offset=0){
        VkCommandBuffer command=begin();vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&slots[currentSlot].set,1,&offset);vkCmdPushConstants(command,layout,VK_SHADER_STAGE_ALL,0,sizeof(params),&params);vkCmdDispatch(command,x,y,1);
    }
    static uint64_t tileMask(const Rect& bounds){return (~uint64_t(0)<<(bounds.x0>>4))&(~uint64_t(0)>>(63-(bounds.x1>>4)));}
    void touch(const Rect& bounds){if(!bounds.valid())return;if(!displayKnown){contentRevision++;return;}for(const Rect& read:displayReads)if(read.intersects(bounds)){contentRevision++;return;}}
    void touchWrapped(int x,int y,int width,int height){RectList regions;split(regions,x&1023,y&511,std::min(width,1024),std::min(height,512));for(const Rect& region:regions)touch(region);}
    void watchDisplay(const int* state,int width,int height){displayReads.count=0;displayKnown=true;if(state[0]&0x800000)return;int words=state[0]&0x200000?std::min(1024,(width*3+1)/2):width;split(displayReads,state[1]&1023,state[2]&511,words,height);}
    void mark(const Rect& bounds){if(!bounds.valid())return;uint64_t mask=tileMask(bounds);for(int row=bounds.y0>>4;row<=bounds.y1>>4;row++)dirtyTiles[row]|=mask;}
    void markWrapped(int x,int y,int w,int h){RectList regions;split(regions,x&1023,y&511,std::min(w,1024),std::min(h,512));for(const Rect& region:regions)mark(region);}
    void refreshTextures(){
        std::array<uint64_t,32> wanted{};for(const Rect& read:reads){uint64_t mask=tileMask(read);for(int row=read.y0>>4;row<=read.y1>>4;row++)wanted[row]|=mask;}
        std::vector<VkImageCopy> regions;
        for(unsigned row=0;row<32;row++){
            uint64_t mask=wanted[row]&dirtyTiles[row];dirtyTiles[row]&=~mask;
            while(mask){unsigned x=__builtin_ctzll(mask),end=x;while(end<64&&(mask&(uint64_t(1)<<end)))end++;
                uint64_t span=(~uint64_t(0)<<x)&(~uint64_t(0)>>(64-end));mask&=~span;
                VkImageCopy region{};region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.srcOffset=region.dstOffset={int(x*16*scale),int(row*16*scale),0};region.extent={uint32_t((end-x)*16*scale),uint32_t(16*scale),1};regions.push_back(region);
            }
        }
        if(regions.empty())return;barrier();vkCmdCopyImage(begin(),hd.handle,VK_IMAGE_LAYOUT_GENERAL,hdRead.handle,VK_IMAGE_LAYOUT_GENERAL,uint32_t(regions.size()),regions.data());barrier();copies+=regions.size();
    }
    void raster(uint32_t offset,const Rect& bounds,unsigned instances,bool ordered=false){
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};pass.renderPass=renderPass;pass.framebuffer=framebuffer;pass.renderArea={{bounds.x0*scale,bounds.y0*scale},{uint32_t((bounds.x1-bounds.x0+1)*scale),uint32_t((bounds.y1-bounds.y0+1)*scale)}};
        VkCommandBuffer command=begin();vkCmdBeginRenderPass(command,&pass,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,ordered?orderedDraw:opaqueDraw);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&slots[currentSlot].set,1,&offset);vkCmdSetScissor(command,0,1,&pass.renderArea);vkCmdDraw(command,6,instances,0,0);vkCmdEndRenderPass(command);
    }
    void flushHd(){
        if(hdPending.empty())return;uint32_t offset=commands(hdPending.data(),hdPending.size()*sizeof(Primitive));Rect bounds{1023,511,0,0};bool opaque=true;
        for(const auto& p:hdPending){Rect b=p.bounds();bounds={std::min(bounds.x0,b.x0),std::min(bounds.y0,b.y0),std::max(bounds.x1,b.x1),std::max(bounds.y1,b.y1)};opaque&=!(p.q[1]&2)&&!(p.q[14]&2);}
        barrier();raster(offset,bounds,unsigned(hdPending.size()),!opaque||!opaqueDraw);barrier();hdDraws++;hdPending.clear();
    }
    void resolveNative(){
        flushHd();std::array<uint32_t,64> rows{};Rect bounds{1023,511,0,0};bool dirty=false;
        for(unsigned row=0;row<32;row++){uint64_t mask=nativeDirty[row];if(!mask)continue;dirty=true;rows[row*2]=uint32_t(mask);rows[row*2+1]=uint32_t(mask>>32);bounds.x0=std::min(bounds.x0,int(__builtin_ctzll(mask)*16));bounds.x1=std::max(bounds.x1,int((63-__builtin_clzll(mask))*16+15));bounds.y0=std::min(bounds.y0,int(row*16));bounds.y1=std::max(bounds.y1,int(row*16+15));}
        if(!dirty)return;uint32_t offset=commands(rows.data(),sizeof(rows));Parameters params;params.bounds[0]=bounds.x0;params.bounds[1]=bounds.y0;params.bounds[2]=bounds.x1;params.bounds[3]=bounds.y1;barrier();dispatch(nativeResolve,params,(bounds.x1-bounds.x0+8)/8,(bounds.y1-bounds.y0+8)/8,offset);barrier();nativeDirty.fill(0);copies++;
    }
    void flush(){
        if(!pendingCount)return;
        if(orderedDraw&&!feedbackBatch){
            bool stale=false;for(const Rect& read:reads){uint64_t mask=tileMask(read);for(int row=read.y0>>4;row<=read.y1>>4;row++)stale|=(dirtyTiles[row]&mask)!=0;}
            if(stale){flushHd();refreshTextures();}else if(hdPending.size()+pendingCount>HD_BATCH)flushHd();
            hdPending.insert(hdPending.end(),pending.begin(),pending.begin()+pendingCount);
            for(const Rect& write:writes){mark(write);uint64_t mask=tileMask(write);for(int row=write.y0>>4;row<=write.y1>>4;row++)nativeDirty[row]|=mask;}
            batches++;pendingCount=0;reads.clear();writes.clear();return;
        }
        if(orderedDraw)resolveNative();uint32_t offset=commands(pending.data(),pendingCount*sizeof(Primitive));barrier();
        Rect bounds{1023,511,0,0};bool opaque=true;for(unsigned i=0;i<pendingCount;i++){Rect b=pending[i].bounds();bounds={std::min(bounds.x0,b.x0),std::min(bounds.y0,b.y0),std::max(bounds.x1,b.x1),std::max(bounds.y1,b.y1)};opaque&=!(pending[i].q[1]&2)&&!(pending[i].q[14]&2);}
        Parameters params;params.count=int(pendingCount);params.snapshot=feedbackBatch;params.bounds[0]=bounds.x0;params.bounds[1]=bounds.y0;params.bounds[2]=bounds.x1;params.bounds[3]=bounds.y1;
        if(feedbackBatch){VkBufferCopy copy{0,0,VRAM_BYTES};vkCmdCopyBuffer(begin(),ram.handle,snapshot.handle,1,&copy);barrier();feedbackDraws+=pendingCount;}
        dispatch(nativeDraw,params,(bounds.x1-bounds.x0+8)/8,(bounds.y1-bounds.y0+8)/8,offset);nativeDraws++;barrier();
        if(scale>1){
            if(feedbackBatch){
                // Refresh exact write rectangles. Their union would overwrite untouched HD pixels.
                params.operation=4;for(const Rect& write:writes){params.shape[0]=write.x0;params.shape[1]=write.y0;params.shape[2]=write.x1-write.x0+1;params.shape[3]=write.y1-write.y0+1;dispatch(hdTransfer,params,(params.shape[2]*scale+7)/8,(params.shape[3]*scale+7)/8,offset);barrier();}
            }else{
                refreshTextures();
                if(opaque&&opaqueDraw)raster(offset,bounds,pendingCount);
                else{params.bounds[0]*=scale;params.bounds[1]*=scale;params.bounds[2]=(params.bounds[2]+1)*scale-1;params.bounds[3]=(params.bounds[3]+1)*scale-1;params.replacementTextures=1;dispatch(hdDraw,params,((bounds.x1-bounds.x0+1)*scale+7)/8,((bounds.y1-bounds.y0+1)*scale+7)/8,offset);}
                hdDraws++;barrier();
            }
            for(const Rect& write:writes)mark(write);
        }
        batches++;pendingCount=0;reads.clear();writes.clear();feedbackBatch=false;
    }
    void addPrimitive(const Primitive& primitive){
        if(!primitive.bounds().valid())return;touch(primitive.bounds());auto range=textureReads(primitive,scale>1);bool ownFeedback=feedback(primitive,range),hazard=false;
        for(const Rect& read:range)for(const Rect& write:writes)hazard|=read.intersects(write);
        for(const Rect& read:reads)hazard|=read.intersects(primitive.bounds());
        if(pendingCount==32||hazard||(pendingCount&&ownFeedback!=feedbackBatch))flush();
        feedbackBatch=ownFeedback;pending[pendingCount++]=primitive;reads.insert(reads.end(),range.begin(),range.end());writes.push_back(primitive.bounds());
    }
    void draw(const int* packet,const int* environment){
        int op=uint32_t(packet[0])>>24;draws++;
        if(op<0x40){int at=1,color=packet[0],clut=0;bool shade=op&16,tex=op&4;Vertex vertices[4];int count=(op&8)?4:3;for(int i=0;i<count;i++){if(i&&shade)color=packet[at++];int xy=packet[at++],uv=tex?packet[at++]:0;if(tex&&i==0)clut=uint32_t(uv)>>16;vertices[i]=Vertex(xy,color,uv,environment[5]);}addPrimitive(triangle(vertices[0],vertices[1],vertices[2],op,clut,environment));if(count==4)addPrimitive(triangle(vertices[1],vertices[2],vertices[3],op,clut,environment));}
        else if(op<0x60)addPrimitive(line(packet,op,environment));else addPrimitive(rectangle(packet,op,environment));
    }
    void transfer(int operation,int x,int y,int width,int height,int a,int b,int c,int d,uint32_t offset=0){
        touchWrapped(x,y,width,operation==1?(c+d+width-1)/width:height);
        if(orderedDraw){if(operation>=2||(operation==1&&(b&2)))resolveNative();else flushHd();}
        Parameters params;params.operation=operation;params.shape[0]=x;params.shape[1]=y;params.shape[2]=width;params.shape[3]=height;params.data[0]=a;params.data[1]=b;params.data[2]=c;params.data[3]=d;
        barrier();dispatch(nativeTransfer,params,operation==3?(std::max(width,height)+7)/8:(width+7)/8,operation==3?1:(height+7)/8,offset);barrier();
        if(scale>1){dispatch(hdTransfer,params,operation==3?(std::max(width,height)*scale+7)/8:(width*scale+7)/8,operation==3?1:(height*scale+7)/8,offset);barrier();markWrapped(x,y,width,operation==1?(c+d+width-1)/width:height);}
    }
    void seedHd(){if(scale==1)return;Parameters params;params.operation=4;params.shape[2]=1024;params.shape[3]=512;dispatch(hdTransfer,params,(1024*scale+7)/8,(512*scale+7)/8);barrier();dirtyTiles.fill(~uint64_t(0));nativeDirty.fill(0);}
    void ensureReadback(VkDeviceSize size){if(size<=readback.size)return;wait();destroy(readback);buffer(readback,size,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);}
    void read(int* pixels){flush();resolveNative();ensureReadback(VRAM_BYTES);barrier();VkBufferCopy copy{0,0,VRAM_BYTES};vkCmdCopyBuffer(begin(),ram.handle,readback.handle,1,&copy);hostReadBarrier();wait();std::memcpy(pixels,readback.mapped,VRAM_BYTES);readbacks++;}
    void set(const int* pixels,bool seed=true){flush();flushHd();wait();contentRevision++;std::memcpy(uploadBuffer.mapped,pixels,VRAM_BYTES);VkBufferCopy copy{0,0,VRAM_BYTES};vkCmdCopyBuffer(begin(),uploadBuffer.handle,ram.handle,1,&copy);barrier();if(seed)seedHd();uploadBytes+=VRAM_BYTES;}

    void createOpaque(bool ordered=false){
        if(!renderPass){
        VkAttachmentDescription attachment{};attachment.format=VK_FORMAT_R32_UINT;attachment.samples=VK_SAMPLE_COUNT_1_BIT;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference color{0,VK_IMAGE_LAYOUT_GENERAL};VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.colorAttachmentCount=1;subpass.pColorAttachments=&color;if(context.orderedColor){subpass.flags=VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT;subpass.inputAttachmentCount=1;subpass.pInputAttachments=&color;}
        VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};pass.attachmentCount=1;pass.pAttachments=&attachment;pass.subpassCount=1;pass.pSubpasses=&subpass;checkGpu(vkCreateRenderPass(device,&pass,nullptr,&renderPass),"Create native Vulkan HD pass");
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=renderPass;fb.attachmentCount=1;fb.pAttachments=&hd.view;fb.width=1024*scale;fb.height=512*scale;fb.layers=1;checkGpu(vkCreateFramebuffer(device,&fb,nullptr,&framebuffer),"Create native Vulkan HD framebuffer");
        }
        VkShaderModule vertex=module(PsxVulkanShaders::OpaqueVertex,sizeof(PsxVulkanShaders::OpaqueVertex)),fragment=VK_NULL_HANDLE;
        try{fragment=ordered?module(PsxVulkanShaders::OrderedFragment,sizeof(PsxVulkanShaders::OrderedFragment)):module(PsxVulkanShaders::OpaqueFragment,sizeof(PsxVulkanShaders::OpaqueFragment));VkSpecializationMapEntry entry;auto constants=specialization(entry);
            VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};for(auto& stage:stages){stage.pName="main";stage.pSpecializationInfo=&constants;}stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vertex;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fragment;
            VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkViewport viewport{0,0,float(1024*scale),float(512*scale),0,1};VkPipelineViewportStateCreateInfo view{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};view.viewportCount=1;view.pViewports=&viewport;view.scissorCount=1;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.lineWidth=1;
            VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};multisample.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState colorBlend{};colorBlend.colorWriteMask=VK_COLOR_COMPONENT_R_BIT;VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&colorBlend;
            if(ordered)blend.flags=VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT;
            VkDynamicState scissor=VK_DYNAMIC_STATE_SCISSOR;VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=1;dynamic.pDynamicStates=&scissor;
            VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};create.stageCount=2;create.pStages=stages;create.pVertexInputState=&input;create.pInputAssemblyState=&assembly;create.pViewportState=&view;create.pRasterizationState=&raster;create.pMultisampleState=&multisample;create.pColorBlendState=&blend;create.pDynamicState=&dynamic;create.layout=layout;create.renderPass=renderPass;
            checkGpu(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&create,nullptr,ordered?&orderedDraw:&opaqueDraw),"Create native Vulkan graphics pipeline");
        }catch(...){vkDestroyShaderModule(device,vertex,nullptr);if(fragment)vkDestroyShaderModule(device,fragment,nullptr);throw;}
        vkDestroyShaderModule(device,vertex,nullptr);vkDestroyShaderModule(device,fragment,nullptr);
    }
    Impl(PsxVulkanContext supplied,int requestedScale):context(supplied),device(supplied.device),scale(requestedScale),commandRange((requestedScale>1&&supplied.orderedColor?HD_BATCH:32)*sizeof(Primitive)),streamCapacity(requestedScale>1&&supplied.orderedColor?2*commandRange:512*1024){try{
        if(scale<1||scale>5||!device)throw std::runtime_error("Invalid native Vulkan renderer configuration");
        VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(context.physical,&properties);name=properties.deviceName;alignment=std::max<VkDeviceSize>(256,properties.limits.minStorageBufferOffsetAlignment);
        uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(context.physical,&families,nullptr);std::vector<VkQueueFamilyProperties> queues(families);vkGetPhysicalDeviceQueueFamilyProperties(context.physical,&families,queues.data());if(context.family>=families||(queues[context.family].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))!=(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))throw std::runtime_error("Native Vulkan needs a graphics/compute queue");
        if(properties.limits.maxImageDimension2D<uint32_t(1024*scale)||properties.limits.maxStorageBufferRange<VRAM_BYTES||properties.limits.maxPushConstantsSize<sizeof(Parameters)||properties.limits.maxPerStageDescriptorStorageBuffers<4||properties.limits.maxPerStageDescriptorStorageImages<3)throw std::runtime_error("GPU limits do not support native Vulkan rendering at this scale");
        VkFormatProperties rgba{},integer{};vkGetPhysicalDeviceFormatProperties(context.physical,VK_FORMAT_R8G8B8A8_UNORM,&rgba);vkGetPhysicalDeviceFormatProperties(context.physical,VK_FORMAT_R32_UINT,&integer);
        if(!(rgba.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)||!(rgba.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT)||!(integer.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)||!(integer.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))throw std::runtime_error("GPU formats do not support native Vulkan rendering");
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};poolInfo.queueFamilyIndex=context.family;poolInfo.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;checkGpu(vkCreateCommandPool(device,&poolInfo,nullptr,&pool),"Create native Vulkan command pool");
        constexpr auto storage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer(ram,VRAM_BYTES,storage);buffer(snapshot,VRAM_BYTES,storage);buffer(counter,4,storage);buffer(uploadBuffer,VRAM_BYTES,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,true);
        auto usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        bool color=scale>1&&(integer.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);image(hd,VK_FORMAT_R32_UINT,usage|(color?VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:0)|(color&&context.orderedColor?VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT:0));image(hdRead,VK_FORMAT_R32_UINT,usage);image(output,VK_FORMAT_R8G8B8A8_UNORM,usage,1,1);
        VkSamplerCreateInfo sampling{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampling.magFilter=sampling.minFilter=VK_FILTER_NEAREST;sampling.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampling.addressModeU=sampling.addressModeV=sampling.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;checkGpu(vkCreateSampler(device,&sampling,nullptr,&sampler),"Create native Vulkan texture sampler");
        unsigned bindingCount=context.orderedColor&&color?9:8;std::array<VkDescriptorSetLayoutBinding,9> bindings{};for(unsigned i=0;i<bindings.size();i++){bindings[i].binding=i;bindings[i].descriptorCount=1;bindings[i].stageFlags=i==8?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_ALL;bindings[i].descriptorType=i==1?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:i<4?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:i==7?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:i==8?VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;}
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};set.bindingCount=bindingCount;set.pBindings=bindings.data();checkGpu(vkCreateDescriptorSetLayout(device,&set,nullptr,&setLayout),"Create native Vulkan descriptor layout");
        VkPushConstantRange push{VK_SHADER_STAGE_ALL,0,sizeof(Parameters)};VkPipelineLayoutCreateInfo pipeline{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pipeline.setLayoutCount=1;pipeline.pSetLayouts=&setLayout;pipeline.pushConstantRangeCount=1;pipeline.pPushConstantRanges=&push;checkGpu(vkCreatePipelineLayout(device,&pipeline,nullptr,&layout),"Create native Vulkan pipeline layout");
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3*SLOTS},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,SLOTS},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,3*SLOTS},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,SLOTS},{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,SLOTS}};VkDescriptorPoolCreateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};allocation.maxSets=SLOTS;allocation.poolSizeCount=5;allocation.pPoolSizes=sizes;checkGpu(vkCreateDescriptorPool(device,&allocation,nullptr,&descriptors),"Create native Vulkan descriptor pool");
        for(auto& slot:slots){
            buffer(slot.stream,streamCapacity,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true);VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};command.commandPool=pool;command.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;command.commandBufferCount=1;checkGpu(vkAllocateCommandBuffers(device,&command,&slot.command),"Allocate native Vulkan commands");VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fence.flags=VK_FENCE_CREATE_SIGNALED_BIT;checkGpu(vkCreateFence(device,&fence,nullptr,&slot.fence),"Create native Vulkan stream fence");
            VkDescriptorSetAllocateInfo descriptor{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};descriptor.descriptorPool=descriptors;descriptor.descriptorSetCount=1;descriptor.pSetLayouts=&setLayout;checkGpu(vkAllocateDescriptorSets(device,&descriptor,&slot.set),"Allocate native Vulkan descriptors");
            VkDescriptorBufferInfo buffers[]={{ram.handle,0,VRAM_BYTES},{slot.stream.handle,0,commandRange},{counter.handle,0,4},{snapshot.handle,0,VRAM_BYTES}};VkDescriptorImageInfo images[]={{VK_NULL_HANDLE,hd.view,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,hdRead.view,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,output.view,VK_IMAGE_LAYOUT_GENERAL},{sampler,hdRead.view,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,hd.view,VK_IMAGE_LAYOUT_GENERAL}};
            std::array<VkWriteDescriptorSet,9> updates{};for(unsigned i=0;i<bindingCount;i++){updates[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};updates[i].dstSet=slot.set;updates[i].dstBinding=i;updates[i].descriptorCount=1;updates[i].descriptorType=bindings[i].descriptorType;if(i<4)updates[i].pBufferInfo=&buffers[i];else updates[i].pImageInfo=&images[i-4];}vkUpdateDescriptorSets(device,bindingCount,updates.data(),0,nullptr);
        }
        compute(nativeDraw,PsxVulkanShaders::Draw);compute(nativeTransfer,PsxVulkanShaders::Transfer);compute(nativeScan,PsxVulkanShaders::Scanout);
        if(scale>1){compute(hdDraw,PsxVulkanShaders::HdDraw);compute(hdTransfer,PsxVulkanShaders::HdTransfer);compute(hdScan,PsxVulkanShaders::HdScanout);compute(nativeResolve,PsxVulkanShaders::NativeResolve);if(color){try{createOpaque();if(context.orderedColor)createOpaque(true);}catch(const std::exception&){if(opaqueDraw)vkDestroyPipeline(device,opaqueDraw,nullptr);opaqueDraw=VK_NULL_HANDLE;if(orderedDraw)vkDestroyPipeline(device,orderedDraw,nullptr);orderedDraw=VK_NULL_HANDLE;}}hdPending.reserve(HD_BATCH);}
        for(const Image* image:{&hd,&hdRead,&output}){VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=image->handle;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;vkCmdPipelineBarrier(begin(),VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);VkClearColorValue zero{};vkCmdClearColorImage(begin(),image->handle,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&barrier.subresourceRange);}
        vkCmdFillBuffer(begin(),ram.handle,0,VRAM_BYTES,0);vkCmdFillBuffer(begin(),snapshot.handle,0,VRAM_BYTES,0);vkCmdFillBuffer(begin(),counter.handle,0,4,0);barrier();dirtyTiles.fill(~uint64_t(0));
    }catch(...){release();throw;}}
    void release()noexcept{
        if(!device)return;
        // Construction failures can leave a recording buffer; it was never submitted.
        for(auto& slot:slots)if(slot.fence)vkWaitForFences(device,1,&slot.fence,VK_TRUE,UINT64_MAX);
        if(framebuffer)vkDestroyFramebuffer(device,framebuffer,nullptr);
        for(VkPipeline pipeline:{nativeDraw,nativeTransfer,nativeScan,hdDraw,hdTransfer,hdScan,opaqueDraw,orderedDraw,nativeResolve})if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);
        if(renderPass)vkDestroyRenderPass(device,renderPass,nullptr);if(layout)vkDestroyPipelineLayout(device,layout,nullptr);if(descriptors)vkDestroyDescriptorPool(device,descriptors,nullptr);if(setLayout)vkDestroyDescriptorSetLayout(device,setLayout,nullptr);if(sampler)vkDestroySampler(device,sampler,nullptr);
        for(auto& slot:slots){destroy(slot.stream);if(slot.fence)vkDestroyFence(device,slot.fence,nullptr);}if(pool)vkDestroyCommandPool(device,pool,nullptr);
        destroy(ram);destroy(snapshot);destroy(counter);destroy(readback);destroy(uploadBuffer);destroy(hd);destroy(hdRead);destroy(output);device=VK_NULL_HANDLE;
    }
    ~Impl(){release();}
};

NativeVulkanGpu::NativeVulkanGpu(PsxVulkanContext context,int scale):impl(std::make_unique<Impl>(context,scale)){}
NativeVulkanGpu::~NativeVulkanGpu()=default;
void NativeVulkanGpu::batch(const int* words,int length){
    for(int at=0;at<length;){int count=words[at++];if(count<2||count>12||at+count+7>length)throw std::runtime_error("Invalid native Vulkan GPU packet");int op=uint32_t(words[at])>>24,expected=op>=0x20&&op<0x40?((op&16)?((op&8)?4:3)*(2+((op>>2)&1)):1+((op&8)?4:3)*(1+((op>>2)&1))):op>=0x40&&op<0x60?((op&16)?4:3):op>=0x60&&op<0x80?2+((op>>2)&1)+((op&24)==0?1:0):0;if(count!=expected)throw std::runtime_error("Invalid native Vulkan packet length");impl->draw(words+at,words+at+count);at+=count+7;}
}
void NativeVulkanGpu::read(int* out){impl->read(out);}
void NativeVulkanGpu::set(const int* in){impl->set(in);}
void NativeVulkanGpu::fill(int color,int xy,int wh){impl->flush();int x=xy&0x3f0,y=(uint32_t(xy)>>16)&511,w=((wh&1023)+15)&~15,h=(uint32_t(wh)>>16)&511;if(!w||!h)return;int pixel=((color&255)>>3)|(((color>>8)&255)>>3<<5)|(((color>>16)&255)>>3<<10);impl->transfer(0,x,y,w,h,pixel,0,0,0);}
void NativeVulkanGpu::upload(const int* words,int count,int start,int x,int y,int width,int total,int mask){impl->flush();if(impl->orderedDraw){if(mask&2)impl->resolveNative();else impl->flushHd();}int samples=std::min(count*2,total-start);if(samples<=0)return;uint32_t offset=impl->commands(words,count*4);impl->transfer(1,x,y,width,(samples+width-1)/width,0,mask,start,samples,offset);impl->uploadBytes+=count*4;}
void NativeVulkanGpu::copy(int from,int to,int wh,int mask){
    impl->flush();int x=to&1023,y=(uint32_t(to)>>16)&511,sx=from&1023,sy=(uint32_t(from)>>16)&511,w=((wh-1)&1023)+1,h=(((uint32_t(wh)>>16)-1)&511)+1;RectList src,dst;GpuPrimitiveBuilder::split(src,sx,sy,w,h);GpuPrimitiveBuilder::split(dst,x,y,w,h);bool overlap=false;for(const Rect& a:src)for(const Rect& b:dst)overlap|=a.intersects(b);
    if(overlap&&(sx==x||sy==y))impl->transfer(3,x,y,w,h,sx,sy,mask,0);
    else if(overlap){std::vector<int> pixels(1024*512);impl->read(pixels.data());for(int row=0;row<h;row++)for(int col=0;col<w;col++){int at=((y+row)&511)*1024+((x+col)&1023);if(!(mask&2)||!(pixels[at]&32768))pixels[at]=pixels[((sy+row)&511)*1024+((sx+col)&1023)]|((mask&1)<<15);}impl->set(pixels.data(),false);if(impl->scale>1){Parameters params;params.operation=4;params.shape[0]=x;params.shape[1]=y;params.shape[2]=w;params.shape[3]=h;impl->dispatch(impl->hdTransfer,params,(w*impl->scale+7)/8,(h*impl->scale+7)/8);impl->barrier();impl->markWrapped(x,y,w,h);}}
    else impl->transfer(2,x,y,w,h,sx,sy,mask,0);
}
std::pair<int,int> NativeVulkanGpu::scanout(const int* state,bool nativeMovie){
    impl->flush();impl->flushHd();if(state[0]&0x200000)impl->resolveNative();int status=state[0],divider=(status&0x10000)?7:((status>>17)&3)==0?10:((status>>17)&3)==1?8:((status>>17)&3)==2?5:4;
    int sampling=nativeMovie?1:impl->scale,width=std::clamp((state[4]-state[3])/divider,1,1024)*sampling,height=std::clamp((state[6]-state[5])*((status&0x480000)==0x480000?2:1),1,512)*sampling;
    impl->resizeOutput(width,height);
    Parameters params;params.status=status;params.display[0]=state[1];params.display[1]=state[2];params.display[2]=width;params.display[3]=height;
    impl->barrier();impl->dispatch(impl->scale>1&&!nativeMovie?impl->hdScan:impl->nativeScan,params,(width+7)/8,(height+7)/8);impl->barrier();impl->watchDisplay(state,width/sampling,height/sampling);impl->scanouts++;return{width,height};
}
void NativeVulkanGpu::capture(const int* state,uint32_t* argb){auto [width,height]=scanout(state);impl->ensureReadback(VkDeviceSize(width)*height*4);VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={uint32_t(width),uint32_t(height),1};vkCmdCopyImageToBuffer(impl->begin(),impl->output.handle,VK_IMAGE_LAYOUT_GENERAL,impl->readback.handle,1,&copy);impl->hostReadBarrier();impl->wait();auto* rgba=static_cast<const uint32_t*>(impl->readback.mapped);for(int i=0;i<width*height;i++){uint32_t pixel=rgba[i];argb[i]=(pixel&0xff00ff00u)|((pixel&255)<<16)|((pixel>>16)&255);}}
void NativeVulkanGpu::submit(){impl->flush();impl->flushHd();impl->submit();}
void NativeVulkanGpu::wait(){impl->flush();impl->flushHd();impl->wait();}
VkImage NativeVulkanGpu::outputImage()const{return impl->output.handle;}
std::string NativeVulkanGpu::description()const{return impl->name+" / Native Vulkan "+std::to_string(impl->scale)+"x / "+(impl->scale==1?"integer compute":impl->orderedDraw?"HD ordered raster with deferred native resolves":impl->opaqueDraw?"HD opaque raster + ordered compute blending":"HD ordered compute");}
void NativeVulkanGpu::statistics(int64_t* out)const{std::fill_n(out,15,0);out[0]=impl->draws.load();out[1]=impl->feedbackDraws.load();out[2]=impl->readbacks.load();out[3]=impl->batches.load();out[5]=impl->copies.load();out[8]=impl->uploadBytes.load();out[9]=impl->scanouts.load();}
void NativeVulkanGpu::drawCounts(int64_t* out)const{out[0]=impl->nativeDraws.load();out[1]=impl->hdDraws.load();}
uint64_t NativeVulkanGpu::revision()const{return impl->contentRevision;}
bool NativeVulkanGpu::fastHd()const{return impl->orderedDraw!=VK_NULL_HANDLE;}
