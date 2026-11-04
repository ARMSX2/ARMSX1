// Run the production Vulkan renderer against the independent scalar rasterizer.
// No Android device or display surface is used by this executable.
#include "vulkan_gpu.h"
#include "gpu_primitives.h"
#include <cassert>
#include <cstdio>
#include <random>
#include <string>

namespace {
unsigned validationErrors=0;
VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void*){if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)validationErrors++;std::fprintf(stderr,"VULKAN VALIDATION: %s\n",data->pMessage);return VK_FALSE;}
void checked(VkResult result){static unsigned check=0;check++;if(result!=VK_SUCCESS)throw std::runtime_error("Vulkan test error "+std::to_string(result)+" at check "+std::to_string(check));}
struct Device {
    VkInstance instance=VK_NULL_HANDLE;VkDevice device=VK_NULL_HANDLE;PsxVulkanContext context;
    VkDebugUtilsMessengerEXT messenger=VK_NULL_HANDLE;
    Device(){
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="ArmSX1 native GPU validation";app.apiVersion=VK_API_VERSION_1_0;
        bool validate=std::getenv("ARMSX_GPU_VALIDATION")!=nullptr;const char* layer="VK_LAYER_KHRONOS_validation";const char* extensions[]={VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,VK_EXT_DEBUG_UTILS_EXTENSION_NAME,VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};create.pApplicationInfo=&app;create.enabledExtensionCount=validate?3:1;create.ppEnabledExtensionNames=extensions;
        VkValidationFeatureEnableEXT synchronization=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;VkValidationFeaturesEXT checks{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};checks.enabledValidationFeatureCount=1;checks.pEnabledValidationFeatures=&synchronization;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;debug.pfnUserCallback=validation;
        if(validate){checks.pNext=&debug;create.pNext=&checks;create.enabledLayerCount=1;create.ppEnabledLayerNames=&layer;}checked(vkCreateInstance(&create,nullptr,&instance));
        if(validate){auto make=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));checked(make(instance,&debug,nullptr,&messenger));}
        uint32_t count=0;checked(vkEnumeratePhysicalDevices(instance,&count,nullptr));std::vector<VkPhysicalDevice> devices(count);checked(vkEnumeratePhysicalDevices(instance,&count,devices.data()));
        for(auto physical:devices){uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,nullptr);std::vector<VkQueueFamilyProperties> properties(families);vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,properties.data());for(unsigned i=0;i<families;i++)if((properties[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)){context.physical=physical;context.family=i;break;}if(context.physical)break;}
        if(!context.physical)throw std::runtime_error("No Vulkan test device");float priority=1;VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queue.queueFamilyIndex=context.family;queue.queueCount=1;queue.pQueuePriorities=&priority;VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};info.queueCreateInfoCount=1;info.pQueueCreateInfos=&queue;
        uint32_t extensionCount=0;checked(vkEnumerateDeviceExtensionProperties(context.physical,nullptr,&extensionCount,nullptr));std::vector<VkExtensionProperties> deviceExtensions(extensionCount);checked(vkEnumerateDeviceExtensionProperties(context.physical,nullptr,&extensionCount,deviceExtensions.data()));const char* orderedExtension=nullptr;for(const auto& extension:deviceExtensions)if(std::string(extension.extensionName)==VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME||std::string(extension.extensionName)==VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME)orderedExtension=extension.extensionName;
        VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT ordered{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
        if(orderedExtension){auto query=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2KHR>(vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceFeatures2KHR"));if(query){VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&ordered;query(context.physical,&features);context.orderedColor=ordered.rasterizationOrderColorAttachmentAccess;if(context.orderedColor){ordered.rasterizationOrderDepthAttachmentAccess=ordered.rasterizationOrderStencilAttachmentAccess=VK_FALSE;info.pNext=&ordered;info.enabledExtensionCount=1;info.ppEnabledExtensionNames=&orderedExtension;}}}
        checked(vkCreateDevice(context.physical,&info,nullptr,&device));context.device=device;vkGetDeviceQueue(device,context.family,0,&context.queue);std::printf("Ordered color attachments: %s\n",context.orderedColor?"supported":"unavailable (fallback exercised)");
    }
    ~Device(){if(device)vkDestroyDevice(device,nullptr);if(messenger){auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));destroy(instance,messenger,nullptr);}if(instance)vkDestroyInstance(instance,nullptr);}
};
void compare(NativeVulkanGpu& gpu,const std::vector<int>& expected,const std::string& label){std::vector<int> actual(expected.size());gpu.read(actual.data());for(size_t i=0;i<actual.size();i++)if(actual[i]!=expected[i])throw std::runtime_error(label+" VRAM mismatch at "+std::to_string(i)+": "+std::to_string(actual[i])+" expected "+std::to_string(expected[i]));}
void draw(NativeVulkanGpu& gpu,Raster& reference,const std::vector<int>& packet,const int* env){std::vector<int> words;words.push_back(int(packet.size()));words.insert(words.end(),packet.begin(),packet.end());words.insert(words.end(),env,env+7);gpu.batch(words.data(),words.size());int op=uint32_t(packet[0])>>24;reference.e=env;if(op<0x40)reference.polygon(packet.data(),op);else if(op<0x60)reference.line(packet.data(),op);else reference.rectangle(packet.data(),op);}
void footprints(){
    int env[7]={0,256,0,0,1023|(511<<10),0,0};
    Primitive example=GpuPrimitiveBuilder::triangle(Vertex(0,0,200),Vertex(100,0,240),Vertex(100<<16,0,240),0x25,0,env);
    for(bool enhanced:{false,true}){auto range=GpuPrimitiveBuilder::textureReads(example,enhanced);assert(!GpuPrimitiveBuilder::feedback(example,range));}
    std::mt19937 random(0x544558);unsigned checkedSamples=0;
    for(unsigned fixture=0;fixture<6000;fixture++){
        int environment[7]={0,int((random()%3)<<7),int(random()&0xfffff),0,127|(127<<10),0,0};
        auto vertex=[&](){return Vertex(int(random()%128)|(int(random()%128)<<16),0,int(random()&65535));};
        Primitive p=GpuPrimitiveBuilder::triangle(vertex(),vertex(),vertex(),0x25,int(random()&0x7fff),environment);if(!p.bounds().valid())continue;
        int scale=1+random()%5;auto ranges=GpuPrimitiveBuilder::textureReads(p,scale>1);
        for(unsigned sample=0;sample<160;sample++){
            int x=p.q[4]*scale+random()%((p.q[6]-p.q[4]+1)*scale),y=p.q[5]*scale+random()%((p.q[7]-p.q[5]+1)*scale);bool inside=true;
            for(int e=0;e<3;e++){int at=16+e*4;i64 edge=i64(p.q[at])*scale+i64(p.q[at+1])*(x-p.q[4]*scale)+i64(p.q[at+2])*(y-p.q[5]*scale);inside&=edge>0||(edge==0&&p.q[at+3]);}if(!inside)continue;
            int uv[2];for(int component=3;component<5;component++){
                i64 fixed=i64(p.q[28+component])+i64(p.q[33+component])*(x/scale-p.q[4])+i64(p.q[38+component])*(y/scale-p.q[5]);
                fixed+=(p.q[33+component]/scale)*(x%scale)+(p.q[33+component]%scale)*(x%scale)/scale+(p.q[38+component]/scale)*(y%scale)+(p.q[38+component]%scale)*(y%scale)/scale;
                int mask=((p.q[10]>>(component==3?0:5))&31)*8,offset=((p.q[10]>>(component==3?10:15))&31)*8&mask;uv[component-3]=((int(fixed>>16)&255)&~mask)|offset;
            }
            int depth=(p.q[9]>>7)&3,shift=depth>=2?0:depth==1?1:2,tx=(((p.q[9]&15)*64)+(uv[0]>>shift))&1023,ty=((((p.q[9]>>4)&1)*256)+uv[1])&511;bool covered=false;for(const Rect& range:ranges)covered|=tx>=range.x0&&tx<=range.x1&&ty>=range.y0&&ty<=range.y1;assert(covered);checkedSamples++;
        }
    }
    std::printf("Texture footprints: false-feedback regression and %u covered native/HD samples passed.\n",checkedSamples);
}
void raster(Device& device,int scale){
    auto context=device.context;context.orderedColor=false; // Exercise the extension-free compute compatibility rules too.
    NativeVulkanGpu gpu(context,scale);std::puts(gpu.description().c_str());std::vector<int> expected(1024*512);std::mt19937 random(0x505358);for(auto& pixel:expected)pixel=random()&65535;gpu.set(expected.data());Raster reference{expected.data(),nullptr};
    constexpr int ops[]={0x20,0x22,0x24,0x25,0x26,0x28,0x2c,0x30,0x34,0x38,0x3c,0x40,0x42,0x50,0x52,0x60,0x62,0x64,0x65,0x66,0x68,0x6c,0x70,0x74,0x78,0x7c};
    unsigned fixtures=0;
    for(int depth=0;depth<3;depth++)for(int mode=0;mode<4;mode++)for(int op:ops){
        // Keep source textures/CLUTs outside the destination for the live-RAM scalar
        // oracle. True feedback is tested separately against the hardware snapshot contract.
        int env[]={0,8|(depth<<7)|(mode<<5)|512,int(random()&0xfffff),0,127|(127<<10),0,int(fixtures%4)};
        std::vector<int> packet;packet.push_back(int(uint32_t(op)<<24)|(random()&0xffffff));int count=op<0x40?((op&8)?4:3):op<0x60?2:1;int xy[]={8|(12<<16),100|(18<<16),20|(104<<16),110|(112<<16)};
        for(int i=0;i<count;i++){if(i&&(op&16))packet.push_back(random()&0xffffff);packet.push_back(xy[i]);if((op<0x40||op>=0x60)&&(op&4))packet.push_back((random()&65535)|((400<<6)<<16));}if(op>=0x60&&!(op&24))packet.push_back(32|(24<<16));
        draw(gpu,reference,packet,env);compare(gpu,expected,"op "+std::to_string(op)+" fixture "+std::to_string(fixtures++));
    }
    int env[]={0,0,0,0,1023|(511<<10),0,0};
    // More than three stream capacities: live command storage must never be overwritten.
    for(int n=0;n<14000;n++)draw(gpu,reference,{int(0x60000000u|(n*123457u&0xffffff)),(n*13&255)|((n*7&127)<<16),4|(4<<16)},env);
    compare(gpu,expected,"stream-slot wrap and pending draw order");
    std::array<int,512> words{};for(int& word:words)word=random();gpu.upload(words.data(),words.size(),3,1000,510,37,1050,1);
    for(int i=0;i<1024;i++){int index=3+i,at=((510+index/37)&511)*1024+((1000+index%37)&1023);expected[at]=(uint32_t(words[i/2])>>((i&1)*16)&65535)|32768;}compare(gpu,expected,"wrapped upload");
    for(int mask=0;mask<4;mask++)for(auto xy:std::array<std::array<int,4>,3>{{{{1000,510,1004,510}},{{32,500,32,506}},{{1000,510,1004,1}}}}){
        int sx=xy[0],sy=xy[1],dx=xy[2],dy=xy[3];gpu.copy(sx|(sy<<16),dx|(dy<<16),32|(8<<16),mask);
        for(int y=0;y<8;y++)for(int x=0;x<32;x++){int at=((dy+y)&511)*1024+((dx+x)&1023);if(!(mask&2)||!(expected[at]&32768))expected[at]=expected[((sy+y)&511)*1024+((sx+x)&1023)]|((mask&1)<<15);}compare(gpu,expected,"overlapping copy");
    }
    gpu.fill(0x00ff00,1009|(510<<16),31|(4<<16));for(int y=0;y<4;y++)for(int x=0;x<32;x++)expected[((510+y)&511)*1024+((1008+x)&1023)]=31<<5;compare(gpu,expected,"wrapped fill");
    for(int mode:{0,0x20000,0x40000,0x60000,0x10000,0x200000,0x220000,0x480000,0x680000,0x800000}){
        int state[]={mode,1007,510,17,179,3,8,0};int divider=(mode&0x10000)?7:((mode>>17)&3)==0?10:((mode>>17)&3)==1?8:((mode>>17)&3)==2?5:4;int width=(179-17)/divider,height=5*((mode&0x480000)==0x480000?2:1);std::vector<uint32_t> pixels(width*height*scale*scale);gpu.capture(state,pixels.data());
        for(int y=0;y<height;y++)for(int x=0;x<width;x++){uint32_t color=0xff000000u;int row=((510+y)&511)*1024;if(!(mode&0x800000)){if(!(mode&0x200000)){int v=expected[row+((1007+x)&1023)];unsigned r=v&31,g=(v>>5)&31,b=(v>>10)&31;color|=((r<<3)|(r>>2))<<16|((g<<3)|(g>>2))<<8|((b<<3)|(b>>2));}else{auto byte=[&](int at){return(expected[row+((at>>1)&1023)]>>((at&1)*8))&255;};int at=1007*2+x*3;color|=byte(at)<<16|byte(at+1)<<8|byte(at+2);}}
            // Canonical transfer regions retain identical pixels at every HD sample.
            if(mode&0x200000||scale==1)assert(pixels[y*scale*width*scale+x*scale]==color);
        }
    }
    // Independently known HD colors: opaque graphics, ordered blending, mask tests.
    std::fill(expected.begin(),expected.end(),0);gpu.set(expected.data());
    draw(gpu,reference,{int(0x600000ff),0,16|(16<<16)},env);
    draw(gpu,reference,{int(0x6200ff00),4|(4<<16),8|(8<<16)},env);
    int state[]={0,0,0,0,160,0,16,0};std::vector<uint32_t> pixels(16*16*scale*scale);gpu.capture(state,pixels.data());
    assert(pixels[0]==0xffff0000u);assert(pixels[(5*scale)*(16*scale)+5*scale]==0xff7b7b00u);
    int64_t counts[2];gpu.drawCounts(counts);assert(counts[0]>0);if(scale>1)assert(counts[1]>0);
    gpu.wait();std::printf("Native Vulkan %dx: %u raster fixtures, 14,000 streamed draws, transfers, scanout and HD pixels passed.\n",scale,fixtures);
}
void fastRaster(Device& device){
    if(!device.context.orderedColor){std::puts("Fast HD path unavailable: this device retains GLES for production HD rendering.");return;}
    NativeVulkanGpu gpu(device.context,3);assert(gpu.fastHd());std::puts(gpu.description().c_str());std::vector<int> expected(1024*512);Raster reference{expected.data(),nullptr};int env[]={0,0,0,0,1023|(511<<10),0,0};
    for(int i=0;i<14000;i++){env[1]=(i%4)<<5;env[6]=i%11==0?2:i%7==0?1:0;int op=i%3?0x60:0x62;draw(gpu,reference,{int(uint32_t(op)<<24|(i*123457u&0xffffff)),(i*13&255)|((i*7&127)<<16),4|(4<<16)},env);}
    compare(gpu,expected,"3x ordered blending and deferred native resolves");int64_t counts[2];gpu.drawCounts(counts);assert(counts[0]==0&&counts[1]>0&&counts[1]<16);
    std::printf("3x fast batching: 14,000 primitives, %lld HD draws, no duplicate native draws.\n",static_cast<long long>(counts[1]));
    std::fill(expected.begin(),expected.end(),0);for(int x=0;x<16;x++)expected[x]=x+1;for(int x=32;x<48;x++)expected[32*1024+x]=(x-31)<<5;gpu.set(expected.data());env[1]=0;env[6]=0;
    draw(gpu,reference,{int(0x2000ff00),16|(16<<16),20|(16<<16),16|(20<<16)},env);
    int state[]={0,0,0,0,480,0,48,0};std::vector<uint32_t> before(48*48*9),after(before.size());gpu.capture(state,before.data());env[1]=256;
    auto feedback=[&](int x,int y){std::vector<int> old=expected;draw(gpu,reference,{int(0x65000000),x|(y<<16),x|(y<<8),8|(1<<16)},env);for(int i=0;i<8;i++)expected[y*1024+x+i]=old[y*1024+x+i];};
    feedback(0,0);feedback(32,32);gpu.capture(state,after.data());for(int y=16*3;y<24*3;y++)for(int x=16*3;x<24*3;x++)assert(before[y*144+x]==after[y*144+x]);
    // Sampling a freshly drawn HD triangle must retain every subpixel, including
    // pixels that differ from an upscaled native-resolution image.
    draw(gpu,reference,{int(0x65000000),128|(128<<16),16|(16<<8),8|(8<<16)},env);
    int wideState[]={0,0,0,0,1440,0,144,0};std::vector<uint32_t> copied(144*144*9);gpu.capture(wideState,copied.data());for(int y=0;y<24;y++)for(int x=0;x<24;x++)assert(copied[(128*3+y)*432+128*3+x]==before[(16*3+y)*144+16*3+x]);
    // A real overlapping texture copy must read its pre-draw snapshot, not pixels
    // overwritten earlier in the same primitive.
    std::fill(expected.begin(),expected.end(),0);for(int x=0;x<16;x++)expected[x]=x+1;gpu.set(expected.data());auto old=expected;draw(gpu,reference,{int(0x65000000),2,0,8|(1<<16)},env);for(int x=0;x<8;x++)expected[x+2]=old[x];compare(gpu,expected,"feedback snapshot");
    env[1]=0;env[6]=1;draw(gpu,reference,{int(0x600000ff),16|(16<<16),16|(16<<16)},env);int word=0x03e003e0;gpu.upload(&word,1,0,16,16,2,2,2);compare(gpu,expected,"masked upload after deferred draw");
    // 4-bit CLUT entries, including STP and opaque texels, use the same ordered
    // raster pipeline as direct-color polygons and retain PS1 blend arithmetic.
    std::fill(expected.begin(),expected.end(),0);for(int y=0;y<8;y++)for(int x=0;x<2;x++)expected[(200+y)*1024+512+x]=0x3210;for(int i=0;i<16;i++)expected[300*1024+i]=((i+1)&31)|((i&1)?32768:0);gpu.set(expected.data());env[1]=8;env[6]=0;
    draw(gpu,reference,{int(0x600000ff),0,8|(8<<16)},env);draw(gpu,reference,{int(0x67000000),0,int(uint32_t(300<<6)<<16)|(200<<8),8|(8<<16)},env);compare(gpu,expected,"paletted semitransparency");
    std::fill(expected.begin(),expected.end(),0);gpu.set(expected.data());compare(gpu,expected,"restore after queued HD writes");
    gpu.capture(state,after.data());auto revision=gpu.revision();gpu.fill(0x00ff00,512|(256<<16),16|(1<<16));assert(gpu.revision()==revision);gpu.fill(0x00ff00,16|(16<<16),16|(1<<16));assert(gpu.revision()>revision);
    int wrappedState[]={0,1007,510,0,320,0,4,0};std::vector<uint32_t> wrapped(32*4*9);gpu.capture(wrappedState,wrapped.data());revision=gpu.revision();gpu.fill(0x0000ff,0,16|(1<<16));assert(gpu.revision()>revision);revision=gpu.revision();gpu.fill(0x0000ff,64|(64<<16),16|(1<<16));assert(gpu.revision()==revision);
    gpu.wait();std::puts("3x fast native Vulkan: ordered masks/blending, feedback snapshots, untouched HD gaps, masked uploads, restore and visible-frame reuse passed.");
}
}
int main(){std::setvbuf(stdout,nullptr,_IONBF,0);try{footprints();{Device device;raster(device,1);raster(device,2);raster(device,3);fastRaster(device);}if(validationErrors)throw std::runtime_error(std::to_string(validationErrors)+" Vulkan validation errors");return 0;}catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}}
