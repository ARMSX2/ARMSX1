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
    private boolean guestAudioTracing;
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
    private long hostThermalSample;
    private volatile int hostThermalStatus=-1;
    public void statisticsEnabled(boolean enabled){performance.enabled(enabled);}
    public int statistics(double[] out){return active&&!blocked()?performance.copy(out,hostCpuPercent,Runtime.getRuntime().availableProcessors(),hostRssMb,hostAvailableMb):0;}
    private void sampleHostUsage(long now){
        long elapsed=android.os.SystemClock.elapsedRealtime(),process=android.os.Process.getElapsedCpuTime();
        if(hostSampleTime>0&&elapsed>hostSampleTime)hostCpuPercent=Math.max(0,Math.min(100,100.0*(process-hostSampleCpu)/(elapsed-hostSampleTime)/Runtime.getRuntime().availableProcessors()));
        hostSampleTime=elapsed;hostSampleCpu=process;
        try(java.io.BufferedReader reader=new java.io.BufferedReader(new FileReader("/proc/self/status"))){String line;while((line=reader.readLine())!=null)if(line.startsWith("VmRSS:")){hostRssMb=Long.parseLong(line.substring(6).trim().split("\\s+")[0])/1024.0;break;}}catch(Exception ignored){hostRssMb=-1;}
        android.app.ActivityManager manager=(android.app.ActivityManager)context.getSystemService(Context.ACTIVITY_SERVICE);
        if(manager!=null){android.app.ActivityManager.MemoryInfo memory=new android.app.ActivityManager.MemoryInfo();manager.getMemoryInfo(memory);hostAvailableMb=memory.availMem/(1024.0*1024);}
        if(hostThermalSample==0||now-hostThermalSample>=5_000_000_000L){
            hostThermalSample=now;hostThermalStatus=-1;
            if(android.os.Build.VERSION.SDK_INT>=29)try{
                android.os.PowerManager power=(android.os.PowerManager)context.getSystemService(Context.POWER_SERVICE);
                if(power!=null)hostThermalStatus=power.getCurrentThermalStatus();
            }catch(RuntimeException ignored){}
            journal.event(EngineLog.INFO,"host.thermal",Map.of("status",hostThermalStatus,"available",hostThermalStatus>=0,"sample_period_ms",5000));
        }
    }
    private volatile float fps;
    private volatile float nominalFps=60;
    private volatile boolean adpf;
    private final SessionTimeline timeline=new SessionTimeline();
    private volatile long frames,nonzero,guestClocks;
    private volatile String lastGameId="";
    private volatile int lastPc;
    private volatile String lastAudio="Not started";
    private volatile String lastCore="Not started";
    private volatile String lastFailureCpuContext="";
    private volatile Map<String,Object> lastCpuTiming=Map.of(),lastGpuTiming=Map.of();
    private String biosPin;
    private volatile String renderer="Stopped",lastError="",gamePath="",discPath="";
    private String configuredGame="",bootPath="";
    private volatile String stateId="";
    private final Set<String> sessionAliases=new LinkedHashSet<>();
    private final Map<String,String> discSources=new ConcurrentHashMap<>();
    private final List<PreparedMedia> restoredMedia=new ArrayList<>();
    public record PreparedMedia(String path,AutoCloseable owner) implements AutoCloseable {
        public void close()throws Exception{if(owner!=null)owner.close();}
    }
    public interface MediaResolver { PreparedMedia prepare(String source)throws Exception; }
    private MediaResolver mediaResolver;
    public void mediaResolver(MediaResolver resolver){mediaResolver=resolver;}
    public synchronized void configureSession(String source){if(!active){configuredGame=BackendStateStore.normalize(source);discSources.clear();sessionAliases.clear();sessionAliases.add(configuredGame);}}
    public synchronized void sessionAlias(String source){if(!active&&source!=null&&!source.isBlank())sessionAliases.add(BackendStateStore.normalize(source));}
    public void discSource(String runtime,String source){discSources.put(runtime,source);}
    private final Map<String,byte[]> discSubchannels=new ConcurrentHashMap<>();
    public void discSubchannel(String runtime,byte[] sbi){discSubchannels.put(runtime,sbi.clone());}
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
    private volatile int aspectOverride=-1,volumeOverride=-1,presentationModeOverride=-1,presentationMultipleOverride=2,cropOverride=-1;
    private volatile float customAspectOverride,fastSpeedOverride=-1,outputAspect=4f/3f;
    private volatile Boolean stretchOverride,integerOverride,mutedOverride,wideOverride;
    private volatile boolean outputStretch,outputInteger;
    private static float decimal(String value,float fallback){try{float result=Float.parseFloat(value);return Float.isFinite(result)?result:fallback;}catch(Exception e){return fallback;}}
    public void displayAspect(int mode){aspectOverride=mode;updateDisplay();}
    public void displayAspectCustom(float value){customAspectOverride=value;updateDisplay();}
    public void stretchDisplay(boolean enabled){stretchOverride=enabled;updateDisplay();}
    public void integerDisplay(boolean enabled){integerOverride=enabled;updateDisplay();}
    public void overscanCrop(int mode){cropOverride=mode>=0&&mode<=2?mode:-1;updateDisplay();}
    private void updateDisplay(){
        String configured=options.getOrDefault("video.display_aspect","classic");int mode=aspectOverride>=0?aspectOverride:switch(configured){case "square"->1;case "wide16x9","16:9"->2;case "custom"->3;default->0;};
        float custom=customAspectOverride>0?customAspectOverride:decimal(options.get("video.display_aspect_custom"),4f/3f);
        outputAspect=switch(mode){case 1->1;case 2->16f/9f;case 3->Math.max(0.1f,Math.min(10,custom));default->4f/3f;};
        outputStretch=stretchOverride!=null?stretchOverride:Boolean.parseBoolean(options.getOrDefault("video.stretch_mode","false"));
        outputInteger=integerOverride!=null?integerOverride:Boolean.parseBoolean(options.getOrDefault("video.integer_scaling","false"));
        updateProjection();
        Gpu target=gpu;if(target!=null){target.configureDisplay(outputAspect,outputStretch,outputInteger);target.configureCrop(cropOverride>=0?cropOverride:switch(options.getOrDefault("video.overscan_crop","none")){case "small"->1;case "full"->2;default->0;});}
    }
    private volatile float projectionAspect=4f/3f;
    public void widescreenHack(boolean enabled){wideOverride=enabled;updateProjection();}
    private void updateProjection(){
        float aspect=outputStretch&&surfaceWidth>0&&surfaceHeight>0?(float)surfaceWidth/surfaceHeight:outputAspect;
        boolean enabled=widescreenEnabled();boolean changed=Gte.projectionScale(enabled,aspect)!=Gte.projectionScale(enabled,projectionAspect);
        projectionAspect=aspect;
        Cpu target=cpu;if(active&&(changed||target!=null&&target.gte.projectionScale()!=Gte.projectionScale(enabled,aspect))){commands.add(()->{if(cpu!=null)cpu.gte.setWidescreen(enabled,aspect);});wake();}
    }
    private boolean widescreenEnabled(){return wideOverride!=null?wideOverride:Boolean.parseBoolean(options.getOrDefault("video.widescreen_hack","false"));}
    public void framePresentation(int mode,int multiple){presentationModeOverride=mode;presentationMultipleOverride=Math.max(2,Math.min(4,multiple));updatePresentation();}
    public String framePresentationStatus(){Gpu target=gpu;return target==null?"Inactive: no game is running":target.presentationStatus();}
    private void updatePresentation(){
        int mode=presentationModeOverride>=0?presentationModeOverride:switch(options.getOrDefault("video.frame_presentation","off")){case "bfi"->1;case "shader_subframes"->2;default->0;};
        int multiple=presentationModeOverride>=0?presentationMultipleOverride:(int)decimal(options.get("video.presentation_multiplier"),2);
        Gpu target=gpu;if(target!=null)target.configurePresentation(mode,multiple,displayRefreshHz,!fast&&!blocked());
    }
    private final Cheats cheats=new Cheats();
    public int cheatsLoad(String path){try{return cheats.load(path);}catch(IOException|RuntimeException error){journal.error("cheats.load",error);notice.accept("Cannot load cheats: "+error.getMessage());return -1;}}
    public String cheatsList(){return cheats.describe();}
    public int cheatsApply(boolean enabled,String[] names){int count=cheats.arm(enabled,names);journal.event(EngineLog.INFO,"cheats.selection",cheats.diagnostic());return count;}
    public int cheatsArmedCount(){return cheats.armedCount();}
    private void configureCheats(){
        boolean enabled=Boolean.parseBoolean(options.getOrDefault("cheats.enabled","false"));
        cheatsLoad(enabled?options.getOrDefault("cheats.file",""):"");
        cheatsApply(enabled,Cheats.selectedNames(options.get("cheats.enabled_codes")));
    }
    private record TextureOptions(boolean dump,boolean replace,String directory,boolean automatic){}
    private volatile TextureOptions textureOptions;
    private volatile String serial="";
    public String gameSerial(){return serial;}
    public void textureOptions(boolean dump,boolean replace,String directory,boolean automatic){
        textureOptions=new TextureOptions(dump,replace,directory==null?"":directory,automatic);
        if(active){commands.add(this::updateTextures);wake();}
    }
    public long[] textureMetrics(){Gpu target=gpu;return target==null?new long[9]:target.textureMetrics();}
    public String textureStatus(){Gpu target=gpu;return target==null?"Texture options will apply when a game starts":target.textureStatus();}
    private void updateTextures(){
        Gpu target=gpu;if(target==null)return;
        TextureOptions request=textureOptions;
        boolean dump=request!=null?request.dump:Boolean.parseBoolean(options.getOrDefault("video.texture_dump","false"));
        boolean replace=request!=null?request.replace:Boolean.parseBoolean(options.getOrDefault("video.texture_replacements","false"));
        String directory=request!=null?request.directory:options.getOrDefault("video.texture_dir","");
        if((request!=null&&request.automatic)||directory.isBlank()){
            File parent=request!=null&&!request.directory.isBlank()?new File(request.directory).getParentFile():new File(root.getParentFile(),"textures");
            directory=new File(parent,serial.isBlank()?"_common":serial).toString();
        }
        target.configureTextures(dump,replace,directory);
    }
    private void mediaTextures(){
        String boot=media!=null&&media.disc!=null?media.disc.bootPath:"";
        java.util.regex.Matcher match=java.util.regex.Pattern.compile("(?i)([A-Z]{4})[_-]([0-9]{3})[.]?([0-9]{2})").matcher(boot);
        serial=match.find()?match.group(1).toUpperCase(Locale.ROOT)+"-"+match.group(2)+match.group(3):"";
        updateTextures();
    }
    private void prepareStateIdentity()throws IOException{
        if(!stateId.isEmpty()||media==null)return;
        stateId=BackendStateStore.identity(media.disc==null?"":media.disc.bootPath,media.executable);
        lastGameId=MigrationBackup.digest(stateId.getBytes(StandardCharsets.UTF_8));
        int adopted=0;try{adopted=BackendStateStore.adoptLegacy(root,stateId,gamePath,bootPath,Set.copyOf(sessionAliases),source->{
            if(stop)return "";
            String lower=source.toLowerCase(Locale.ROOT);
            PreparedMedia prepared=(lower.endsWith(".m3u")||lower.endsWith(".cue")&&source.startsWith("content://"))&&mediaResolver!=null?mediaResolver.prepare(source):new PreparedMedia(source,null);
            try(prepared;Media old=openMedia(prepared.path())){return BackendStateStore.identity(old.disc==null?"":old.disc.bootPath,old.executable);}
        });}catch(IOException failure){journal.error(EngineLog.WARN,"states.identity.migration",failure);}
        journal.event(EngineLog.INFO,"states.identity",Map.of("game_id",stateId,"adopted_legacy_slots",adopted));
    }
    public void shadersChanged(){if(active&&blocked()){commands.add(()->{if(gpu!=null)gpu.prepareShaders();});wake();}}
    public void audioVolume(int volume){volumeOverride=volume;updateAudio();}
    public void audioMuted(boolean muted){mutedOverride=muted;updateAudio();}
    public void fastForwardSpeed(float speed){fastSpeedOverride=Float.isFinite(speed)?speed:2;updateAudio();}
    private boolean muteFast(){return fast&&Boolean.parseBoolean(options.getOrDefault("audio.mute_fast_forward","false"));}
    private float fastSpeed(){float configured=fastSpeedOverride>=0?fastSpeedOverride:decimal(options.get("runtime.fast_forward_speed"),2);return configured<=0?0:Math.max(1,Math.min(16,configured));}
    private void updateAudio(){
        AudioOutput output=audio;if(output==null)return;output.setFastForwardTempo(fast&&!muteFast()?fastSpeed():1);
        boolean muted=mutedOverride!=null?mutedOverride:Boolean.parseBoolean(options.getOrDefault("audio.muted","false"));
        float normal=volumeOverride>=0?volumeOverride:decimal(options.get("audio.volume"),100);
        float volume=fast?decimal(options.get("audio.fast_forward_volume"),100):normal;
        output.setGain(muted||muteFast()?0:volume/100f);
    }
    private record Restore(byte[] state,String disc,String identity,CompletableFuture<Boolean> ready) {}
    private static final class Media implements AutoCloseable {
        final ByteSource source; final CueDisc cue; final IsoDisc disc; final byte[] executable;
        Media(ByteSource source)throws IOException{
            this.source=source;cue=null;
            // A decoded disc's system area may itself begin with executable data.
            // Explicit PBP/CHD container identity takes precedence over that prefix.
            if(!(source instanceof PbpDisc)&&!(source instanceof ChdSource)&&new String(source.read(0,8),StandardCharsets.US_ASCII).equals("PS-X EXE")){
                disc=null;byte[] header=source.read(0,2048);int size=ExeLoader.le(header,28);
                if(size<4||size>2*1024*1024)throw new IOException("Invalid executable size");executable=source.read(0,size+2048);
            }else{disc=source instanceof PbpDisc||source instanceof ChdSource chd&&chd.hasTracks()?new IsoDisc((SectorSource)source):new IsoDisc(source);executable=disc.bootExecutable();}
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
    /** Attach only on the VM thread: the core must never format guest events with logging off. */
    private void updateGuestAudioTracing(){
        boolean enabled=journal.accepts(EngineLog.INFO);if(bus==null||enabled==guestAudioTracing)return;guestAudioTracing=enabled;
        Bus target=bus;
        target.cdrom.setTraceSink(enabled?event->guestAudioEvent("audio.guest.cd.event",target,event):null);
        target.spu.setTraceSink(enabled?event->guestAudioEvent("audio.guest.spu.event",target,event):null);
        if(enabled)guestAudioSnapshot();
    }
    private void guestAudioEvent(String path,Bus target,String event){
        if(!journal.accepts(EngineLog.INFO))return;Cpu current=cpu;
        journal.event(EngineLog.INFO,path,Map.of("event",event,"cpu_boundary_clocks",current==null?0:current.instructions,
                "cpu_boundary_pc",Integer.toHexString(current==null?0:current.pc),"spu_frames",target.spu.frames,
                "context_note","CPU context is the last Java/native boundary, not the precise MMIO instruction; SPU frames are sampled at the event."));
    }
    private void guestAudioSnapshot(){
        if(bus==null||!journal.accepts(EngineLog.INFO))return;Cpu current=cpu;
        journal.event(EngineLog.INFO,"audio.guest.snapshot",Map.of("guest_clocks",current==null?0:current.instructions,
                "pc",Integer.toHexString(current==null?0:current.pc),"cd",bus.cdrom.loggingSnapshot(),"spu",bus.spu.loggingSnapshot()));
    }
    public void displayMetadata(int width,int height,float hz){if(width>0){surfaceWidth=width;surfaceHeight=height;}if(hz>0)displayRefreshHz=hz;updateProjection();updatePresentation();journal.event(EngineLog.INFO,"presentation.display",Map.of("width",surfaceWidth,"height",surfaceHeight,"refresh_hz",displayRefreshHz));}
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
        Media media=openMediaImage(path);
        try{attachSubchannel(media,path);}catch(IOException e){journal.error("cd.sbi",e);notice.accept("Ignoring SBI file: "+e.getMessage());}
        return media;
    }
    // LibCrypt PAL discs need the SBI that the game library stages beside the image (same name, .sbi).
    private void attachSubchannel(Media media,String path)throws IOException{
        if(media.disc==null)return;byte[] registered=discSubchannels.get(path);
        if(registered!=null){Map<Integer,byte[]> entries=SubchannelPatch.parse(registered);media.disc.subchannel(entries);journal.event(EngineLog.INFO,"cd.sbi.loaded",Map.of("file","registered","entries",entries.size()));return;}
        Uri uri=Uri.parse(path);if(uri.getScheme()!=null&&!uri.getScheme().equals("file"))return;
        File image=new File(uri.getScheme()==null?path:uri.getPath()),parent=image.getParentFile();if(parent==null)return;
        String name=image.getName(),stem=name.lastIndexOf('.')>0?name.substring(0,name.lastIndexOf('.')):name;
        File[] matches=parent.listFiles((dir,candidate)->candidate.equalsIgnoreCase(stem+".sbi"));
        if(matches==null||matches.length!=1)return;
        if(matches[0].length()>1024*1024)throw new IOException("SBI file is too large");
        Map<Integer,byte[]> entries=SubchannelPatch.parse(Files.readAllBytes(matches[0].toPath()));
        media.disc.subchannel(entries);journal.event(EngineLog.INFO,"cd.sbi.loaded",Map.of("file",matches[0].getName(),"entries",entries.size()));
    }
    private Media openMediaImage(String path)throws IOException{
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
        ByteSource source=source(path);try{Media result=new Media(source);if(source instanceof PbpDisc pbp)journal.event(EngineLog.INFO,"cd.pbp.mounted",Map.of("tracks",pbp.lastTrack(),"sectors",pbp.sectorCount(),"discs",pbp.discCount()));journal.event(EngineLog.INFO,"media.boot",Map.of("kind",result.disc==null?"psx_exe":"disc_image","boot_path",result.disc==null?"":result.disc.bootPath,"header",ExeLoader.headerSummary(result.executable)));return result;}catch(Exception e){source.close();throw e;}
    }
    /** Blocking, as ArmSX1's existing runVMThread contract requires. */
    public boolean run(String path){return run(path, () -> true);}
    /** The host's session cancellation and this active/stop transition share this monitor. */
    public boolean run(String path,java.util.function.BooleanSupplier allowed){
        synchronized(this){if(!allowed.getAsBoolean()||active||root==null)return false;active=true;stop=false;paused=false;frames=nonzero=guestClocks=0;gamePath=configuredGame.isEmpty()?path:configuredGame;stateId="";lastGameId=MigrationBackup.digest(gamePath.getBytes(StandardCharsets.UTF_8));configuredGame="";bootPath=discPath=path;lastError="";lastFailureCpuContext="";lastPc=0;lastAudio=lastCore="Not started";lastCpuTiming=lastGpuTiming=Map.of();fps=0;}
        boolean booted=false;Restore restore=null;int priority=boostThread();startupTiming.reset();performance.reset();hostSampleTime=hostSampleCpu=hostThermalSample=0;hostThermalStatus=-1;hostCpuPercent=hostRssMb=hostAvailableMb=-1;timeline.start(System.nanoTime());event("launch");
        try{
            options=readConfig();if(options.containsKey("video.texture_dump")||options.containsKey("video.texture_replacements")||options.containsKey("video.texture_dir"))textureOptions=null;configureLogging(options);journal.startSession(MigrationBackup.digest(path.getBytes(StandardCharsets.UTF_8)));bios=readBios();configureCheats();
            do{
                restart=null;performance.reset();
                int scale=Math.max(1,Math.min(5,Integer.parseInt(options.getOrDefault("video.internal_scale","1"))));
                RendererSelection selected=RendererSelection.from(options);int mode=selected.presentation();
                // Keep one graphics API attached to a retained Android Surface. Software
                // still rasterizes on the CPU; its finished pixels use the texture presenter.
                try(Gpu graphics=mode==0&&scale==1&&!selected.hardware()?new Gpu(true,false):new Gpu(true,mode,scale,selected.hardware())){
                    gpu=graphics;displayGpu=graphics;gpu.setJournal(journal);ShaderChain.backend(mode);updateDisplay();updatePresentation();clearSurface();gpu.prepareShaders();configurePads();bus=new Bus(gpu,pads[0]);bus.dma.setJournal(journal);bus.sio0.setPad(1,pads[1]);bus.loadBios(bios);
                    try(CardStorage cards=new CardStorage(new File(root,"cards"),bus)){
                        String mounted=restore==null?discPath:restore.disc();media=mounted.isEmpty()?null:openMedia(mounted);prepareStateIdentity();mediaTextures();if(media!=null&&media.disc!=null)bus.cdrom.insert(media.disc);
                        try{
                            cpu=new Cpu(bus);cpu.reset();guestAudioTracing=false;updateGuestAudioTracing();
                            if(restore!=null&&restore.state()!=null)SaveState.restore(restore.state(),cpu,bus,gpu,restore.identity());
                            else if(media!=null&&(media.disc==null||media.disc.region()==' '&&Boolean.parseBoolean(options.getOrDefault("cpu.fast_boot","false")))){cpu=BiosBoot.initialize(bios,bus,()->stop);bus.cdrom.prepareBootHandoff();ExeLoader.loadInto(media.executable,bus,cpu);}
                            else if(media!=null&&Boolean.parseBoolean(options.getOrDefault("cpu.fast_boot","false")))cpu=BiosBoot.discBoot(bios,bus,()->stop);
                            cpu.gte.setWidescreen(widescreenEnabled(),projectionAspect);
                            String cpuMode=options.getOrDefault("cpu.execution_mode","cached");
                            if(!cpuMode.equals("interpreter"))cpu.enableNative(!cpuMode.equals("cached_interpreter"),true,true);
                            try(AudioOutput output=new AudioOutput(true,Boolean.parseBoolean(options.getOrDefault("diagnostics.audio_capture","false")),context)){
                                audio=output;output.setJournal(journal);updateAudio();output.setPaused(blocked());bus.spu.setAudioSink((pcm,count)->{int audible=0;for(int i=0;i<count;i++)if(pcm[i]!=0)audible++;nonzero+=audible;output.samples(pcm,count);});
                                renderer="PSX Lab / "+gpu.renderer();booted=true;
                                timeline.incident(System.nanoTime(),"CPU execution mode: "+cpu.executionMode());
                                timeline.incident(System.nanoTime(),"start: "+renderer+", scale "+scale+", "+output.deviceDiagnostic()+", GPU "+(gpu.hardwareDescription().isEmpty()?"none (software renderer)":gpu.hardwareDescription())+", ADPF "+(adpf?"on":"off")+", VM thread priority "+threadPriority()+", "+Runtime.getRuntime().availableProcessors()+" cores"+", disc "+discLayout(media));
                                if(restore!=null){discPath=restore.disc();restore.ready().complete(true);}pauseCallback.accept(paused);event("ready");
                                try{loop(output);}catch(Exception failure){lastFailureCpuContext=cpu.diagnostic();journal.event(EngineLog.FATAL,"runtime.cpu_context",Map.of("cpu",lastFailureCpuContext,"exception",failure.toString(),"guest_clocks",cpu.instructions));throw failure;}finally{try{lastAudio=output.diagnostic();}catch(IllegalStateException ignored){}}
                            }finally{audio=null;bus.cdrom.setTraceSink(null);bus.spu.setTraceSink(null);guestAudioTracing=false;bus.spu.setAudioSink(null);cpu.closeNative();}
                        }finally{if(media!=null){media.close();media=null;}if(pendingMedia!=null){pendingMedia.close();pendingMedia=null;}}
                    }finally{clearSurface();}
                }finally{displayGpu=null;gpu=null;bus=null;cpu=null;}
                // Retain only a staged disc needed by the next machine, after the old
                // CD readers have closed. Repeated loads must not accumulate game copies.
                String keep=restart==null?"":restart.disc();
                for(Iterator<PreparedMedia> it=restoredMedia.iterator();it.hasNext();){PreparedMedia prepared=it.next();if(!prepared.path().equals(keep)){discSources.remove(prepared.path());try{prepared.close();}finally{it.remove();}}}
                restore=restart;
            }while(!stop&&restore!=null);
        }catch(Exception e){booted=false;error(e);if(restore!=null)restore.ready().complete(false);if(restart!=null)restart.ready().complete(false);}
        finally{restoreThread(priority);performance.reset();resetButtons();fps=0;renderer="Stopped";synchronized(this){active=false;}Runnable job;while((job=commands.poll())!=null)job.run();event("stopped");writeReport();cheats.arm(false,null);for(PreparedMedia prepared:restoredMedia)try{prepared.close();}catch(Exception e){error(e);}restoredMedia.clear();discSources.clear();clearSessionOverrides();gamePath=discPath=bootPath="";serial="";}
        return booted;
    }
    /** The host reuses its VM thread: raise it to the GPU worker's and DSP's level for the session only. */
    private static int boostThread(){try{int old=android.os.Process.getThreadPriority(android.os.Process.myTid());android.os.Process.setThreadPriority(Math.min(old,android.os.Process.THREAD_PRIORITY_DISPLAY));return old;}catch(RuntimeException e){return Integer.MIN_VALUE;}}
    private static void restoreThread(int old){if(old!=Integer.MIN_VALUE)try{android.os.Process.setThreadPriority(old);}catch(RuntimeException ignored){}}
    public void setAdpf(boolean value){adpf=value;}
    /** Disc format and track layout, so a report shows missing CD-audio tracks (a single data track). */
    private static String discLayout(Media value){
        if(value==null||value.disc==null)return "none (executable)";
        IsoDisc disc=value.disc;StringBuilder out=new StringBuilder();
        try{
            // CUE media owns a CueDisc instead of a ByteSource. Diagnostics must
            // describe either owner without preventing startup or a disc change.
            out.append(value.cue!=null?"CUE/BIN":value.source!=null?value.source.getClass().getSimpleName():"disc image");
            int tracks=disc.lastTrack(),audio=0;out.append(", ").append(tracks).append(" track(s), ").append(disc.sectorCount()).append(" sectors");
            for(int track=1;track<=tracks;track++){int start=disc.trackStart(track);boolean sound=disc.audioTrack(start);if(sound)audio++;if(track<=16)out.append(track==1?": ":", ").append(track).append(sound?" audio @":" data @").append(start);}
            if(tracks>16)out.append(", ...");
            out.append("; ").append(audio).append(" audio track(s)");
        }catch(RuntimeException e){out.append(", track list unavailable: ").append(e.getMessage());}
        return out.toString();
    }
    /** GPU execution time per stage over a row, as GPU milliseconds per second (1000 = the GPU never idle). */
    private static String gpuSummary(long[] before,long[] now,long span){
        if(now==null)return "gpu: software renderer";
        if(now[0]==0)return "gpu: timer queries unavailable on this GPU";
        if(now[0]!=1)return "gpu: timing disabled (logging off)";
        if(before==null||span<=0)return "gpu: starting";
        double perSecond=1e9/span/1e6;String[] names={"draw","resolve","output","transfer","upload"};StringBuilder out=new StringBuilder("gpu ms/s");long total=0;
        for(int i=0;i<5;i++){long d=now[1+i]-before[1+i];total+=d;out.append(String.format(Locale.ROOT," %s %.0f",names[i],d*perSecond));}
        out.append(String.format(Locale.ROOT," = %.0f, draw spans %.0f/s",total*perSecond,(now[6]-before[6])*1e9/span));
        if(now.length>=17&&before.length>=17)out.append(String.format(Locale.ROOT,", draws/s native %.0f HD %.0f",(now[15]-before[15])*1e9/span,(now[16]-before[16])*1e9/span));
        if(now.length>=21&&before.length>=21)out.append(String.format(Locale.ROOT,", HD batch ends/s texture %.0f transfer %.0f full %.0f other %.0f",(now[17]-before[17])*1e9/span,(now[18]-before[18])*1e9/span,(now[19]-before[19])*1e9/span,(now[20]-before[20])*1e9/span));
        long skipped=now[11]-before[11],lost=now[12]-before[12],waits=now[14]-before[14];
        if(skipped>0||lost>0)out.append(String.format(Locale.ROOT," (untimed spans %d, discarded %d)",skipped,lost));
        out.append(String.format(Locale.ROOT,", frame limit waited %d ms over %d frames",(now[13]-before[13])/1_000_000,waits));
        return out.toString();
    }
    /** Top registers from Cpu.registerDeltas, e.g. "1F801044 Controller status r812 w0". */
    private static String registerSummary(long[] d){
        if(d.length==0)return "none";StringBuilder out=new StringBuilder();
        for(int at=0;at+3<d.length&&at<16;at+=4){if(out.length()>0)out.append(", ");out.append(String.format(Locale.ROOT,"%08X %s r%d w%d",(int)d[at],HardwareRegisterReport.name((int)d[at]),d[at+1],d[at+2]));}
        return out.toString();
    }
    private static long threadCpu(){try{return android.os.Debug.threadCpuTimeNanos();}catch(RuntimeException e){return 0;}}
    private static long gcCount(){try{String value=android.os.Debug.getRuntimeStat("art.gc.gc-count");return value==null?0:Long.parseLong(value);}catch(RuntimeException e){return 0;}}
    /** Monotonic counters for FrameCause. Native CPU statistics: {compiled, JIT instructions, interpreted, hits, links, invalidations, device calls, ...}. */
    private long[] causeCounters(long emulationCpu){
        long[] s=cpu.nativeStatistics(),c=new long[FrameCause.COUNT];Gpu g=gpu;
        c[FrameCause.EMULATION_CPU]=emulationCpu;c[FrameCause.COMPILED]=s[0];c[FrameCause.JIT]=s[1];c[FrameCause.INTERPRETED]=s[2];c[FrameCause.INVALIDATED]=s[5];c[FrameCause.DEVICE_CALLS]=s[6];
        c[FrameCause.PRIMITIVES]=g.primitives;c[FrameCause.CD_SECTORS]=bus.cdrom.sectorsRead;c[FrameCause.FMV_BLOCKS]=bus.mdec.decodedBlocks;
        c[FrameCause.GPU_QUEUE_FULL]=g.queueFullNanos;c[FrameCause.GPU_DRAW]=g.drawNanos;c[FrameCause.DISPLAY_WAIT]=g.displayWaitNanos;c[FrameCause.DISPLAY_CONVERT]=g.displayConvertNanos;
        c[FrameCause.DISPLAY_PRESENT]=g.displayPresentNanos;c[FrameCause.DISPLAY_PRESENT_CPU]=g.displayPresentCpuNanos;c[FrameCause.DISPLAYS]=g.displays;c[FrameCause.GC]=gcCount();c[FrameCause.READBACK_WAIT]=g.readbackWaitNanos;c[FrameCause.READBACKS]=g.readbackSyncs;
        return c;
    }
    /** Which core the VM thread last ran on and that core's clock against its maximum: shows throttling and small-core placement. */
    private static String coreReport(){
        try{
            String stat=new String(Files.readAllBytes(Paths.get("/proc/self/task/"+android.os.Process.myTid()+"/stat")),StandardCharsets.US_ASCII);
            int core=Integer.parseInt(stat.substring(stat.lastIndexOf(')')+2).trim().split(" ")[36]); // Field 39, counted from field 3.
            String folder="/sys/devices/system/cpu/cpu"+core+"/cpufreq/";
            try{
                long current=Long.parseLong(new String(Files.readAllBytes(Paths.get(folder+"scaling_cur_freq")),StandardCharsets.US_ASCII).trim()),maximum=Long.parseLong(new String(Files.readAllBytes(Paths.get(folder+"cpuinfo_max_freq")),StandardCharsets.US_ASCII).trim());
                return String.format(Locale.ROOT,", ran on core %d at %.2f of %.2f GHz",core,current/1e6,maximum/1e6);
            }catch(IOException|RuntimeException e){return ", ran on core "+core;}
        }catch(IOException|RuntimeException e){return "";}
    }
    private static int threadPriority(){try{return android.os.Process.getThreadPriority(android.os.Process.myTid());}catch(RuntimeException e){return 0;}}
    private boolean blocked(){Surface target=surface;return paused||audioSuspended||presentationSuspended||target==null||!target.isValid();}
    private void loop(AudioOutput output)throws Exception{try(FrameHints hints=new FrameHints(context)){loop(output,hints);}}
    private void loop(AudioOutput output,FrameHints hints)throws Exception{
        FastForwardPacer fastPacer=new FastForwardPacer();
        FrameRateMeter frameRate=new FrameRateMeter();AudioAwareFramePacer pacer=new AudioAwareFramePacer(cpu.instructions,System.nanoTime());long last=gpu.vblanks,measured=System.nanoTime();boolean wasBlocked=false,wasFast=false;EngineLog.Frame diagnosticFrame=null;long perfCpu=0,perfEpoch=0;
        // Frame work excludes pacing sleeps and blocked waits; the GPU worker joins once it has started.
        long workStarted=System.nanoTime();int[] hintThreads=null;
        // Report timeline: each frame's emulation time, plus per-row baselines for the counters below.
        long frameEmulation=0,rowBuffered=Long.MAX_VALUE,rowRefills=output.recoveries,rowDsp=output.dspCpuNanos(),rowSectors=bus.cdrom.sectorsRead,rowMdec=bus.mdec.decodedBlocks,rowCdCommands=bus.cdrom.commands,rowCdData=bus.cdrom.dataSectors,rowCdLost=bus.cdrom.lostSectors,rowCdOver=bus.cdrom.overwrittenReads,rowXa=bus.cdrom.xaSectors,rowCdda=bus.cdrom.cddaSectors,rowReadSyncs=gpu.readbackSyncs,rowReadSkips=gpu.readbackSkips,rowReadWait=gpu.readbackWaitNanos,seenRefills=rowRefills;
        int rowUnderruns=output.underruns(),seenUnderruns=rowUnderruns,seenThermal=hostThermalStatus,rowSkipped=0;
        // Late frames are explained against this session's normal frames; see FrameCause.
        FrameCause cause=new FrameCause();long[] causeBase=causeCounters(0);
        // Where code runs and which registers it touches, per row: a stuck game shows one small loop.
        long measuredRow=System.nanoTime();
        PcSampler pcSamples=new PcSampler();int loopRows=0,movieStallRows=0;cpu.registerDeltas();long[] rowGpu=gpu.gpuStageTimes();long frameEmulationCpu=0,lastFrameAt=0,rowDisplays=gpu.displays;
        while(!stop&&restart==null){
            Runnable job;while((job=commands.poll())!=null)job.run();if(stop||restart!=null)break;
            updateGuestAudioTracing();
            if(blocked()){
                if(!wasBlocked&&surface!=null&&surface.isValid()&&gpu.presentationNeedsRestore()){Surface target=surface;gpu.restorePresentation(target,true,frame->{if(frame!=null)draw(target,frame);});}
                output.setPaused(true);fastPacer.reset();wasBlocked=true;fps=0;diagnosticFrame=null;journal.leaveFrame();synchronized(gate){gate.wait(10);}continue;
            }
            if(wasBlocked){workStarted=System.nanoTime();timeline.resume(workStarted);lastFrameAt=0;performance.reset();perfCpu=perfEpoch=0;output.setPaused(muteFast());pacer=new AudioAwareFramePacer(cpu.instructions,System.nanoTime());measured=System.nanoTime();frameRate.reset();wasBlocked=false;}
            if(fast!=wasFast){timeline.incident(System.nanoTime(),"fast-forward "+(fast?"on, speed "+(fastSpeed()==0?"unlimited":fastSpeed()+"x")+(muteFast()?", muted":""):"off"));bus.spu.flushAudio();bus.spu.setAudioSink(muteFast()?null:(pcm,count)->{int audible=0;for(int i=0;i<count;i++)if(pcm[i]!=0)audible++;nonzero+=audible;output.samples(pcm,count);});updateAudio();output.setPaused(muteFast());pacer=new AudioAwareFramePacer(cpu.instructions,System.nanoTime());wasFast=fast;frameRate.reset();}
            if(pendingMedia!=null&&cpu.instructions>=insertAt){bus.cdrom.insert(pendingMedia.disc);if(media!=null)media.close();media=pendingMedia;pendingMedia=null;mediaTextures();timeline.incident(System.nanoTime(),"disc changed: "+discLayout(media));}
            long ticket=performance.ticket();
            if(ticket!=perfEpoch){perfCpu=0;perfEpoch=ticket;}
            gpu.performanceEnabled=ticket!=0;bus.dma.performanceEnabled=ticket!=0;
            long perfStarted=ticket!=0?System.nanoTime():0;
            cpu.configureProfiling(journal.enabled());
            if(diagnosticFrame==null)diagnosticFrame=journal.frame(cpu.instructions,cpu.pc);
            long emulationStarted=System.nanoTime(),emulationCpu=threadCpu();try(EngineLog.Span span=journal.begin("cpu.runToFrame",EngineLog.TRACE)){cpu.runToFrame(100000);}frameEmulation+=System.nanoTime()-emulationStarted;frameEmulationCpu+=threadCpu()-emulationCpu;pcSamples.add(cpu.pc);guestClocks=cpu.instructions;lastPc=cpu.pc;if(perfStarted!=0)perfCpu+=System.nanoTime()-perfStarted;
            if(gpu.vblanks==last)continue;last=gpu.vblanks;cheats.apply(bus);if(diagnosticFrame!=null)diagnosticFrame.cpuDone(cpu.instructions,cpu.pc);
            long flushStarted=ticket!=0?System.nanoTime():0;
            long devicesStarted=System.nanoTime(),devicesCpu=threadCpu();try(EngineLog.Span span=journal.begin("cpu.flush_devices",EngineLog.DEBUG)){bus.flushClocks();bus.spu.flushAudio();}frameEmulation+=System.nanoTime()-devicesStarted;frameEmulationCpu+=threadCpu()-devicesCpu;
            if(flushStarted!=0)perfCpu+=System.nanoTime()-flushStarted;
            long paceStarted=diagnosticFrame==null&&ticket==0?0:System.nanoTime(),buffered=output.bufferedFrames();
            boolean accelerating=fast;if(!accelerating)fastPacer.reset();
            long frameWork=System.nanoTime()-workStarted;rowBuffered=Math.min(rowBuffered,buffered);
            if(adpf){int worker=gpu.workerTid;if(hintThreads==null||hintThreads.length==1&&worker>0)hintThreads=worker>0?new int[]{android.os.Process.myTid(),worker}:new int[]{android.os.Process.myTid()};hints.report(hintThreads,(long)(1e9/Math.max(1,nominalFps)),frameWork);}else hints.close();
            long wait=accelerating?fastPacer.waitNanos(cpu.instructions,System.nanoTime(),fastSpeed()):pacer.waitNanos(cpu.instructions,System.nanoTime(),buffered);if(wait>0)TimeUnit.NANOSECONDS.sleep(wait);
            workStarted=System.nanoTime();
            float paced=accelerating?fastSpeed():1;
            long[] counters=causeCounters(frameEmulationCpu),frameDelta=FrameCause.delta(counters,causeBase);causeBase=counters;
            String late=timeline.frame(workStarted,paced<=0?0:(long)(1e9/Math.max(1,nominalFps)/paced),frameEmulation,frameWork,Math.max(0,wait));
            if(late!=null)timeline.incident(workStarted,late+", audio buffered "+buffered*1000/44100+" ms"+(accelerating?", fast-forward":"")+", pc "+String.format(Locale.ROOT,"%08X",cpu.pc)+", thermal "+hostThermalStatus+coreReport()+"; "+cause.explain(lastFrameAt==0?0:workStarted-lastFrameAt,frameEmulation,frameWork,Math.max(0,wait),frameDelta));
            else cause.add(frameDelta);
            lastFrameAt=workStarted;frameEmulation=frameEmulationCpu=0;
            long[] slowPresent=gpu.takeSlowPresent(SessionTimeline.STALL_NANOS);if(slowPresent!=null)timeline.incident(System.nanoTime(),FrameCause.slowPresent(slowPresent));
            String slowRead=gpu.takeSlowReadback();if(slowRead!=null)timeline.incident(System.nanoTime(),slowRead);
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
                boolean everyField=!accelerating&&gpu.requiresEveryFieldPresentation();
                boolean presentFrame=everyField||(accelerating?fastPacer.present(System.nanoTime(),displayRefreshHz):fastPacer.presentNormally());
                if(!presentFrame&&!accelerating)rowSkipped++;
                java.util.function.Consumer<Gpu.Frame> completedFrame=frame->{try{long drawing=ticket==0?0:System.nanoTime();if(frame!=null)draw(target,frame);long completed=System.nanoTime();
                    if(ticket!=0){Gpu.Performance p=graphics.performance();performance.completed(ticket,completed,measuredCpu,measuredIdle,p.presentNanos()+(frame==null?0:completed-drawing),new PerformanceStatistics.Sample(clocks,triangles,rectangles,lines,mdec,sectors,dma,activeVoices,p.width(),p.height(),p.scale()));}
                    frames++;float rate=frameRate.completed(completed);if(!Float.isNaN(rate)&&!stop&&!paused&&!audioSuspended&&!presentationSuspended&&surface==target)fps=rate;if(trace)startupTiming.record(clock,requested,completed);}finally{if(presentFrame&&!everyField)fastPacer.presentationComplete();}};
                if(presentFrame)gpu.requestSurfaceFrame(target,true,completedFrame,submitted);
                else gpu.completeFrameWithoutPresentation(completedFrame,submitted,accelerating?"fast_forward_skip":"display_backpressure");
            }
            journal.leaveFrame();
            long now=System.nanoTime();if(now-measured>=500000000L){boolean sampled=performance.enabled()||journal.enabled();if(sampled)sampleHostUsage(now);
                int underruns=output.underruns();long refills=output.recoveries;
                if(underruns>seenUnderruns||refills>seenRefills)timeline.incident(now,"audio"+(underruns>seenUnderruns?" underrun +"+(underruns-seenUnderruns):"")+(refills>seenRefills?" ran dry +"+(refills-seenRefills):"")+", buffered "+output.bufferedFrames()*1000/44100+" ms, fps "+String.format(Locale.ROOT,"%.1f",fps)+(fast?", fast-forward":""));
                seenUnderruns=underruns;seenRefills=refills;
                if(timeline.due(now)){
                    if(!sampled)sampleHostUsage(now);
                    if(hostThermalStatus!=seenThermal){timeline.incident(now,"thermal status "+seenThermal+" -> "+hostThermalStatus);seenThermal=hostThermalStatus;}
                    long dsp=output.dspCpuNanos(),cdSectors=bus.cdrom.sectorsRead,fmvBlocks=bus.mdec.decodedBlocks;
                    timeline.row(now,String.format(Locale.ROOT,"%s | display shown %d, skipped %d | audio min %d ms, underruns +%d, dry +%d, dsp %d%% | cpu %.0f%% thermal %d rss %.0f MB | vram reads waited +%d (%d ms), skipped +%d | cd commands +%d %s, sectors +%d, xa +%d, cdda +%d, fmv blocks +%d, data +%d, lost +%d, overwritten +%d",fast?"fast-forward":"normal",gpu.displays-rowDisplays,rowSkipped,rowBuffered==Long.MAX_VALUE?-1:rowBuffered*1000/44100,underruns-rowUnderruns,refills-rowRefills,(dsp-rowDsp)/(SessionTimeline.INTERVAL_NANOS/100),hostCpuPercent,hostThermalStatus,hostRssMb,gpu.readbackSyncs-rowReadSyncs,(gpu.readbackWaitNanos-rowReadWait)/1_000_000,gpu.readbackSkips-rowReadSkips,bus.cdrom.commands-rowCdCommands,bus.cdrom.recentCommands(),cdSectors-rowSectors,bus.cdrom.xaSectors-rowXa,bus.cdrom.cddaSectors-rowCdda,fmvBlocks-rowMdec,bus.cdrom.dataSectors-rowCdData,bus.cdrom.lostSectors-rowCdLost,bus.cdrom.overwrittenReads-rowCdOver));
                    String io=registerSummary(cpu.registerDeltas());long hot=pcSamples.hotWindow();
                    long[] gpuNow=gpu.gpuStageTimes();timeline.rowDetail("code "+pcSamples.top(3)+" | io "+io+" | "+gpuSummary(rowGpu,gpuNow,now-measuredRow));rowGpu=gpuNow;measuredRow=now;
                    if((int)hot>=85){if(++loopRows==2)timeline.incident(now,String.format(Locale.ROOT,"possible hang: game code has stayed in one small loop at %08X (%d%% of samples) for about 10 s; normal in a quiet menu, a hang if the screen is stuck; hardware used most: %s",(int)(hot>>>32),(int)hot,io));}else loopRows=0;
                    pcSamples.clear();
                    rowBuffered=Long.MAX_VALUE;rowDisplays=gpu.displays;rowSkipped=0;rowUnderruns=underruns;rowRefills=refills;rowDsp=dsp;rowSectors=cdSectors;long cdData=bus.cdrom.dataSectors-rowCdData,cdLost=bus.cdrom.lostSectors-rowCdLost,cdOver=bus.cdrom.overwrittenReads-rowCdOver;
                    if(cdLost>0||cdOver>0)timeline.incident(now,String.format(Locale.ROOT,"cd sectors lost +%d (last lba %d), read buffers overwritten +%d (last lba %d), mode %02X",cdLost,bus.cdrom.lastLostLba,cdOver,bus.cdrom.lastOverwriteLba,bus.cdrom.modeRegister()));
                    // A movie streams mostly data sectors with XA audio; no MDEC work for two rows means its picture is stuck.
                    if(cdData>=200&&bus.cdrom.xaSectors>rowXa&&fmvBlocks==rowMdec){if(++movieStallRows==2)timeline.incident(now,String.format(Locale.ROOT,"movie stalled: %d data sectors and XA audio streamed in 5 s but no video was decoded; lost +%d, overwritten +%d, mode %02X",cdData,cdLost,cdOver,bus.cdrom.modeRegister()));}else movieStallRows=0;
                    rowMdec=fmvBlocks;rowCdData=bus.cdrom.dataSectors;rowCdLost=bus.cdrom.lostSectors;rowCdOver=bus.cdrom.overwrittenReads;rowCdCommands=bus.cdrom.commands;rowXa=bus.cdrom.xaSectors;rowCdda=bus.cdrom.cddaSectors;rowReadSyncs=gpu.readbackSyncs;rowReadSkips=gpu.readbackSkips;rowReadWait=gpu.readbackWaitNanos;
                }nominalFps=videoRate(gpu.status());lastAudio=output.diagnostic();lastCore=cpu.executionDiagnostic()+"\n"+bus.cdrom.diagnostic()+"\n"+bus.dma.diagnostic()+"\n"+bus.mdec.diagnostic()+"\n"+bus.spu.diagnostic();renderer="PSX Lab / "+gpu.renderer();if(journal.enabled()){guestAudioSnapshot();lastCpuTiming=cpu.nativeTimings();lastGpuTiming=gpu.nativeTimings();journal.event(EngineLog.INFO,"engine.snapshot",Map.of("fps",fps,"thermal_status",hostThermalStatus,"audio",output.loggingSnapshot(),"audio_summary",lastAudio,"core",lastCore,"gpu",gpu.renderStatistics(),"native_cpu",lastCpuTiming,"native_gpu",lastGpuTiming,"memory",Map.of("process_rss_mb",hostRssMb,"system_available_mb",hostAvailableMb,"java_heap_used_bytes",Runtime.getRuntime().totalMemory()-Runtime.getRuntime().freeMemory(),"java_heap_limit_bytes",Runtime.getRuntime().maxMemory(),"native_heap_allocated_bytes",android.os.Debug.getNativeHeapAllocatedSize())));}measured=now;}
        }
    }
    private void clearSurface(){Surface target=surface;if(target==null||!target.isValid()||gpu==null)return;gpu.control(0x03000001);gpu.requestSurfaceFrame(target,false,frame->{if(frame!=null)draw(target,frame);});gpu.sync();}
    // Software output reuses one image and lets the GPU scale it; a new full-frame Bitmap per field
    // fed the garbage collector, and CPU filtering to a 1080p+ panel cost more than emulating the frame.
    private final Object canvasLock=new Object();
    private final Paint canvasPaint=new Paint(Paint.FILTER_BITMAP_FLAG);
    private final RectF canvasRect=new RectF();
    private Bitmap canvasBitmap;
    private boolean softwareCanvas;
    private void draw(Surface target,Gpu.Frame frame){
        if(surface!=target||!target.isValid())return;
        synchronized(canvasLock){
            int w=frame.width(),h=frame.height();
            if(canvasBitmap==null||canvasBitmap.getWidth()!=w||canvasBitmap.getHeight()!=h){if(canvasBitmap!=null)canvasBitmap.recycle();canvasBitmap=Bitmap.createBitmap(w,h,Bitmap.Config.ARGB_8888);}
            canvasBitmap.setPixels(frame.pixels(),0,w,0,0,w,h);
            try{Canvas canvas=lockCanvas(target);try{canvas.drawColor(Color.BLACK);float width=outputStretch?canvas.getWidth():Math.min(canvas.getWidth(),canvas.getHeight()*outputAspect),height=outputStretch?canvas.getHeight():width/outputAspect;if(outputInteger&&!outputStretch){int k=(int)Math.min(width/w,height/h);if(k>=1){width=w*k;height=h*k;}}float x=(canvas.getWidth()-width)/2,y=(canvas.getHeight()-height)/2;canvasRect.set(x,y,x+width,y+height);canvas.drawBitmap(canvasBitmap,null,canvasRect,canvasPaint);}finally{target.unlockCanvasAndPost(canvas);}}catch(IllegalArgumentException|IllegalStateException ignored){}
        }
    }
    /** A device whose hardware canvas fails once keeps the CPU canvas for the rest of the process. */
    private Canvas lockCanvas(Surface target){
        if(!softwareCanvas)try{return target.lockHardwareCanvas();}catch(IllegalArgumentException|IllegalStateException|UnsupportedOperationException e){softwareCanvas=true;}
        return target.lockCanvas(null);
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
    /** Read-only copy of the 2 MiB main RAM, taken on the emulation thread; empty with no game running. */
    public byte[] memorySnapshot(){return call(()->bus.snapshotRam(),new byte[0]);}
    public boolean save(int slot){return call(()->{if(pendingMedia!=null)throw new IOException("Wait for the disc tray to close before saving");File file=BackendStateStore.file(root,stateId.isEmpty()?gamePath:stateId,slot);String disc=discPath.equals(bootPath)?"":discSources.getOrDefault(discPath,discPath);if(disc.contains("/cache/saf-launch/"))throw new IOException("Cannot save a temporary disc without its original source");BackendStateStore.write(file,gamePath,disc,stateId,SaveState.capture(cpu,bus,gpu,gamePath));screenshotNow(file+".png");if(slot==-1){atomic(new File(root,"autosave-game.txt"),gamePath.getBytes(StandardCharsets.UTF_8));atomic(new File(root,"autosave-key.txt"),(stateId+"\n"+gamePath).getBytes(StandardCharsets.UTF_8));}event("save-slot="+slot);return true;},false);}
    public boolean load(int slot,boolean acknowledgeCards){
        if(!acknowledgeCards){notice.accept("Loading restores machine state using the current memory cards. Back up your cards first.");return false;}
        CompletableFuture<Boolean> ready=new CompletableFuture<>();
        if(!active)return false;
        PreparedMedia prepared=null;
        try{
            BackendStateStore.Stored stored=BackendStateStore.read(stateFile(slot));
            if(stored.identity().isEmpty()?!BackendStateStore.sameGame(stored.game(),gamePath,bootPath):!stored.identity().equals(stateId))throw new IOException("Save state belongs to a different game");
            String disc=stored.disc();
            // v1 states recorded the deleted SAF boot copy. Match only its generated URI hash.
            if(disc.isEmpty()||BackendStateStore.sameGame(disc,gamePath,bootPath))disc=bootPath;
            else if(mediaResolver!=null)prepared=mediaResolver.prepare(disc);
            String mounted=prepared==null?disc:prepared.path();PreparedMedia retained=prepared;
            boolean accepted=call(()->{SaveState.validate(stored.payload(),stored.game(),bios);try(Media checked=openMedia(mounted)){}if(retained!=null){restoredMedia.add(retained);discSources.put(mounted,stored.disc());}restart=new Restore(stored.payload(),mounted,stored.game(),ready);event("load-slot="+slot);return true;},false);
            if(!accepted){if(prepared!=null)prepared.close();return false;}
        }catch(Exception e){if(prepared!=null)try{prepared.close();}catch(Exception closing){e.addSuppressed(closing);}error(e);return false;}
        try{return ready.get(15,TimeUnit.SECONDS);}catch(Exception e){error(e);return false;}
    }
    public void reset(){call(()->{restart=new Restore(null,discPath,gamePath,new CompletableFuture<>());return true;},false);}
    /** Apply internal resolution and rasterizer changes to the running game. Both are fixed when a session's Gpu is
     *  created, so an in-game change used to wait for a restart. This snapshots the machine and goes round the same
     *  restart path a state load uses, with the settings read again; the state format skips those Gpu fields, so the
     *  restore keeps the new ones. The graphics API stays the one the session started with. Blocking, like load. */
    public boolean reloadVideo(){
        CompletableFuture<Boolean> ready=new CompletableFuture<>();
        boolean accepted=call(()->{if(pendingMedia!=null)throw new IOException("Wait for the disc tray to close before changing the resolution");
            byte[] state=SaveState.capture(cpu,bus,gpu,gamePath);Map<String,String> next=readConfig();String api=options.get("video.gpu_backend");
            if(api==null)next.remove("video.gpu_backend");else next.put("video.gpu_backend",api);options=next;
            restart=new Restore(state,discPath,gamePath,ready);event("reload-video");return true;},false);
        if(!accepted)return false;try{return ready.get(15,TimeUnit.SECONDS);}catch(Exception e){error(e);return false;}
    }
    private String stateIdentity(int slot){if(!gamePath.isEmpty()||slot!=-1||root==null)return gamePath;File index=new File(root,"autosave-game.txt");try{return index.length()<=65536?new String(Files.readAllBytes(index.toPath()),StandardCharsets.UTF_8):"";}catch(IOException e){return "";}}
    public File stateFile(int slot){
        String source=stateIdentity(slot),key=gamePath.isEmpty()?"":stateId;
        if(gamePath.isEmpty()&&slot==-1&&root!=null)try{
            File index=new File(root,"autosave-key.txt");if(index.length()<=66000){String text=new String(Files.readAllBytes(index.toPath()),StandardCharsets.UTF_8);int split=text.indexOf('\n');if(split>=0&&text.substring(split+1).equals(source)&&BackendStateStore.validIdentity(text.substring(0,split)))key=text.substring(0,split);}
        }catch(IOException ignored){}
        return BackendStateStore.find(root,key,source,bootPath,slot);
    }
    public String stateGame(int slot){try{return stateFile(slot).isFile()?stateIdentity(slot):"";}catch(IllegalArgumentException e){return "";}}
    public byte[] preview(int slot){try{return Files.readAllBytes(new File(stateFile(slot)+".png").toPath());}catch(IOException|IllegalArgumentException e){return new byte[0];}}
    public void screenshot(String path){Gpu.Frame frame=call(()->gpu.displayFrame(),null);if(frame!=null)try{writeScreenshot(path,frame);}catch(IOException e){error(e);}}
    private void screenshotNow(String path)throws IOException{writeScreenshot(path,gpu.displayFrame());}
    // Only framebuffer capture needs a guest boundary. PNG encoding and fsync
    // belong to the requesting host thread, so diagnosis does not starve audio.
    private void writeScreenshot(String path,Gpu.Frame frame)throws IOException{Bitmap image=Bitmap.createBitmap(frame.pixels(),frame.width(),frame.height(),Bitmap.Config.ARGB_8888);try(ByteArrayOutputStream bytes=new ByteArrayOutputStream()){if(!image.compress(Bitmap.CompressFormat.PNG,100,bytes))throw new IOException("Screenshot failed");atomic(new File(path),bytes.toByteArray());}finally{image.recycle();}}
    public boolean changeDisc(String path){return call(()->{Media next=openMedia(path);if(next.disc==null){next.close();throw new IOException("Disc replacement requires a CD image");}if(pendingMedia!=null)pendingMedia.close();bus.cdrom.eject();pendingMedia=next;insertAt=cpu.instructions+33868800L;discPath=path;event("disc-change");return true;},false);}
    /** Frontend code for the DualShock Analog button (NativeApp.setPadButton). */
    static final int ANALOG_BUTTON=200;
    public void button(int player,int key,int range,boolean down){if(player<0||player>=pads.length)return;if(key==ANALOG_BUTTON){if(down){pads[player].toggleAnalog();event("analog-button player="+player);}return;}if((key>=110&&key<=113)||(key>=120&&key<=123)){int stick=key>=120?1:0,offset=key%10;PadState.State s=pads[player].get();int x=stick==0?s.leftX():s.rightX(),y=stick==0?s.leftY():s.rightY();int v=down?Math.min(127,Math.max(0,range)*127/32767):0;if(offset==0||offset==2)y=128+(offset==0?-v:v);else x=128+(offset==3?-v:v);analog(player,stick,x,y);return;}int bit=buttonBit(key);if(bit<0)return;PadState p=pads[player];synchronized(p){PadState.State s=p.get();int mask=1<<bit;p.update(down?s.pressed()|mask:s.pressed()&~mask,s.rightX(),s.rightY(),s.leftX(),s.leftY());}}
    public static int buttonBit(int key){return switch(key){case 109->0;case 106->1;case 107->2;case 108->3;case 19->4;case 22->5;case 20->6;case 21->7;case 104->8;case 105->9;case 102->10;case 103->11;case 100->12;case 97->13;case 96->14;case 99->15;default->-1;};}
    public void analog(int player,int stick,int x,int y){if(player<0||player>=pads.length||stick<0||stick>1)return;PadState p=pads[player];synchronized(p){PadState.State s=p.get();p.update(s.pressed(),stick==1?x:s.rightX(),stick==1?y:s.rightY(),stick==0?x:s.leftX(),stick==0?y:s.leftY());}}
    /** Apply [input] settings before the pads are created: the DualShock otherwise always started in
     * digital mode, so sticks only worked in games that switch the pad to analog themselves. Port 2 keeps
     * its existing connection handling. */
    private void configurePads(){
        boolean analog=Boolean.parseBoolean(options.getOrDefault("input.analog_mode_default","false"));
        for(PadState pad:pads)pad.startAnalog=analog;
        pads[0].controllerType=switch(options.getOrDefault("input.controller1_type","AnalogController")){case "DigitalController"->1;case "AnalogJoystick"->2;case "None"->3;default->0;};
    }
    public void resetButtons(){for(PadState pad:pads)synchronized(pad){pad.clear();}}
    private void clearSessionOverrides(){
        aspectOverride=volumeOverride=presentationModeOverride=cropOverride=-1;presentationMultipleOverride=2;
        customAspectOverride=0;fastSpeedOverride=-1;stretchOverride=integerOverride=mutedOverride=wideOverride=null;textureOptions=null;
    }
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
    private synchronized void event(String value){timeline.incident(System.nanoTime(),value);if(events.size()==128)events.removeFirst();events.addLast(android.os.SystemClock.elapsedRealtime()+" "+value);android.util.Log.i("PSXLabBackend",value);journal.event(EngineLog.INFO,"runtime.event",Map.of("message",value));}
    /** No BIOS/RAM/game data or authentication settings are included in a report. */
    public synchronized String diagnosticReport(){
        org.json.JSONObject json=new org.json.JSONObject();try{json.put("schema",1);json.put("backend","PSX Lab ArmSX1 adapter 1");json.put("gameId",lastGameId);json.put("renderer",renderer);json.put("displayRenderer",displayRenderer());org.json.JSONObject settings=new org.json.JSONObject();for(String key:List.of("video.gpu_backend","video.renderer","video.internal_scale","cpu.fast_boot","cpu.execution_mode","console.region"))if(options.containsKey(key))settings.put(key,options.get(key));json.put("options",settings);json.put("biosSha256",bios==null?"":MigrationBackup.digest(bios));json.put("active",active);json.put("paused",blocked());json.put("frames",frames);json.put("guestClocks",guestClocks);json.put("pc",String.format(Locale.ROOT,"%08X",lastPc));json.put("fps",fps);json.put("nominalFps",nominalFps);json.put("audioNonzero",nonzero);json.put("audio",audioDiagnostic());json.put("core",lastCore);json.put("lastError",lastError);json.put("failureCpuContext",lastFailureCpuContext);json.put("nativeTimings",new org.json.JSONObject(Map.of("cpu",lastCpuTiming,"gpu",lastGpuTiming)));json.put("memory",new org.json.JSONObject(Map.of("processRssMb",hostRssMb,"systemAvailableMb",hostAvailableMb,"javaHeapUsedBytes",Runtime.getRuntime().totalMemory()-Runtime.getRuntime().freeMemory(),"nativeHeapAllocatedBytes",android.os.Debug.getNativeHeapAllocatedSize(),"thermalStatus",hostThermalStatus)));json.put("timeline",timelineReport());json.put("events",new org.json.JSONArray(events));json.put("logging",new org.json.JSONObject(journal.summary()));json.put("cheats",new org.json.JSONObject(cheats.diagnostic()));json.put("textures",new org.json.JSONObject(Map.of("status",textureStatus(),"metrics",new org.json.JSONArray(textureMetrics()))));json.put("highRefresh",framePresentationStatus());json.put("shader",ShaderChain.requested()?(gpu!=null?gpu.shaderStatus():ShaderChain.status()):"Off");json.put("display",new org.json.JSONObject(Map.of("width",surfaceWidth,"height",surfaceHeight,"refreshHz",displayRefreshHz,"aspect",outputAspect,"stretch",outputStretch,"integerScaling",outputInteger,"timingDomain","host/API; physical present timestamps unavailable")));return json.toString(2);}catch(org.json.JSONException e){throw new IllegalStateException(e);}
    }
    /** Rows every five seconds and every incident since launch; not limited by the engine log tail. */
    private org.json.JSONObject timelineReport()throws org.json.JSONException{
        org.json.JSONObject value=new org.json.JSONObject();
        value.put("legend","late = frames over 1.5x their paced period; emu = share of wall time running the PS1 core; work = emulation plus frame submission; display shown/skipped = frames presented and frames dropped because the display was still busy; vram reads waited/skipped = VRAM-to-CPU transfers that had to wait for GPU drawing, and those served from the CPU copy because the GPU had not drawn there; cd commands = commands sent (last 12 in hex), with sectors read, XA and CD-audio sectors played; code = where guest code was when emulation slices ended (80000080 is the interrupt vector); io = hardware registers read (r) and written (w) most in the row; gpu ms/s = GPU execution time per wall-clock second for each hardware-renderer stage (draw = game primitives, resolve = HD to native copy, output = screen picture and present, transfer = VRAM fills/copies, upload = CPU writes); near 1000 means the GPU is the bottleneck; draw spans = timed GPU segments, not draw calls; draws/s native/HD = actual raster API calls (a batched call may contain many primitives); timing queries run only while logging is on; frame limit = time the GPU thread waited to keep at most two frames queued; audio min = lowest queued audio; dry = queue emptied and refilled; dsp = audio stretch CPU of one core. Incidents for frames over 50 ms end with why: where the time went, what was unusual against normal frames, and the display stages; slow display frame incidents come from the GPU thread.");
        value.put("incidents",new org.json.JSONArray(timeline.incidents()));value.put("dropped_incidents",timeline.droppedIncidents());
        value.put("rows",new org.json.JSONArray(timeline.rows()));value.put("dropped_rows",timeline.droppedRows());
        return value;
    }
    private void writeReport(){try{if(root!=null){byte[] report=diagnosticReport().getBytes(StandardCharsets.UTF_8);atomic(new File(root,"last-session.json"),report);atomic(new File(journal.directory(),"last-session.json"),report);}}catch(IOException e){android.util.Log.e("PSXLabBackend","Cannot write diagnostic report",e);}}
    private void error(Exception e){lastError=e.toString();journal.error(EngineLog.FATAL,"runtime.error",e);event("error: "+e.getClass().getSimpleName());android.util.Log.e("PSXLabBackend",lastError,e);writeReport();notice.accept(lastError);}
    private static void atomic(File target,byte[] bytes)throws IOException{BackendStateStore.atomic(target,bytes);}
}
