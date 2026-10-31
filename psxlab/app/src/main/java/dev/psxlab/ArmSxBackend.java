package dev.psxlab;

import android.content.Context;
import android.graphics.*;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.view.Surface;
import dev.psxlab.core.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.Consumer;

/** ArmSX1 host contract. All guest mutations execute at a CPU frame boundary. */
public final class ArmSxBackend {
    private final Context context;
    private final EngineLog journal;
    private boolean runtimeLogging,captureLogging;private int runtimeLogLevel=2;
    private volatile float displayRefreshHz;private volatile int surfaceWidth,surfaceHeight;
    private final Consumer<Boolean> pauseCallback;
    private final Consumer<String> notice;
    private final Object gate=new Object();
    private final ConcurrentLinkedQueue<Runnable> commands=new ConcurrentLinkedQueue<>();
    private final PadState[] pads={new PadState(),new PadState()};
    private final StartupTiming startupTiming=new StartupTiming(2048);
    private volatile boolean traceStartup;
    private volatile boolean active,stop,paused,audioSuspended,presentationSuspended,fast;
    private volatile Surface surface;
    private volatile AudioOutput audio;
    private final PerformanceStatistics performance=new PerformanceStatistics();
    private volatile double hostCpuPercent=-1,hostRssMb=-1,hostAvailableMb=-1;
    private long hostSampleTime,hostSampleCpu;
    public void statisticsEnabled(boolean enabled){performance.enabled(enabled);}
    public int statistics(double[] out){return active&&!blocked()?performance.copy(out,hostCpuPercent,Runtime.getRuntime().availableProcessors(),hostRssMb,hostAvailableMb):0;}
    private void sampleHostUsage(long now){
        long elapsed=android.os.SystemClock.elapsedRealtime(),process=android.os.Process.getElapsedCpuTime();
        if(hostSampleTime>0&&elapsed>hostSampleTime)hostCpuPercent=Math.max(0,Math.min(100,100.0*(process-hostSampleCpu)/(elapsed-hostSampleTime)/Runtime.getRuntime().availableProcessors()));
        hostSampleTime=elapsed;hostSampleCpu=process;
        try(java.io.BufferedReader reader=new java.io.BufferedReader(new FileReader("/proc/self/status"))){String line;while((line=reader.readLine())!=null)if(line.startsWith("VmRSS:")){hostRssMb=Long.parseLong(line.substring(6).trim().split("\\s+")[0])/1024.0;break;}}catch(Exception ignored){hostRssMb=-1;}
        android.app.ActivityManager manager=(android.app.ActivityManager)context.getSystemService(Context.ACTIVITY_SERVICE);
        if(manager!=null){android.app.ActivityManager.MemoryInfo memory=new android.app.ActivityManager.MemoryInfo();manager.getMemoryInfo(memory);hostAvailableMb=memory.availMem/(1024.0*1024);}
    }
    private volatile float fps;
    private volatile float nominalFps=60;
    private volatile long frames,nonzero,guestClocks;
    private volatile int lastPc;
    private volatile String lastAudio="Not started";
    private volatile String lastCore="Not started";
    private volatile Map<String,Object> lastCpuTiming=Map.of(),lastGpuTiming=Map.of();
    private String biosPin;
    private volatile String renderer="Stopped",lastError="",gamePath="",discPath="";
    private File root,biosFolder,config;
    private byte[] bios;
    private Cpu cpu;
    private Bus bus;
    private Gpu gpu;
    private volatile Gpu displayGpu;
    private Media media,pendingMedia;
    private long insertAt;
    private Restore restart;
    private final ArrayDeque<String> events=new ArrayDeque<>();
    private volatile Map<String,String> options=Map.of();
    private volatile int aspectOverride=-1,volumeOverride=-1,presentationModeOverride=-1,presentationMultipleOverride=2;
    private volatile float customAspectOverride,fastSpeedOverride=-1,outputAspect=4f/3f;
    private volatile Boolean stretchOverride,integerOverride,mutedOverride,wideOverride;
    private volatile boolean outputStretch,outputInteger;
    private static float decimal(String value,float fallback){try{float result=Float.parseFloat(value);return Float.isFinite(result)?result:fallback;}catch(Exception e){return fallback;}}
    public void displayAspect(int mode){aspectOverride=mode;updateDisplay();}
    public void displayAspectCustom(float value){customAspectOverride=value;updateDisplay();}
    public void stretchDisplay(boolean enabled){stretchOverride=enabled;updateDisplay();}
    public void integerDisplay(boolean enabled){integerOverride=enabled;updateDisplay();}
    private void updateDisplay(){
        String configured=options.getOrDefault("video.display_aspect","classic");int mode=aspectOverride>=0?aspectOverride:switch(configured){case "square"->1;case "wide16x9","16:9"->2;case "custom"->3;default->0;};
        float custom=customAspectOverride>0?customAspectOverride:decimal(options.get("video.display_aspect_custom"),4f/3f);
        outputAspect=switch(mode){case 1->1;case 2->16f/9f;case 3->Math.max(0.1f,Math.min(10,custom));default->4f/3f;};
        outputStretch=stretchOverride!=null?stretchOverride:Boolean.parseBoolean(options.getOrDefault("video.stretch_mode","false"));
        outputInteger=integerOverride!=null?integerOverride:Boolean.parseBoolean(options.getOrDefault("video.integer_scaling","false"));
        Gpu target=gpu;if(target!=null)target.configureDisplay(outputAspect,outputStretch,outputInteger);
    }
    public void widescreenHack(boolean enabled){wideOverride=enabled;commands.add(()->{if(cpu!=null)cpu.gte.setWidescreen(enabled);});wake();}
    private boolean widescreenEnabled(){return wideOverride!=null?wideOverride:Boolean.parseBoolean(options.getOrDefault("video.widescreen_hack","false"));}
    public void framePresentation(int mode,int multiple){presentationModeOverride=mode;presentationMultipleOverride=Math.max(2,Math.min(4,multiple));updatePresentation();}
    public String framePresentationStatus(){Gpu target=gpu;return target==null?"Inactive: no game is running":target.presentationStatus();}
    private void updatePresentation(){
        int mode=presentationModeOverride>=0?presentationModeOverride:switch(options.getOrDefault("video.frame_presentation","off")){case "bfi"->1;case "shader_subframes"->2;default->0;};
        int multiple=presentationModeOverride>=0?presentationMultipleOverride:(int)decimal(options.get("video.presentation_multiplier"),2);
        Gpu target=gpu;if(target!=null)target.configurePresentation(mode,multiple,displayRefreshHz,!fast&&!blocked());
    }
    public void shadersChanged(){if(active&&blocked()){commands.add(()->{if(gpu!=null)gpu.prepareShaders();});wake();}}
    public void audioVolume(int volume){volumeOverride=volume;updateAudio();}
    public void audioMuted(boolean muted){mutedOverride=muted;updateAudio();}
    public void fastForwardSpeed(float speed){fastSpeedOverride=Float.isFinite(speed)?speed:2;updateAudio();}
    private boolean muteFast(){return fast&&Boolean.parseBoolean(options.getOrDefault("audio.mute_fast_forward","false"));}
    private float fastTempo(){float configured=fastSpeedOverride>=0?fastSpeedOverride:decimal(options.get("runtime.fast_forward_speed"),2);return configured<=0?16:Math.max(1,Math.min(16,configured));}
    private void updateAudio(){
        AudioOutput output=audio;if(output==null)return;output.setTempoFactor(fast&&!muteFast()?fastTempo():1);
        boolean muted=mutedOverride!=null?mutedOverride:Boolean.parseBoolean(options.getOrDefault("audio.muted","false"));
        float normal=volumeOverride>=0?volumeOverride:decimal(options.get("audio.volume"),100);
        float volume=fast?decimal(options.get("audio.fast_forward_volume"),100):normal;
        output.setGain(muted||muteFast()?0:volume/100f);
    }
    private record Restore(byte[] state,String disc,CompletableFuture<Boolean> ready) {}
    private static final class Media implements AutoCloseable {
        final ByteSource source; final CueDisc cue; final IsoDisc disc; final byte[] executable;
        Media(ByteSource source)throws IOException{
            this.source=source;cue=null;
            if(new String(source.read(0,8),StandardCharsets.US_ASCII).equals("PS-X EXE")){
                disc=null;byte[] header=source.read(0,2048);int size=ExeLoader.le(header,28);
                if(size<4||size>2*1024*1024)throw new IOException("Invalid executable size");executable=source.read(0,size+2048);
            }else{disc=source instanceof PbpDisc?new IsoDisc((SectorSource)source):new IsoDisc(source);executable=disc.bootExecutable();}
        }
        Media(CueDisc cue)throws IOException{this.source=null;this.cue=cue;disc=new IsoDisc((SectorSource)cue);executable=disc.bootExecutable();}
        public void close()throws IOException{if(cue!=null)cue.close();else source.close();}
    }
    public ArmSxBackend(Context context,Consumer<Boolean> pauseCallback,Consumer<String> notice){
        this(context,pauseCallback,notice,new File(context.getFilesDir(),"logs/psxlab"));
    }
    public ArmSxBackend(Context context,Consumer<Boolean> pauseCallback,Consumer<String> notice,File logDirectory){
        this.context=context.getApplicationContext();this.pauseCallback=pauseCallback;this.notice=notice;
        journal=new EngineLog(logDirectory,new EngineLog.Clock(){public long wall(){return System.nanoTime();}public long cpu(){return android.os.Debug.threadCpuTimeNanos();}public long tid(){return android.os.Process.myTid();}});
    }
    public synchronized void configureLogging(boolean enabled,int level){runtimeLogging=enabled;runtimeLogLevel=level;applyLogger();}
    public synchronized void captureDiagnostics(boolean enabled){captureLogging=enabled;applyLogger();}
    private void applyLogger(){journal.configure(runtimeLogging||captureLogging,captureLogging?Math.min(runtimeLogLevel,EngineLog.INFO):runtimeLogLevel);}
    public void displayMetadata(int width,int height,float hz){if(width>0){surfaceWidth=width;surfaceHeight=height;}if(hz>0)displayRefreshHz=hz;updatePresentation();journal.event(EngineLog.INFO,"presentation.display",Map.of("width",surfaceWidth,"height",surfaceHeight,"refresh_hz",displayRefreshHz));}
    public boolean flushDiagnosticLog(long timeout){return journal.flush(timeout);}
    public String diagnosticDirectory(){return journal.directory().getAbsolutePath();}
    private void configureLogging(Map<String,String> settings){
        boolean enabled=Boolean.parseBoolean(settings.getOrDefault("runtime.logging_enabled",Boolean.toString(!Boolean.parseBoolean(settings.getOrDefault("runtime.quiet","true")))));
        int level;try{level=Integer.parseInt(settings.getOrDefault("runtime.log_level","2"));}catch(NumberFormatException e){level=2;}configureLogging(enabled,level);
    }
    public synchronized boolean initialize(String dataRoot,String biosRoot,String configuration){
        if(active)return false;
        try{
            File hostRoot=new File(dataRoot);root=new File(hostRoot,"psxlab");biosFolder=new File(biosRoot);config=new File(configuration);
            // Archive both backends before the first migration session; no source saves are moved.
            File marker=new File(root,"migration-backup.txt");
            if(!marker.isFile()){
                File backup=MigrationBackup.create(hostRoot,new File(context.getExternalFilesDir(null),"migration-backups"),"swanstation","memcards","memorycards","cards","sstates","states","psxlab/cards","psxlab/states");
                if(!root.isDirectory()&&!root.mkdirs())throw new IOException("Cannot create backend directory");
                atomic(marker,backup.getAbsolutePath().getBytes(StandardCharsets.UTF_8));
            }
            // Onboarding initializes the host before the user has selected a BIOS.
            // Resolve and validate the selected BIOS at launch, after per-game settings.
            lastError="";configureLogging(readConfig());return true;
        }catch(Exception e){error(e);return false;}
    }
    private byte[] readBios()throws IOException{
        File[] files=biosFolder.isFile()?new File[]{biosFolder}:biosFolder.listFiles(f->f.isFile()&&f.length()==512*1024);
        if(files==null||files.length==0)throw new IOException("A 512 KiB PlayStation BIOS is required");
        Arrays.sort(files,Comparator.comparing(File::getName));
        String selected=readConfig().get("bios.override_file");if(selected!=null&&selected.isBlank())selected=null;if(selected==null)selected=biosPin;
        File chosen=selected==null?files[0]:new File(selected);
        if(!chosen.isFile())chosen=new File(biosFolder,selected==null?files[0].getName():selected);
        byte[] bytes=Files.readAllBytes(chosen.toPath());if(bytes.length!=512*1024)throw new IOException("Invalid PlayStation BIOS size");return bytes;
    }
    private Map<String,String> readConfig()throws IOException{
        Map<String,String> values=new HashMap<>();String section="";
        if(!config.isFile())return values;
        for(String row:Files.readAllLines(config.toPath(),StandardCharsets.UTF_8)){
            String line=row.trim();if(line.startsWith("#")||line.isEmpty())continue;
            if(line.startsWith("[")&&line.endsWith("]")){section=line.substring(1,line.length()-1);continue;}
            int split=line.indexOf('=');if(split<0)continue;String value=line.substring(split+1).trim();
            if(value.startsWith("\"")&&value.endsWith("\""))value=value.substring(1,value.length()-1);
            values.put(section+"."+line.substring(0,split).trim(),value);
        }return values;
    }
    private ParcelFileDescriptor descriptor(String path)throws IOException{
        Uri uri=Uri.parse(path);ParcelFileDescriptor fd=uri.getScheme()!=null&&uri.getScheme().equals("content")?context.getContentResolver().openFileDescriptor(uri,"r"):ParcelFileDescriptor.open(new File(uri.getScheme()!=null&&uri.getScheme().equals("file")?uri.getPath():path),ParcelFileDescriptor.MODE_READ_ONLY);
        if(fd==null)throw new IOException("Cannot open game image");return fd;
    }
    private ByteSource source(String path)throws IOException{
        try(ParcelFileDescriptor fd=descriptor(path)){
            // Recognize CHD by its header, including content URIs with no extension.
            try(FileSource probe=new FileSource(new ParcelFileDescriptor.AutoCloseInputStream(ParcelFileDescriptor.dup(fd.getFileDescriptor())),journal)){
                byte[] magic=probe.read(0,8);
                if(new String(magic,StandardCharsets.US_ASCII).equals("MComprHD"))return new ChdSource(fd,journal);
                if(PbpDisc.hasMagic(magic))return new PbpDisc(new FileSource(new ParcelFileDescriptor.AutoCloseInputStream(ParcelFileDescriptor.dup(fd.getFileDescriptor())),journal));
            }
            return new FileSource(new ParcelFileDescriptor.AutoCloseInputStream(ParcelFileDescriptor.dup(fd.getFileDescriptor())),journal);
        }
    }
    private Media openMedia(String path)throws IOException{
        Uri uri=Uri.parse(path);String filePath=uri.getScheme()==null?path:uri.getPath();
        if(CueDisc.isCueName(filePath)){
            if(uri.getScheme()!=null&&!uri.getScheme().equals("file"))throw new IOException("Launch the CUE through the game library so its BIN files can be prepared together");
            File cueFile=new File(filePath).getCanonicalFile(),parent=cueFile.getParentFile();
            if(cueFile.length()>1024*1024)throw new IOException("CUE text exceeds 1 MiB");
            CueDisc cue=new CueDisc(new String(Files.readAllBytes(cueFile.toPath()),StandardCharsets.UTF_8),name->{
                File track=new File(parent,CueDisc.relativeName(name)).getCanonicalFile();
                if(!track.toPath().startsWith(parent.toPath()))throw new IOException("CUE track is outside its folder");
                return new FileSource(new FileInputStream(track),journal);
            });
            try{Media result=new Media(cue);journal.event(EngineLog.INFO,"cd.cue.mounted",Map.of("tracks",cue.lastTrack(),"sectors",cue.sectorCount()));return result;}
            catch(Exception e){try{cue.close();}catch(IOException closing){e.addSuppressed(closing);}throw e;}
        }
        ByteSource source=source(path);try{Media result=new Media(source);if(source instanceof PbpDisc pbp)journal.event(EngineLog.INFO,"cd.pbp.mounted",Map.of("tracks",pbp.lastTrack(),"sectors",pbp.sectorCount(),"discs",pbp.discCount()));return result;}catch(Exception e){source.close();throw e;}
    }
    /** Blocking, as ArmSX1's existing runVMThread contract requires. */
    public boolean run(String path){return run(path, () -> true);}
    /** The host's session cancellation and this active/stop transition share this monitor. */
    public boolean run(String path,java.util.function.BooleanSupplier allowed){
        synchronized(this){if(!allowed.getAsBoolean()||active||root==null)return false;active=true;stop=false;paused=false;frames=nonzero=guestClocks=0;gamePath=discPath=path;lastError="";lastPc=0;lastAudio=lastCore="Not started";lastCpuTiming=lastGpuTiming=Map.of();fps=0;}
        boolean booted=false;Restore restore=null;startupTiming.reset();performance.reset();hostSampleTime=hostSampleCpu=0;hostCpuPercent=hostRssMb=hostAvailableMb=-1;event("launch");
        try{
            options=readConfig();configureLogging(options);journal.startSession(MigrationBackup.digest(path.getBytes(StandardCharsets.UTF_8)));bios=readBios();
            do{
                restart=null;performance.reset();
                int scale=Math.max(1,Math.min(5,Integer.parseInt(options.getOrDefault("video.internal_scale","1"))));
                RendererSelection selected=RendererSelection.from(options);int mode=selected.presentation();
                // Keep one graphics API attached to a retained Android Surface. Software
                // still rasterizes on the CPU; its finished pixels use the texture presenter.
                try(Gpu graphics=mode==0&&scale==1&&!selected.hardware()?new Gpu(true,false):new Gpu(true,mode,scale,selected.hardware())){
                    gpu=graphics;displayGpu=graphics;gpu.setJournal(journal);ShaderChain.backend(mode);updateDisplay();updatePresentation();clearSurface();gpu.prepareShaders();bus=new Bus(gpu,pads[0]);bus.dma.setJournal(journal);bus.sio0.setPad(1,pads[1]);bus.loadBios(bios);
                    try(CardStorage cards=new CardStorage(new File(root,"cards"),bus)){
                        String mounted=restore==null?discPath:restore.disc();media=mounted.isEmpty()?null:openMedia(mounted);if(media!=null&&media.disc!=null)bus.cdrom.insert(media.disc);
                        try{
                            cpu=new Cpu(bus);cpu.reset();
                            if(restore!=null&&restore.state()!=null)SaveState.restore(restore.state(),cpu,bus,gpu,gamePath);
                            else if(media!=null&&(media.disc==null||Boolean.parseBoolean(options.getOrDefault("cpu.fast_boot","false")))){cpu=BiosBoot.initialize(bios,bus,()->stop);bus.cdrom.prepareBootHandoff();ExeLoader.loadInto(media.executable,bus,cpu);}
                            cpu.gte.setWidescreen(widescreenEnabled());cpu.enableNative(true,true,true);
                            try(AudioOutput output=new AudioOutput(true,Boolean.parseBoolean(options.getOrDefault("diagnostics.audio_capture","false")),context)){
                                audio=output;output.setJournal(journal);updateAudio();output.setPaused(blocked());bus.spu.setAudioSink((pcm,count)->{int audible=0;for(int i=0;i<count;i++)if(pcm[i]!=0)audible++;nonzero+=audible;output.samples(pcm,count);});
                                renderer="PSX Lab / "+gpu.renderer();booted=true;
                                if(restore!=null){discPath=restore.disc();restore.ready().complete(true);}pauseCallback.accept(paused);event("ready");
                                try{loop(output);}finally{try{lastAudio=output.diagnostic();}catch(IllegalStateException ignored){}}
                            }finally{audio=null;bus.spu.setAudioSink(null);cpu.closeNative();}
                        }finally{if(media!=null){media.close();media=null;}if(pendingMedia!=null){pendingMedia.close();pendingMedia=null;}}
                    }finally{clearSurface();}
                }finally{displayGpu=null;gpu=null;bus=null;cpu=null;}
                restore=restart;
            }while(!stop&&restore!=null);
        }catch(Exception e){booted=false;error(e);if(restore!=null)restore.ready().complete(false);if(restart!=null)restart.ready().complete(false);}
        finally{performance.reset();resetButtons();fps=0;renderer="Stopped";synchronized(this){active=false;}Runnable job;while((job=commands.poll())!=null)job.run();event("stopped");writeReport();gamePath=discPath="";}
        return booted;
    }
    private boolean blocked(){Surface target=surface;return paused||audioSuspended||presentationSuspended||target==null||!target.isValid();}
    private void loop(AudioOutput output)throws Exception{
        FrameRateMeter frameRate=new FrameRateMeter();AudioAwareFramePacer pacer=new AudioAwareFramePacer(cpu.instructions,System.nanoTime());long last=gpu.vblanks,measured=System.nanoTime();boolean wasBlocked=false,wasFast=false;EngineLog.Frame diagnosticFrame=null;long perfCpu=0,perfEpoch=0;
        while(!stop&&restart==null){
            Runnable job;while((job=commands.poll())!=null)job.run();if(stop||restart!=null)break;
            if(blocked()){
                if(!wasBlocked&&surface!=null&&surface.isValid()&&gpu.presentationNeedsRestore()){Surface target=surface;gpu.restorePresentation(target,true,frame->{if(frame!=null)draw(target,frame);});}
                output.setPaused(true);wasBlocked=true;fps=0;diagnosticFrame=null;journal.leaveFrame();synchronized(gate){gate.wait(10);}continue;
            }
            if(wasBlocked){performance.reset();perfCpu=perfEpoch=0;output.setPaused(muteFast());pacer=new AudioAwareFramePacer(cpu.instructions,System.nanoTime());measured=System.nanoTime();frameRate.reset();wasBlocked=false;}
            if(fast!=wasFast){bus.spu.flushAudio();bus.spu.setAudioSink(muteFast()?null:(pcm,count)->{int audible=0;for(int i=0;i<count;i++)if(pcm[i]!=0)audible++;nonzero+=audible;output.samples(pcm,count);});updateAudio();output.setPaused(muteFast());pacer=new AudioAwareFramePacer(cpu.instructions,System.nanoTime());wasFast=fast;frameRate.reset();}
            if(pendingMedia!=null&&cpu.instructions>=insertAt){bus.cdrom.insert(pendingMedia.disc);if(media!=null)media.close();media=pendingMedia;pendingMedia=null;}
            long ticket=performance.ticket();
            if(ticket!=perfEpoch){perfCpu=0;perfEpoch=ticket;}
            gpu.performanceEnabled=ticket!=0;bus.dma.performanceEnabled=ticket!=0;
            long perfStarted=ticket!=0?System.nanoTime():0;
            cpu.configureProfiling(journal.enabled());
            if(diagnosticFrame==null)diagnosticFrame=journal.frame(cpu.instructions,cpu.pc);
            try(EngineLog.Span span=journal.begin("cpu.runToFrame",EngineLog.TRACE)){cpu.runToFrame(100000);}guestClocks=cpu.instructions;lastPc=cpu.pc;if(perfStarted!=0)perfCpu+=System.nanoTime()-perfStarted;
            if(gpu.vblanks==last)continue;last=gpu.vblanks;if(diagnosticFrame!=null)diagnosticFrame.cpuDone(cpu.instructions,cpu.pc);
            long flushStarted=ticket!=0?System.nanoTime():0;
            try(EngineLog.Span span=journal.begin("cpu.flush_devices",EngineLog.DEBUG)){bus.flushClocks();bus.spu.flushAudio();}
            if(flushStarted!=0)perfCpu+=System.nanoTime()-flushStarted;
            long paceStarted=diagnosticFrame==null&&ticket==0?0:System.nanoTime(),buffered=output.bufferedFrames();
            long wait=fast?0:pacer.waitNanos(cpu.instructions,System.nanoTime(),buffered);if(wait>0)TimeUnit.NANOSECONDS.sleep(wait);
            if(diagnosticFrame!=null)diagnosticFrame.pacing(wait,System.nanoTime()-paceStarted,buffered);
            long measuredIdle=ticket==0?0:System.nanoTime()-paceStarted,measuredCpu=perfCpu;perfCpu=0;
            int voices=0;if(ticket!=0)for(int voice=0;voice<24;voice++)if((bus.spu.read(0x1f801c0c+voice*16)&0x7fff)!=0)voices++;
            final int activeVoices=voices;
            final long clocks=cpu.instructions,triangles=gpu.performanceTriangles,rectangles=gpu.performanceRectangles,lines=gpu.performanceLines,mdec=bus.mdec.decodedBlocks,sectors=bus.cdrom.sectorsRead;
            final long[] dma=ticket==0?null:bus.dma.performanceWords.clone();
            Gpu graphics=gpu;
            EngineLog.Frame submitted=diagnosticFrame;diagnosticFrame=null;if(submitted!=null)submitted.queued();
            Surface target=surface;
            if(target!=null&&target.isValid()){
                boolean trace=traceStartup;long requested=trace?System.nanoTime():0,clock=cpu.instructions;
                gpu.requestSurfaceFrame(target,true,frame->{long drawing=ticket==0?0:System.nanoTime();if(frame!=null)draw(target,frame);long completed=System.nanoTime();
                    if(ticket!=0){Gpu.Performance p=graphics.performance();performance.completed(ticket,completed,measuredCpu,measuredIdle,p.presentNanos()+(frame==null?0:completed-drawing),new PerformanceStatistics.Sample(clocks,triangles,rectangles,lines,mdec,sectors,dma,activeVoices,p.width(),p.height(),p.scale()));}
                    frames++;float rate=frameRate.completed(completed);if(!Float.isNaN(rate)&&!stop&&!paused&&!audioSuspended&&!presentationSuspended&&surface==target)fps=rate;if(trace)startupTiming.record(clock,requested,completed);},submitted);
            }
            journal.leaveFrame();
            long now=System.nanoTime();if(now-measured>=500000000L){if(performance.enabled()||journal.enabled())sampleHostUsage(now);nominalFps=videoRate(gpu.status());lastAudio=output.diagnostic();lastCore=cpu.executionDiagnostic()+"\n"+bus.cdrom.diagnostic()+"\n"+bus.dma.diagnostic()+"\n"+bus.spu.diagnostic();renderer="PSX Lab / "+gpu.renderer();if(journal.enabled()){lastCpuTiming=cpu.nativeTimings();lastGpuTiming=gpu.nativeTimings();journal.event(EngineLog.INFO,"engine.snapshot",Map.of("fps",fps,"audio",output.loggingSnapshot(),"audio_summary",lastAudio,"core",lastCore,"gpu",gpu.renderStatistics(),"native_cpu",lastCpuTiming,"native_gpu",lastGpuTiming,"memory",Map.of("process_rss_mb",hostRssMb,"system_available_mb",hostAvailableMb,"java_heap_used_bytes",Runtime.getRuntime().totalMemory()-Runtime.getRuntime().freeMemory(),"java_heap_limit_bytes",Runtime.getRuntime().maxMemory(),"native_heap_allocated_bytes",android.os.Debug.getNativeHeapAllocatedSize())));}measured=now;}
        }
    }
    private void clearSurface(){Surface target=surface;if(target==null||!target.isValid()||gpu==null)return;gpu.control(0x03000001);gpu.requestSurfaceFrame(target,false,frame->{if(frame!=null)draw(target,frame);});gpu.sync();}
    private void draw(Surface target,Gpu.Frame frame){
        if(surface!=target||!target.isValid())return;Bitmap bitmap=Bitmap.createBitmap(frame.pixels(),frame.width(),frame.height(),Bitmap.Config.ARGB_8888);
        try{Canvas canvas=target.lockCanvas(null);try{canvas.drawColor(Color.BLACK);float width=outputStretch?canvas.getWidth():Math.min(canvas.getWidth(),canvas.getHeight()*outputAspect),height=outputStretch?canvas.getHeight():width/outputAspect;if(outputInteger&&!outputStretch){int k=(int)Math.min(width/frame.width(),height/frame.height());if(k>=1){width=frame.width()*k;height=frame.height()*k;}}float x=(canvas.getWidth()-width)/2,y=(canvas.getHeight()-height)/2;canvas.drawBitmap(bitmap,null,new RectF(x,y,x+width,y+height),new Paint(Paint.FILTER_BITMAP_FLAG));}finally{target.unlockCanvasAndPost(canvas);}}catch(IllegalArgumentException|IllegalStateException ignored){}finally{bitmap.recycle();}
    }
    public void surface(Surface value){surface=value;updatePresentation();if(value==null)resetButtons();event(value==null?"surface-lost":"surface-attached");wake();}
    public void pause(boolean value){paused=value;updatePresentation();if(value)resetButtons();AudioOutput out=audio;if(out!=null)out.setPaused(blocked()||muteFast());pauseCallback.accept(value);event(value?"pause":"resume");wake();}
    public void suspendAudio(boolean value){audioSuspended=value;updatePresentation();event("audio-suspended="+value);wake();}
    public void suspendPresentation(boolean value){presentationSuspended=value;updatePresentation();event("presentation-suspended="+value);wake();}
    public void stop(){stop=true;resetButtons();AudioOutput out=audio;if(out!=null)out.close();wake();}
    public void fastForward(boolean value){fast=value;updateAudio();updatePresentation();wake();}
    private void wake(){synchronized(gate){gate.notifyAll();}}
    private <T> T call(Callable<T> action,T failed){
        if(!active)return failed;CompletableFuture<T> result=new CompletableFuture<>();commands.add(()->{if(result.isCancelled())return;try{result.complete(!stop&&bus!=null?action.call():failed);}catch(Exception e){error(e);result.complete(failed);}});wake();
        try{return result.get(15,TimeUnit.SECONDS);}catch(Exception e){result.cancel(false);error(e);return failed;}
    }
    public boolean save(int slot){return call(()->{if(pendingMedia!=null)throw new IOException("Wait for the disc tray to close before saving");File file=stateFile(slot);BackendStateStore.write(file,discPath,SaveState.capture(cpu,bus,gpu,gamePath));screenshotNow(file+".png");if(slot==-1)atomic(new File(root,"autosave-game.txt"),gamePath.getBytes(StandardCharsets.UTF_8));event("save-slot="+slot);return true;},false);}
    public boolean load(int slot,boolean acknowledgeCards){
        if(!acknowledgeCards){notice.accept("Loading restores machine state using the current memory cards. Back up your cards first.");return false;}
        CompletableFuture<Boolean> ready=new CompletableFuture<>();
        boolean accepted=call(()->{BackendStateStore.Stored stored=BackendStateStore.read(stateFile(slot));SaveState.validate(stored.payload(),gamePath,bios);try(Media checked=openMedia(stored.disc())){}restart=new Restore(stored.payload(),stored.disc(),ready);event("load-slot="+slot);return true;},false);
        if(!accepted)return false;try{return ready.get(15,TimeUnit.SECONDS);}catch(Exception e){error(e);return false;}
    }
    public void reset(){call(()->{restart=new Restore(null,discPath,new CompletableFuture<>());return true;},false);}
    /** Apply internal resolution and rasterizer changes to the running game. Both are fixed when a session's Gpu is
     *  created, so an in-game change used to wait for a restart. This snapshots the machine and goes round the same
     *  restart path a state load uses, with the settings read again; the state format skips those Gpu fields, so the
     *  restore keeps the new ones. The graphics API stays the one the session started with. Blocking, like load. */
    public boolean reloadVideo(){
        CompletableFuture<Boolean> ready=new CompletableFuture<>();
        boolean accepted=call(()->{if(pendingMedia!=null)throw new IOException("Wait for the disc tray to close before changing the resolution");
            byte[] state=SaveState.capture(cpu,bus,gpu,gamePath);Map<String,String> next=readConfig();String api=options.get("video.gpu_backend");
            if(api==null)next.remove("video.gpu_backend");else next.put("video.gpu_backend",api);options=next;
            restart=new Restore(state,discPath,ready);event("reload-video");return true;},false);
        if(!accepted)return false;try{return ready.get(15,TimeUnit.SECONDS);}catch(Exception e){error(e);return false;}
    }
    private String stateIdentity(int slot){if(!gamePath.isEmpty()||slot!=-1||root==null)return gamePath;File index=new File(root,"autosave-game.txt");try{return index.length()<=65536?new String(Files.readAllBytes(index.toPath()),StandardCharsets.UTF_8):"";}catch(IOException e){return "";}}
    public File stateFile(int slot){return BackendStateStore.file(root,stateIdentity(slot),slot);}
    public String stateGame(int slot){try{return stateFile(slot).isFile()?stateIdentity(slot):"";}catch(IllegalArgumentException e){return "";}}
    public byte[] preview(int slot){try{return Files.readAllBytes(new File(stateFile(slot)+".png").toPath());}catch(IOException|IllegalArgumentException e){return new byte[0];}}
    public void screenshot(String path){Gpu.Frame frame=call(()->gpu.displayFrame(),null);if(frame!=null)try{writeScreenshot(path,frame);}catch(IOException e){error(e);}}
    private void screenshotNow(String path)throws IOException{writeScreenshot(path,gpu.displayFrame());}
    // Only framebuffer capture needs a guest boundary. PNG encoding and fsync
    // belong to the requesting host thread, so diagnosis does not starve audio.
    private void writeScreenshot(String path,Gpu.Frame frame)throws IOException{Bitmap image=Bitmap.createBitmap(frame.pixels(),frame.width(),frame.height(),Bitmap.Config.ARGB_8888);try(ByteArrayOutputStream bytes=new ByteArrayOutputStream()){if(!image.compress(Bitmap.CompressFormat.PNG,100,bytes))throw new IOException("Screenshot failed");atomic(new File(path),bytes.toByteArray());}finally{image.recycle();}}
    public boolean changeDisc(String path){return call(()->{Media next=openMedia(path);if(next.disc==null){next.close();throw new IOException("Disc replacement requires a CD image");}if(pendingMedia!=null)pendingMedia.close();bus.cdrom.eject();pendingMedia=next;insertAt=cpu.instructions+33868800L;discPath=path;event("disc-change");return true;},false);}
    public void button(int player,int key,int range,boolean down){if(player<0||player>=pads.length)return;if((key>=110&&key<=113)||(key>=120&&key<=123)){int stick=key>=120?1:0,offset=key%10;PadState.State s=pads[player].get();int x=stick==0?s.leftX():s.rightX(),y=stick==0?s.leftY():s.rightY();int v=down?Math.min(127,Math.max(0,range)*127/32767):0;if(offset==0||offset==2)y=128+(offset==0?-v:v);else x=128+(offset==3?-v:v);analog(player,stick,x,y);return;}int bit=buttonBit(key);if(bit<0)return;PadState p=pads[player];synchronized(p){PadState.State s=p.get();int mask=1<<bit;p.update(down?s.pressed()|mask:s.pressed()&~mask,s.rightX(),s.rightY(),s.leftX(),s.leftY());}}
    public static int buttonBit(int key){return switch(key){case 109->0;case 106->1;case 107->2;case 108->3;case 19->4;case 22->5;case 20->6;case 21->7;case 104->8;case 105->9;case 102->10;case 103->11;case 100->12;case 97->13;case 96->14;case 99->15;default->-1;};}
    public void analog(int player,int stick,int x,int y){if(player<0||player>=pads.length||stick<0||stick>1)return;PadState p=pads[player];synchronized(p){PadState.State s=p.get();p.update(s.pressed(),stick==1?x:s.rightX(),stick==1?y:s.rightY(),stick==0?x:s.leftX(),stick==0?y:s.leftY());}}
    public void resetButtons(){for(PadState pad:pads)synchronized(pad){pad.clear();}}
    /** Explicit raw-card import. Never shares live card files with SwanStation. */
    public synchronized boolean importCard(int slot,File source){
        if(active||root==null||slot<0||slot>1)return false;
        try{if(source.length()!=MemoryCard.SIZE)throw new IOException("Only raw 128 KiB memory cards can be imported");byte[] bytes=Files.readAllBytes(source.toPath());if(bytes[0]!='M'||bytes[1]!='C')throw new IOException("Memory card header is invalid");MigrationBackup.create(root,new File(context.getExternalFilesDir(null),"migration-backups"),"cards");atomic(new File(root,"cards/card"+(slot+1)+".mcd"),bytes);event("import-card="+slot);return true;}catch(Exception e){error(e);return false;}
    }
    public synchronized String cardPath(int slot){return root==null||slot<0||slot>1?"":new File(root,"cards/card"+(slot+1)+".mcd").getAbsolutePath();}
    public PadState.State pad(int player){return pads[player].get();}
    public synchronized void selectBios(String name){if(!active&&name!=null&&!name.isEmpty()&&new File(name).getName().equals(name))biosPin=name;}
    public float nominalFps(){return nominalFps;}
    public void traceStartup(boolean enabled){traceStartup=enabled;}
    public String startupTiming(){return startupTiming.tsv();}
    public static float videoRate(int gpuStatus){return (gpuStatus&0x100000)!=0?50:60;}
    public boolean active(){return active;}public float fps(){return fps;}public long frames(){return frames;}public long audioNonzero(){return nonzero;}
    public long guestClocks(){return guestClocks;}public String renderer(){return renderer;}public String displayRenderer(){Gpu target=displayGpu;return target==null?"Stopped":target.displayRenderer();}public String lastError(){return lastError;}
    public String audioDiagnostic(){AudioOutput out=audio;try{return out==null?lastAudio:out.diagnostic();}catch(IllegalStateException ignored){return lastAudio;}}
    private synchronized void event(String value){if(events.size()==128)events.removeFirst();events.addLast(android.os.SystemClock.elapsedRealtime()+" "+value);android.util.Log.i("PSXLabBackend",value);journal.event(EngineLog.INFO,"runtime.event",Map.of("message",value));}
    /** No BIOS/RAM/game data or authentication settings are included in a report. */
    public synchronized String diagnosticReport(){
        org.json.JSONObject json=new org.json.JSONObject();try{json.put("schema",1);json.put("backend","PSX Lab ArmSX1 adapter 1");json.put("gameId",MigrationBackup.digest(gamePath.getBytes(StandardCharsets.UTF_8)));json.put("renderer",renderer);json.put("displayRenderer",displayRenderer());org.json.JSONObject settings=new org.json.JSONObject();for(String key:List.of("video.gpu_backend","video.renderer","video.internal_scale","cpu.fast_boot","cpu.execution_mode","console.region"))if(options.containsKey(key))settings.put(key,options.get(key));json.put("options",settings);json.put("biosSha256",bios==null?"":MigrationBackup.digest(bios));json.put("active",active);json.put("paused",blocked());json.put("frames",frames);json.put("guestClocks",guestClocks);json.put("pc",String.format(Locale.ROOT,"%08X",lastPc));json.put("fps",fps);json.put("nominalFps",nominalFps);json.put("audioNonzero",nonzero);json.put("audio",audioDiagnostic());json.put("core",lastCore);json.put("lastError",lastError);json.put("nativeTimings",new org.json.JSONObject(Map.of("cpu",lastCpuTiming,"gpu",lastGpuTiming)));json.put("memory",new org.json.JSONObject(Map.of("processRssMb",hostRssMb,"systemAvailableMb",hostAvailableMb,"javaHeapUsedBytes",Runtime.getRuntime().totalMemory()-Runtime.getRuntime().freeMemory(),"nativeHeapAllocatedBytes",android.os.Debug.getNativeHeapAllocatedSize())));json.put("events",new org.json.JSONArray(events));json.put("logging",new org.json.JSONObject(journal.summary()));json.put("highRefresh",framePresentationStatus());json.put("shader",ShaderChain.requested()?(gpu!=null?gpu.shaderStatus():ShaderChain.status()):"Off");json.put("display",new org.json.JSONObject(Map.of("width",surfaceWidth,"height",surfaceHeight,"refreshHz",displayRefreshHz,"aspect",outputAspect,"stretch",outputStretch,"integerScaling",outputInteger,"timingDomain","host/API; physical present timestamps unavailable")));return json.toString(2);}catch(org.json.JSONException e){throw new IllegalStateException(e);}
    }
    private void writeReport(){try{if(root!=null){byte[] report=diagnosticReport().getBytes(StandardCharsets.UTF_8);atomic(new File(root,"last-session.json"),report);atomic(new File(journal.directory(),"last-session.json"),report);}}catch(IOException e){android.util.Log.e("PSXLabBackend","Cannot write diagnostic report",e);}}
    private void error(Exception e){lastError=e.toString();journal.error(EngineLog.FATAL,"runtime.error",e);event("error: "+e.getClass().getSimpleName());android.util.Log.e("PSXLabBackend",lastError,e);writeReport();notice.accept(lastError);}
    private static void atomic(File target,byte[] bytes)throws IOException{BackendStateStore.atomic(target,bytes);}
}
