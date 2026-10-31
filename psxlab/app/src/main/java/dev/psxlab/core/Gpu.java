package dev.psxlab.core;

import java.util.Arrays;
import java.util.concurrent.*;

/** Original GP0 software backend with functional primitive rasterization. Ordered bounded queue provides backpressure. */
public final class Gpu implements AutoCloseable {
    private static final int[] DISPLAY_COLORS=new int[65536];
    static{for(int i=0;i<DISPLAY_COLORS.length;i++)DISPLAY_COLORS[i]=argb(i);}
    private final int[] vram = new int[1024 * 512];
    private final ThreadPoolExecutor worker;
    private final int[] packet = new int[12];
    private final int[] drawBatch=new int[512];
    private int drawBatchWords,drawBatchCount;
    private final SoftwareRasterizer raster=new SoftwareRasterizer(vram);
    private final boolean hardwareDrawing;
    private final int internalScale,presentationMode;
    private final boolean baseVulkanOutput;
    private volatile boolean shaderPresenterFailed;
    private boolean displayStretch,displayInteger;
    private String baseRenderer;
    private volatile String shaderPresenterError="";
    private volatile HardwareRasterizer hardware;
    private record HighRefresh(int mode,int multiple,float hz,boolean allowed){}
    private volatile HighRefresh highRefresh=new HighRefresh(0,2,0,true);
    private volatile int presentationDecision;
    private volatile float presentationRate=60,shaderAspect=4f/3f;
    private volatile long blackPresented,shaderSubframes;
    private boolean displayVsync;
    public void configurePresentation(int mode,int multiple,float hz,boolean allowed){highRefresh=new HighRefresh(Math.max(0,Math.min(2,mode)),Math.max(2,Math.min(4,multiple)),hz,allowed);}
    public boolean presentationNeedsRestore(){return presentationDecision==PresentationPolicy.BFI;}
    public String presentationStatus(){HighRefresh setting=highRefresh;int decision=PresentationPolicy.evaluate(setting.mode,setting.multiple,setting.hz,presentationRate,setting.allowed,hardware!=null||vulkan!=null,vulkan!=null,setting.mode==2&&ShaderChain.active());return PresentationPolicy.describe(decision,setting.multiple,setting.hz,presentationRate);}
    public long blackFramesPresented(){return blackPresented;}
    public long shaderSubframesPresented(){return shaderSubframes;}
    private int presentationDecision(int[] state,boolean allowed){
        HighRefresh setting=highRefresh;presentationRate=(state[0]&0x100000)!=0?50:60;
        int decision=PresentationPolicy.evaluate(setting.mode,setting.multiple,setting.hz,presentationRate,setting.allowed&&allowed,hardware!=null||vulkan!=null,vulkan!=null,setting.mode==2&&ShaderChain.active());
        if(decision!=presentationDecision&&journal!=null)journal.event(EngineLog.INFO,"presentation.mode",java.util.Map.of("decision",decision,"mode",setting.mode,"multiplier",setting.multiple,"display_hz",setting.hz,"game_hz",presentationRate,"status",PresentationPolicy.describe(decision,setting.multiple,setting.hz,presentationRate)));
        presentationDecision=decision;boolean sync=decision==PresentationPolicy.BFI;
        if(hardware!=null&&sync!=displayVsync){hardware.setVsync(sync);displayVsync=sync;}
        return decision;
    }
    private void presentExtra(android.view.Surface surface,int decision,EngineLog.Frame timing){
        if(surface==null||!surface.isValid()||(decision!=PresentationPolicy.BFI&&decision!=PresentationPolicy.SHADER))return;
        int multiple=highRefresh.multiple;
        for(int phase=2;phase<=multiple;phase++){
            try(EngineLog.Span span=timing==null?null:timing.stage(decision==PresentationPolicy.BFI?"gpu.present.bfi":"gpu.present.shader_subframe")){
                boolean accepted;
                if(decision==PresentationPolicy.BFI){accepted=vulkan!=null?vulkan.presentBlack(surface):hardware!=null&&hardware.presentBlack(surface);if(accepted)blackPresented++;}
                else{if(vulkan==null||!ShaderChain.active())break;ShaderChain.phase(multiple,phase,presentationRate,shaderAspect);accepted=vulkan.presentSubframe(surface);if(accepted)shaderSubframes++;}
                if(timing!=null)timing.extraPresentation(phase,accepted);
                if(!accepted)break;
            }
        }
    }
    public volatile boolean performanceEnabled;
    public long performanceTriangles,performanceRectangles,performanceLines;
    public record Performance(long presentNanos,int width,int height,int scale) {}
    private volatile Performance performance=new Performance(0,0,0,1);
    public Performance performance(){return performance;}
    private void measurePresentation(long start,int[] state){
        if(start==0)return;
        int divider=(state[0]&0x10000)!=0?7:switch((state[0]>>>17)&3){case 0->10;case 1->8;case 2->5;default->4;};
        int width=Math.max(1,Math.min(1024,(state[4]-state[3])/divider));
        int height=Math.max(1,Math.min(512,(state[6]-state[5])*((state[0]&0x480000)==0x480000?2:1)));
        performance=new Performance(System.nanoTime()-start,width,height,internalScale);
    }
    private volatile EngineLog journal;
    private long nextNativeSnapshot;
    private volatile java.util.Map<String,Object> lastNativeTiming=java.util.Map.of();
    public java.util.Map<String,Object> nativeTimings(){return lastNativeTiming;}
    public void setJournal(EngineLog value){journal=value;}
    private volatile VulkanPresenter vulkan;
    private boolean hardwareDirty;
    private volatile String renderer=NativeRasterizer.AVAILABLE?"Native software":"Java software";
    public String renderer(){return renderer;}
    /** Active presentation objects, independent of the rasterizer or requested settings. */
    public String displayRenderer(){return vulkan!=null?"Vulkan":hardware!=null?"OpenGL":"Software";}
    public String renderStatistics(){HardwareRasterizer active=hardware;VulkanPresenter output=vulkan;return "GPU queue="+queuedCommands()+" "+(active==null?renderer:active.statistics())+(output==null?"":" "+vulkanFrames.statistics()+" "+output.statistics())+" · "+presentationStatus()+" black="+blackPresented+" shader_subframes="+shaderSubframes;}
    private boolean polyline,polyShaded,polyHalf;
    private int polyCommand,polyXy,polyColor,pendingPolyColor,displayX,displayY;
    private int count,expected,gpuStatus=0x14802000,gpuRead;
    private int uploadX,uploadY,uploadW,uploadH,uploaded;
    private final int[] uploadBuffer=new int[512];
    private int uploadWords,uploadStart;
    private int readX,readY,readW,readH,readPixels;
    private boolean readSynchronized;
    private long softwareVramRevision;
    private final ScanoutCache vulkanFrames=new ScanoutCache();
    private final int[] environment=new int[7];
    private int horizontalStart=0x200,horizontalEnd=0xc00,verticalStart=0x10,verticalEnd=0x100;
    private long videoRemainder;
    private int line,halfCycle,dotHalfCycles;
    private boolean videoInitialized,hblank,vblank,field;
    public long fields,vblanks;
    private volatile RuntimeException failure;
    public boolean diagnosticsEnabled;
    private java.util.function.Consumer<String> traceSink;
    public void setTraceSink(java.util.function.Consumer<String> sink){traceSink=sink;}
    public volatile long diagnosticQueueWait,diagnosticDraw,diagnosticScanout;
    public int queuedCommands(){return worker==null?0:worker.getQueue().size();}
    public Gpu(boolean threaded){this(threaded,false,false);}
    public Gpu(boolean threaded,boolean hardwareDrawing){this(threaded,hardwareDrawing,true);}
    public Gpu(boolean threaded,int renderer){this(threaded,renderer==1,renderer==1,renderer==2);}
    public Gpu(boolean threaded,int renderer,int scale){this(threaded,renderer==1||scale>1,renderer==1||scale>1,renderer==2,scale,renderer);}
    public Gpu(boolean threaded,int output,int scale,boolean hardwareRasterizer){this(threaded,hardwareRasterizer||scale>1,hardwareRasterizer||scale>1||output!=2,output==2,scale,output);}
    private Gpu(boolean threaded,boolean hardwareDrawing,boolean texturePresentation){this(threaded,hardwareDrawing,texturePresentation,false);}
    private Gpu(boolean threaded,boolean hardwareDrawing,boolean texturePresentation,boolean vulkanOutput){this(threaded,hardwareDrawing,texturePresentation,vulkanOutput,1,vulkanOutput?2:texturePresentation?1:0);}
    private Gpu(boolean threaded,boolean hardwareDrawing,boolean texturePresentation,boolean vulkanOutput,int scale,int presentationMode) {
        if(scale<1||scale>5)throw new IllegalArgumentException("Invalid internal resolution");
        this.internalScale=scale;this.presentationMode=presentationMode;
        this.baseVulkanOutput=vulkanOutput;
        if(scale>1&&!NativeRasterizer.AVAILABLE)throw new IllegalStateException("HD rendering requires the native GPU module");
        this.hardwareDrawing=hardwareDrawing;
        Arrays.fill(vram, 0);
        worker = threaded ? new ThreadPoolExecutor(1, 1, 0, TimeUnit.SECONDS,
            new ArrayBlockingQueue<>(256), r -> new Thread(()->{
                // HD feeds a real-time display; keep driver submission ahead of background work.
                if(scale>1)try{android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_DISPLAY);}catch(SecurityException ignored){}
                r.run();
            }, "PSX-GPU"),
            (r, executor) -> {
                if (executor.isShutdown()) throw new RejectedExecutionException("GPU closed");
                try { long before=diagnosticsEnabled?System.nanoTime():0;executor.getQueue().put(r);if(diagnosticsEnabled)diagnosticQueueWait+=System.nanoTime()-before; }
                catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new RejectedExecutionException(e); }
            }) : null;
        if(texturePresentation && NativeRasterizer.AVAILABLE)submitDirect(()->{
            try{hardware=new HardwareRasterizer(scale);renderer=scale>1?scale+"× HD drawing / "+(presentationMode==2?"Vulkan":"OpenGL")+" display":hardwareDrawing?"OpenGL ES compute":"Native software + OpenGL display";}
            catch(RuntimeException e){if(scale>1)throw e;android.util.Log.w("PSXGpu","GLES unavailable; using the software display",e);}
        });
        if(vulkanOutput)submitDirect(()->{try{vulkan=new VulkanPresenter(scale);renderer="Vulkan display / "+(scale>1?scale+"× OpenGL compute rasterizer":hardwareDrawing?"OpenGL compute rasterizer":"native software rasterizer");}catch(RuntimeException e){renderer=hardware!=null?"OpenGL display (Vulkan unavailable)":"Software (Vulkan unavailable)";android.util.Log.w("PSXGpu","Vulkan display unavailable",e);}});
    }
    private void reconcileShaderPresentation(){
        if(baseVulkanOutput)return;
        if(ShaderChain.requested()){
            if(vulkan==null&&!shaderPresenterFailed){
                try{
                    VulkanPresenter output=new VulkanPresenter(internalScale);
                    try{if(hardware!=null)hardware.detachPresentation();output.configureDisplay(shaderAspect,displayStretch,displayInteger);}
                    catch(RuntimeException error){output.close();throw error;}
                    vulkan=output;baseRenderer=renderer;renderer=baseRenderer+" / Vulkan shader presentation";ShaderChain.backend(2);
                }catch(RuntimeException error){shaderPresenterFailed=true;shaderPresenterError=error.getMessage();if(journal!=null)journal.error("gpu.shader.presenter",error);android.util.Log.w("PSXGpu","Shader presentation unavailable; retaining selected renderer",error);}
            }
        }else{
            shaderPresenterFailed=false;shaderPresenterError="";
            if(vulkan!=null){vulkan.close();vulkan=null;if(hardware!=null)hardware.detachPresentation();renderer=baseRenderer;ShaderChain.backend(presentationMode);}
        }
    }
    public String shaderStatus(){return shaderPresenterFailed?"Shader presentation unavailable: "+shaderPresenterError:ShaderChain.status();}
    private void shaderPresentationFailed(RuntimeException error){
        if(journal!=null)journal.error("gpu.shader.presenter",error);
        if(vulkan!=null){vulkan.close();vulkan=null;}
        if(hardware!=null)hardware.detachPresentation();
        shaderPresenterError=error.toString();shaderPresenterFailed=true;renderer=baseRenderer+" (shader unavailable)";
    }
    public void prepareShaders(){if(ShaderChain.requested()){submit(()->{reconcileShaderPresentation();if(vulkan!=null)ShaderChain.prepare();});sync();}}
    /** All native display state stays owned by the GPU worker. */
    public void configureDisplay(float aspect,boolean stretch,boolean integerScale){submit(()->{
        shaderAspect=aspect;
        displayStretch=stretch;displayInteger=integerScale;
        if(hardware!=null)hardware.configureDisplay(aspect,stretch,integerScale);
        if(vulkan!=null)vulkan.configureDisplay(aspect,stretch,integerScale);
    });}
    private void submit(Runnable r) {flushDrawBatch();submitDirect(r);}
    private void submitDirect(Runnable r) {
        check();
        EngineLog log=journal;Runnable guarded;
        if(log!=null&&log.enabled()){long frame=log.currentFrame();guarded=()->{log.enterFrame(frame);try{r.run();}catch(RuntimeException e){failure=e;log.error("gpu.command",e);}finally{log.leaveFrame();}};}
        else guarded = () -> { try { r.run(); } catch (RuntimeException e) { failure = e; } };
        if (worker == null) guarded.run(); else worker.execute(guarded);
    }
    private void check() { if (failure != null) throw failure; }
    private void draw(int[] p){draw(p,p.length);}
    private void draw(int[] p,int length){
        int op=p[0]>>>24;
        if(performanceEnabled){if(op<0x40)performanceTriangles+=(op&8)!=0?2:1;else if(op<0x60)performanceLines++;else performanceRectangles++;}
        if(op<0x40 && (op&4)!=0){int page=p[(op&16)!=0?5:4]>>>16;environment[1]=(environment[1]&~0x9ff)|(page&0x9ff);gpuStatus=(gpuStatus&~0x81ff)|(page&511)|((page&0x800)<<4);}
        if(drawBatchWords+length+8>drawBatch.length)flushDrawBatch();
        drawBatch[drawBatchWords++]=length;System.arraycopy(p,0,drawBatch,drawBatchWords,length);drawBatchWords+=length;
        System.arraycopy(environment,0,drawBatch,drawBatchWords,7);drawBatchWords+=7;drawBatchCount++;
        if(drawBatchCount>=16)flushDrawBatch();
    }
    private void flushDrawBatch(){
        if(drawBatchCount==0)return;
        int[] commands=Arrays.copyOf(drawBatch,drawBatchWords);int count=drawBatchCount;drawBatchWords=drawBatchCount=0;
        submitDirect(()->{long before=diagnosticsEnabled?System.nanoTime():0;try(EngineLog.Span span=journal==null?null:journal.begin(hardwareDrawing?"gpu.raster.gles":"gpu.raster.software",EngineLog.TRACE)){if(hardware!=null && hardwareDrawing){hardware.drawBatch(commands);hardwareDirty=true;raster.primitives+=count;}else{raster.drawBatch(commands,count);softwareVramRevision++;}}if(diagnosticsEnabled)diagnosticDraw+=System.nanoTime()-before;});
    }
    private static int packetSize(int op){
        if(op>=0x20 && op<0x40){int n=(op&8)!=0?4:3;return (op&16)!=0?n*(2+((op>>>2)&1)):1+n*(1+((op>>>2)&1));}
        if(op>=0x40 && op<0x60)return (op&16)!=0?4:3;
        if(op>=0x60 && op<0x80)return 2+((op>>>2)&1)+((op&24)==0?1:0);
        return op==2 || op>=0xa0 && op<=0xdf?3:op>=0x80 && op<=0x9f?4:0;
    }
    public void write(int word) {
        check();
        readSynchronized=false;
        if(polyline){
            if(!polyHalf && (word&0xf000f000)==0x50005000){polyline=false;return;}
            if(polyShaded && !polyHalf){pendingPolyColor=word;polyHalf=true;return;}
            int[] segment=polyShaded?new int[]{(polyCommand&0xff000000)|(polyColor&0xffffff),polyXy,pendingPolyColor,word}:new int[]{polyCommand,polyXy,word};
            draw(segment);polyXy=word;if(polyShaded)polyColor=pendingPolyColor;polyHalf=false;return;
        }
        if(uploaded<uploadW*uploadH){
            if(uploadWords==0)uploadStart=uploaded;
            uploadBuffer[uploadWords++]=word;uploaded+=Math.min(2,uploadW*uploadH-uploaded);
            if(uploadWords==uploadBuffer.length || uploaded==uploadW*uploadH)flushUpload();return;
        }
        if(count==0){
            int op=word>>>24;
            if(op==0x1f){gpuStatus|=0x01000000;return;}
            if(op==0 || op==1 || op==3 || op>=4 && op<=0x1e || op==0xe0 || op>=0xe7 && op<=0xef)return;
            if(op>=0xe1 && op<=0xe6){environment[op-0xe0]=word&0xffffff;if(op==0xe1)gpuStatus=(gpuStatus&~0x87ff)|(word&0x7ff)|((word&0x800)<<4);if(op==0xe6)gpuStatus=(gpuStatus&~0x1800)|((word&3)<<11);return;}
            expected=packetSize(op);
            if(expected==0)throw new UnsupportedOperationException(String.format("GP0 %02X drawing command is not implemented",op));
        }
        packet[count++] = word;
        if(count==expected){
            int command=packet[0],xy=packet[1],wh=packet[2],extra=packet[3],op=command>>>24;count=0;
            if(op>=0x20 && op<0x80){
                draw(packet,expected);
                if(op>=0x40 && op<0x60 && (op&8)!=0){polyline=true;polyShaded=(op&16)!=0;polyHalf=false;polyCommand=command;polyXy=packet[polyShaded?3:2];polyColor=polyShaded?packet[2]:command;}
            }else if(op==2){submit(()->fill(command,xy,wh));}
            else if(op>=0x80 && op<=0x9f){
                int w=((extra-1)&1023)+1,h=(((extra>>>16)-1)&511)+1,mask=environment[6];
                submit(()->{if(hardware!=null && hardwareDrawing){hardware.copy(xy,wh,extra,mask);hardwareDirty=true;}else{for(int y=0;y<h;y++)for(int x=0;x<w;x++)store(((((wh>>>16)+y)&511)*1024)+((wh+x)&1023),vram[((((xy>>>16)+y)&511)*1024)+((xy+x)&1023)],mask);softwareVramRevision++;}});
            }else if(op>=0xa0 && op<=0xbf){uploadX=xy&1023;uploadY=(xy>>>16)&511;uploadW=((wh-1)&1023)+1;uploadH=(((wh>>>16)-1)&511)+1;uploaded=0;}
            // Queue earlier draws now; the first read publishes their VRAM once.
            // Parsing the transfer header does not itself consume rendered data.
            else{flushDrawBatch();readX=xy&1023;readY=(xy>>>16)&511;readW=((wh-1)&1023)+1;readH=(((wh>>>16)-1)&511)+1;readPixels=0;}
        }
    }
    private void flushUpload(){
        if(uploadWords==0)return;
        int[] words=Arrays.copyOf(uploadBuffer,uploadWords);int start=uploadStart,x=uploadX,y=uploadY,w=uploadW,total=uploadW*uploadH,mask=environment[6];uploadWords=0;
        submit(()->{if(hardware!=null && hardwareDrawing){hardware.upload(words,start,x,y,w,total,mask);hardwareDirty=true;}else{for(int i=0;i<words.length*2 && start+i<total;i++)store(((y+(start+i)/w)&511)*1024+((x+(start+i)%w)&1023),(words[i/2]>>>((i&1)*16))&65535,mask);softwareVramRevision++;}});
    }
    public int read(){
        check();if(readPixels>=readW*readH)return gpuRead;
        // A VRAM transfer can contain 262144 words. The fence publishes all
        // earlier drawing/readback once; subsequent words read that same VRAM.
        // A new GP0/GP1 write or state restore invalidates this host-only fence.
        if(!readSynchronized){sync(true);readSynchronized=true;}
        int value=0;for(int i=0;i<2;i++){int at=readPixels++;value|=vram[((readY+at/readW)&511)*1024+((readX+at%readW)&1023)]<<(16*i);}
        return gpuRead=value;
    }
    public int status(){
        int result=gpuStatus|0x14000000|(readPixels<readW*readH?0x08000000:0),direction=(gpuStatus>>>29)&3;
        boolean request=direction==1 || direction==2 || direction==3 && (result&0x08000000)!=0;
        return (result&~0x02000000)|(request?0x02000000:0);
    }
    public void control(int word){
        readSynchronized=false;
        int op=(word>>>24)&63;
        if(traceSink!=null && (op==5 || op==8))traceSink.accept(String.format(java.util.Locale.ROOT,"GP1_%02X %08X vblank=%d",op,word,vblanks));
        switch(op){
            case 0 -> {sync();count=uploadW=uploadH=readW=readH=0;polyline=polyHalf=false;displayX=displayY=0;gpuStatus=0x14802000;horizontalStart=0x200;horizontalEnd=0xc00;verticalStart=0x10;verticalEnd=0x100;Arrays.fill(environment,0);}
            case 1 -> {flushUpload();count=uploadW=uploadH=0;polyline=polyHalf=false;}
            case 2 -> gpuStatus&=~0x01000000;
            case 3 -> gpuStatus=(gpuStatus&~0x800000)|((word&1)<<23);
            case 4 -> gpuStatus=(gpuStatus&~0x60000000)|((word&3)<<29);
            case 5 -> {displayX=word&1023;displayY=(word>>>10)&511;}
            case 6 -> {horizontalStart=word&4095;horizontalEnd=(word>>>12)&4095;}
            case 7 -> {verticalStart=word&1023;verticalEnd=(word>>>10)&1023;}
            case 8 -> gpuStatus=(gpuStatus&~0x7f4000)|((word&63)<<17)|((word&64)<<10);
            case 9 -> {}
            default -> {
                if(op>=16 && op<=31){int query=word&15;gpuRead=switch(query){case 2->environment[2];case 3->environment[3];case 4->environment[4];case 5->environment[5];case 7->2;case 8->0;default->gpuRead;};}
                else throw new UnsupportedOperationException(String.format("GP1 %02X is not implemented",op));
            }
        }
    }
    /** Nominal video-clock scheduling; instruction stalls and interlaced half-lines remain approximate. */
    int clocksToEvent(Timers timers){
        if(!videoInitialized || (gpuStatus&0x01000000)!=0)return 1;
        boolean pal=(gpuStatus&0x100000)!=0;int period=pal?6810:6825;
        int boundary=period;if(halfCycle<horizontalStart*2)boundary=Math.min(boundary,horizontalStart*2);if(halfCycle<horizontalEnd*2)boundary=Math.min(boundary,horizontalEnd*2);
        long halves=Math.max(1,boundary-halfCycle);int dots=timers.dotClocksToEvent();
        if(dots!=Integer.MAX_VALUE){int div=(gpuStatus&0x10000)!=0?7:switch((gpuStatus>>>17)&3){case 0->10;case 1->8;case 2->5;default->4;};halves=Math.min(halves,(long)dots*div*2-dotHalfCycles);}
        long numerator=((halves+1)/2)*33868800-videoRemainder,frequency=pal?53203425:53693175;
        return (int)Math.max(1,(numerator+frequency-1)/frequency);
    }
    public void tickVideo(Bus bus,int cpuCycles){
        if((gpuStatus&0x01000000)!=0)bus.raiseInterrupt(1);
        boolean pal=(gpuStatus&0x100000)!=0;
        videoRemainder+=(long)cpuCycles*(pal?53203425:53693175);
        int remaining=(int)(videoRemainder/33868800)*2;videoRemainder%=33868800;
        if(!videoInitialized){videoInitialized=true;hblank=halfCycle<horizontalStart*2 || halfCycle>=horizontalEnd*2;vblank=line<verticalStart || line>=verticalEnd;bus.timers.hblank(hblank);bus.timers.vblank(vblank);}
        int period=pal?6810:6825,totalLines=pal?314:263;
        int divider=(gpuStatus&0x10000)!=0?7:switch((gpuStatus>>>17)&3){case 0->10;case 1->8;case 2->5;default->4;};
        while(remaining>0){
            int boundary=period;if(halfCycle<horizontalStart*2)boundary=Math.min(boundary,horizontalStart*2);if(halfCycle<horizontalEnd*2)boundary=Math.min(boundary,horizontalEnd*2);
            int amount=Math.min(remaining,boundary-halfCycle);
            if(amount<=0){halfCycle=0;line=(line+1)%totalLines;continue;}
            halfCycle+=amount;remaining-=amount;dotHalfCycles+=amount;
            int dots=dotHalfCycles/(divider*2);dotHalfCycles%=divider*2;if(dots>0)bus.timers.dotClocks(dots);
            if(halfCycle>=period){halfCycle=0;line++;if(line>=totalLines){line=0;fields++;field=!field;}}
            boolean nextHblank=horizontalStart>=horizontalEnd || halfCycle<horizontalStart*2 || halfCycle>=horizontalEnd*2;
            boolean nextVblank=verticalStart>=verticalEnd || line<verticalStart || line>=verticalEnd;
            if(nextHblank!=hblank){hblank=nextHblank;bus.timers.hblank(hblank);}
            if(nextVblank!=vblank){vblank=nextVblank;bus.timers.vblank(vblank);if(vblank){vblanks++;bus.raiseInterrupt(0);}}
        }
        boolean interlace=(gpuStatus&0x400000)!=0;
        gpuStatus=(gpuStatus&0x7fffdfff)|((interlace?field:(line&1)!=0)?0x80000000:0)|(interlace?(field?0x2000:0):0x2000);
    }
    private void store(int address,int value,int mask){if((mask&2)==0 || (vram[address]&0x8000)==0)vram[address]=value|((mask&1)<<15);}
    public String diagnostic(){sync();return "GPU: "+raster.primitives+" primitives, "+raster.pixels+" pixels; display "+displayX+","+displayY+"; status "+Integer.toHexString(gpuStatus);}
    public record Frame(int width,int height,int[] pixels){}
    /** Selected display buffer, including packed 24-bit video frames. */
    public Frame displayFrame(){
        if(internalScale>1){flushUpload();int[] state={gpuStatus,displayX,displayY,horizontalStart,horizontalEnd,verticalStart,verticalEnd,0};FutureTask<Frame> task=new FutureTask<>(()->hardware.capture(state));submit(task::run);try{return task.get();}catch(InterruptedException e){Thread.currentThread().interrupt();throw new IllegalStateException(e);}catch(ExecutionException e){throw new IllegalStateException(e.getCause());}}
        sync(true);return captureFrame(gpuStatus,displayX,displayY,horizontalStart,horizontalEnd,verticalStart,verticalEnd);
    }
    /** Ordered scanout without making the CPU wait for rasterization or the pixel copy. */
    public void requestDisplayFrame(java.util.function.Consumer<Frame> receiver){
        flushUpload();
        int status=gpuStatus,x=displayX,y=displayY,hs=horizontalStart,he=horizontalEnd,vs=verticalStart,ve=verticalEnd;
        submit(()->{long before=diagnosticsEnabled?System.nanoTime():0;Frame frame;if(internalScale>1)frame=hardware.capture(new int[]{status,x,y,hs,he,vs,ve,0});else{readHardware();frame=captureFrame(status,x,y,hs,he,vs,ve);}if(diagnosticsEnabled)diagnosticScanout+=System.nanoTime()-before;receiver.accept(frame);});
    }
    /** GPU scanout into a persistent texture and an Android window surface; null means no host pixel copy. */
    public void requestSurfaceFrame(android.view.Surface surface,boolean filter,java.util.function.Consumer<Frame> receiver){requestSurfaceFrame(surface,filter,receiver,null);}
    public void restorePresentation(android.view.Surface surface,boolean filter,java.util.function.Consumer<Frame> receiver){requestSurfaceFrame(surface,filter,receiver,null,false);}
    public void requestSurfaceFrame(android.view.Surface surface,boolean filter,java.util.function.Consumer<Frame> receiver,EngineLog.Frame timing){requestSurfaceFrame(surface,filter,receiver,timing,true);}
    private void requestSurfaceFrame(android.view.Surface surface,boolean filter,java.util.function.Consumer<Frame> receiver,EngineLog.Frame timing,boolean extras){
        flushUpload();int[] state={gpuStatus,displayX,displayY,horizontalStart,horizontalEnd,verticalStart,verticalEnd,filter?1:0};
        submit(()->{
            long perfStart=performanceEnabled?System.nanoTime():0;
            java.util.function.Consumer<Frame> completed=perfStart==0?receiver:frame->{measurePresentation(perfStart,state);receiver.accept(frame);};
            reconcileShaderPresentation();
            boolean profiling=journal!=null&&journal.enabled();
            if(hardware!=null)hardware.configureProfiling(profiling);if(vulkan!=null)vulkan.configureProfiling(profiling);
            if(profiling){long now=System.nanoTime();if(now>=nextNativeSnapshot){
                lastNativeTiming=java.util.Map.of("hardware",hardware==null?java.util.Map.of("available",false):hardware.nativeTimings(),"presentation",vulkan==null?java.util.Map.of("available",false):vulkan.nativeTimings());
                journal.event(EngineLog.INFO,"gpu.native.snapshot",lastNativeTiming);nextNativeSnapshot=now+500000000L;
            }}
            if(timing!=null)timing.gpuBegin(renderer,state);
            int decision=presentationDecision(state,extras);
            ShaderChain.phase(decision==PresentationPolicy.SHADER?highRefresh.multiple:1,1,presentationRate,shaderAspect);
            if(timing!=null)timing.presentationSequence(decision==PresentationPolicy.BFI?"bfi":decision==PresentationPolicy.SHADER?"shader_subframes":"normal",decision==PresentationPolicy.BFI||decision==PresentationPolicy.SHADER?highRefresh.multiple:1);
            try{
                if(internalScale>1||(hardwareDrawing&&hardware!=null&&vulkan!=null)){
                    if(surface!=null&&surface.isValid())try(EngineLog.Span span=timing==null?null:timing.stage(vulkan!=null?"gpu.present.vulkan.shared":"gpu.present.gles.hd")){
                        boolean accepted;
                        try{accepted=vulkan!=null?hardware.presentResolved(surface,state,vulkan.nativeHandle()):hardware.present(surface,state);}
                        catch(RuntimeException error){if(baseVulkanOutput||vulkan==null)throw error;shaderPresentationFailed(error);accepted=hardware.present(surface,state);}
                        if(timing!=null)timing.presentation(accepted);
                    }
                    presentExtra(surface,decision,timing);completed.accept(null);return;
                }
                if(vulkan!=null){
                    try(EngineLog.Span span=timing==null?null:timing.stage("gpu.scanout.convert")){
                        if(vulkanFrames.matches(softwareVramRevision,state))vulkanFrames.reused();
                        else{Frame previous=vulkanFrames.frame();vulkanFrames.update(softwareVramRevision,state,captureFrame(state[0],state[1],state[2],state[3],state[4],state[5],state[6],previous==null?null:previous.pixels()));}
                    }
                    Frame frame=vulkanFrames.frame();try{if(surface!=null&&surface.isValid())try(EngineLog.Span span=timing==null?null:timing.stage("gpu.present.vulkan")){boolean accepted=vulkan.present(surface,frame,filter,vulkanFrames.token());if(timing!=null)timing.presentation(accepted);}presentExtra(surface,decision,timing);completed.accept(null);}catch(RuntimeException e){if(journal!=null)journal.error("gpu.present.vulkan",e);vulkan.close();vulkan=null;renderer="Software (Vulkan unavailable)";completed.accept(frame);}
                }else if(hardware!=null){if(surface!=null&&surface.isValid()){
                    if(!hardwareDrawing)try(EngineLog.Span span=timing==null?null:timing.stage("gpu.upload.gles")){hardware.setDisplayVram(vram,state,softwareVramRevision);}
                    try(EngineLog.Span span=timing==null?null:timing.stage("gpu.present.gles")){boolean accepted=hardware.present(surface,state);if(timing!=null)timing.presentation(accepted);}
                }presentExtra(surface,decision,timing);completed.accept(null);}else try(EngineLog.Span span=timing==null?null:timing.stage("gpu.scanout.software")){completed.accept(captureFrame(state[0],state[1],state[2],state[3],state[4],state[5],state[6]));}
            }finally{if(timing!=null)timing.complete();}
        });
    }
    private void readHardware(){if(hardware!=null && hardwareDrawing && hardwareDirty){hardware.readVram(vram);hardwareDirty=false;}}
    private Frame captureFrame(int gpuStatus,int displayX,int displayY,int horizontalStart,int horizontalEnd,int verticalStart,int verticalEnd){
        return captureFrame(gpuStatus,displayX,displayY,horizontalStart,horizontalEnd,verticalStart,verticalEnd,null);
    }
    // Only the Vulkan GPU worker owns/reuses this storage. Captures still get independent arrays.
    private Frame captureFrame(int gpuStatus,int displayX,int displayY,int horizontalStart,int horizontalEnd,int verticalStart,int verticalEnd,int[] reusable){
        int divider=(gpuStatus&0x10000)!=0?7:switch((gpuStatus>>>17)&3){case 0->10;case 1->8;case 2->5;default->4;};
        int width=Math.max(1,Math.min(1024,(horizontalEnd-horizontalStart)/divider));
        int height=Math.max(1,Math.min(512,(verticalEnd-verticalStart)*((gpuStatus&0x480000)==0x480000?2:1)));
        int[] out=reusable!=null&&reusable.length==width*height?reusable:new int[width*height];
        if((gpuStatus&0x800000)!=0){Arrays.fill(out,0xff000000);return new Frame(width,height,out);}
        boolean rgb24=(gpuStatus&0x200000)!=0;
        if(!rgb24){
            for(int y=0;y<height;y++){
                int row=((displayY+y)&511)*1024,destination=y*width;
                for(int x=0;x<width;x++)out[destination+x]=DISPLAY_COLORS[vram[row+((displayX+x)&1023)]&65535];
            }
        }else{
            for(int y=0;y<height;y++)for(int x=0;x<width;x++){
                int row=((displayY+y)&511)*1024,byteX=displayX*2+x*3;
                int r=vram[row+((byteX>>>1)&1023)]>>>((byteX&1)*8)&255;byteX++;
                int g=vram[row+((byteX>>>1)&1023)]>>>((byteX&1)*8)&255;byteX++;
                int b=vram[row+((byteX>>>1)&1023)]>>>((byteX&1)*8)&255;
                out[y*width+x]=0xff000000|(r<<16)|(g<<8)|b;
            }
        }
        return new Frame(width,height,out);
    }
    private static int argb(int p){int r=p&31,g=(p>>>5)&31,b=(p>>>10)&31;return 0xff000000|((r<<3|r>>>2)<<16)|((g<<3|g>>>2)<<8)|(b<<3|b>>>2);}
    private void fill(int c, int xy, int wh) {
        if(hardware!=null && hardwareDrawing){hardware.fill(c,xy,wh);hardwareDirty=true;return;}
        int x = xy & 0x3f0, y = (xy >>> 16) & 511;
        int w = ((wh & 1023) + 15) & ~15, h = (wh >>> 16) & 511;
        int r = (c & 255) >>> 3, g = ((c >>> 8) & 255) >>> 3, b = ((c >>> 16) & 255) >>> 3;
        int pixel=r|(g<<5)|(b<<10);
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) vram[((y+j)&511)*1024+((x+i)&1023)] = pixel;
        softwareVramRevision++;
    }
    public void sync(){sync(false);}
    void prepareSnapshot(){sync(true);}
    void restoreSnapshot(){readSynchronized=false;submitDirect(()->{softwareVramRevision++;if(hardware!=null && hardwareDrawing){hardware.setVram(vram);hardwareDirty=false;}});sync();}
    private void sync(boolean readback) {
        try(EngineLog.Span span=journal==null?null:journal.watch(readback?"gpu.sync.readback":"gpu.sync.commands",EngineLog.DEBUG,8_000_000L)){
        flushUpload();flushDrawBatch();
        if(readback)submitDirect(this::readHardware);
        if (worker != null) {
            FutureTask<Void> fence = new FutureTask<>(() -> null); worker.execute(fence);
            try { fence.get(); }
            catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new IllegalStateException(e); }
            catch (ExecutionException e) { throw new IllegalStateException(e); }
        }
        check();
        }
    }
    public int[] snapshot() {
        sync(true);int[] out=new int[vram.length];
        for(int i=0;i<out.length;i++){int p=vram[i],r=p&31,g=(p>>>5)&31,b=(p>>>10)&31;out[i]=0xff000000|((r<<3|r>>>2)<<16)|((g<<3|g>>>2)<<8)|(b<<3|b>>>2);}
        return out;
    }
    @Override public void close() {try{sync();}finally{
        Runnable cleanup=()->{if(vulkan!=null){vulkan.close();vulkan=null;}if(hardware!=null){hardware.close();hardware=null;}};
        if(worker!=null){FutureTask<Void> close=new FutureTask<>(cleanup,null);try{worker.execute(close);close.get();}catch(InterruptedException e){Thread.currentThread().interrupt();}catch(ExecutionException e){throw new IllegalStateException(e);}finally{worker.shutdown();}}
        else cleanup.run();
    }}
}
