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
    private volatile float fps;
    private volatile float nominalFps=60;
    private volatile long frames,nonzero,guestClocks;
    private volatile int lastPc;
    private volatile String lastAudio="Not started";
    private volatile String lastCore="Not started";
    private String biosPin;
    private volatile String renderer="Stopped",lastError="",gamePath="",discPath="";
    private File root,biosFolder,config;
    private byte[] bios;
    private Cpu cpu;
    private Bus bus;
    private Gpu gpu;
    private Media media,pendingMedia;
    private long insertAt;
    private Restore restart;
    private final ArrayDeque<String> events=new ArrayDeque<>();
    private Map<String,String> options=Map.of();
    private record Restore(byte[] state,String disc,CompletableFuture<Boolean> ready) {}
    private static final class Media implements AutoCloseable {
        final ByteSource source; final IsoDisc disc; final byte[] executable;
        Media(ByteSource source)throws IOException{
            this.source=source;
            if(new String(source.read(0,8),StandardCharsets.US_ASCII).equals("PS-X EXE")){
                disc=null;byte[] header=source.read(0,2048);int size=ExeLoader.le(header,28);
                if(size<4||size>2*1024*1024)throw new IOException("Invalid executable size");executable=source.read(0,size+2048);
            }else{disc=new IsoDisc(source);executable=disc.bootExecutable();}
        }
        public void close()throws IOException{source.close();}
    }
    public ArmSxBackend(Context context,Consumer<Boolean> pauseCallback,Consumer<String> notice){
        this.context=context.getApplicationContext();this.pauseCallback=pauseCallback;this.notice=notice;
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
            lastError="";return true;
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
            try(FileSource probe=new FileSource(new ParcelFileDescriptor.AutoCloseInputStream(ParcelFileDescriptor.dup(fd.getFileDescriptor())))){
                if(new String(probe.read(0,8),StandardCharsets.US_ASCII).equals("MComprHD"))return new ChdSource(fd);
            }
            return new FileSource(new ParcelFileDescriptor.AutoCloseInputStream(ParcelFileDescriptor.dup(fd.getFileDescriptor())));
        }
    }
    private Media openMedia(String path)throws IOException{
        ByteSource source=source(path);try{return new Media(source);}catch(Exception e){source.close();throw e;}
    }
    /** Blocking, as ArmSX1's existing runVMThread contract requires. */
    public boolean run(String path){return run(path, () -> true);}
    /** The host's session cancellation and this active/stop transition share this monitor. */
    public boolean run(String path,java.util.function.BooleanSupplier allowed){
        synchronized(this){if(!allowed.getAsBoolean()||active||root==null)return false;active=true;stop=false;paused=false;frames=nonzero=guestClocks=0;gamePath=discPath=path;lastError="";}
        boolean booted=false;Restore restore=null;startupTiming.reset();event("launch");
        try{
            options=readConfig();bios=readBios();
            do{
                restart=null;
                int scale=Math.max(1,Math.min(5,Integer.parseInt(options.getOrDefault("video.internal_scale","1"))));
                String selected=options.getOrDefault("video.renderer","software");int mode=selected.equals("vulkan")?2:selected.equals("software")?0:1;
                // Keep one graphics API attached to a retained Android Surface. Software
                // still rasterizes on the CPU; its finished pixels use the texture presenter.
                try(Gpu graphics=mode==0&&scale==1?new Gpu(true,false):new Gpu(true,mode,scale)){
                    gpu=graphics;clearSurface();bus=new Bus(gpu,pads[0]);bus.sio0.setPad(1,pads[1]);bus.loadBios(bios);
                    try(CardStorage cards=new CardStorage(new File(root,"cards"),bus)){
                        String mounted=restore==null?discPath:restore.disc();media=mounted.isEmpty()?null:openMedia(mounted);if(media!=null&&media.disc!=null)bus.cdrom.insert(media.disc);
                        try{
                            cpu=new Cpu(bus);cpu.reset();
                            if(restore!=null&&restore.state()!=null)SaveState.restore(restore.state(),cpu,bus,gpu,gamePath);
                            else if(media!=null&&(media.disc==null||Boolean.parseBoolean(options.getOrDefault("cpu.fast_boot","false")))){cpu=BiosBoot.initialize(bios,bus,()->stop);bus.cdrom.prepareBootHandoff();ExeLoader.loadInto(media.executable,bus,cpu);}
                            cpu.enableNative(true,true,true);
                            try(AudioOutput output=new AudioOutput(true,Boolean.parseBoolean(options.getOrDefault("diagnostics.audio_capture","false")),context)){
                                audio=output;output.setPaused(blocked());bus.spu.setAudioSink((pcm,count)->{for(int i=0;i<count;i++)if(pcm[i]!=0)nonzero++;output.samples(pcm,count);});
                                renderer="PSX Lab / "+gpu.renderer();booted=true;
                                if(restore!=null){discPath=restore.disc();restore.ready().complete(true);}pauseCallback.accept(paused);event("ready");
                                try{loop(output);}finally{try{lastAudio=output.diagnostic();}catch(IllegalStateException ignored){}}
                            }finally{audio=null;bus.spu.setAudioSink(null);cpu.closeNative();}
                        }finally{if(media!=null){media.close();media=null;}if(pendingMedia!=null){pendingMedia.close();pendingMedia=null;}}
                    }finally{clearSurface();}
                }finally{gpu=null;bus=null;cpu=null;}
                restore=restart;
            }while(!stop&&restore!=null);
        }catch(Exception e){booted=false;error(e);if(restore!=null)restore.ready().complete(false);if(restart!=null)restart.ready().complete(false);}
        finally{resetButtons();fps=0;renderer="Stopped";synchronized(this){active=false;}Runnable job;while((job=commands.poll())!=null)job.run();event("stopped");writeReport();gamePath=discPath="";}
        return booted;
    }
    private boolean blocked(){Surface target=surface;return paused||audioSuspended||presentationSuspended||target==null||!target.isValid();}
    private void loop(AudioOutput output)throws Exception{
        AudioAwareFramePacer pacer=new AudioAwareFramePacer(cpu.instructions,System.nanoTime());long last=gpu.vblanks,measured=System.nanoTime(),measuredFrames=frames;boolean wasBlocked=false,wasFast=false;
        while(!stop&&restart==null){
            Runnable job;while((job=commands.poll())!=null)job.run();if(stop||restart!=null)break;
            if(blocked()){
                output.setPaused(true);wasBlocked=true;fps=0;synchronized(gate){gate.wait(10);}continue;
            }
            if(wasBlocked){output.setPaused(fast);pacer=new AudioAwareFramePacer(cpu.instructions,System.nanoTime());measured=System.nanoTime();measuredFrames=frames;wasBlocked=false;}
            if(fast!=wasFast){bus.spu.flushAudio();bus.spu.setAudioSink(fast?null:(pcm,count)->{for(int i=0;i<count;i++)if(pcm[i]!=0)nonzero++;output.samples(pcm,count);});output.setPaused(fast);pacer=new AudioAwareFramePacer(cpu.instructions,System.nanoTime());wasFast=fast;}
            if(pendingMedia!=null&&cpu.instructions>=insertAt){bus.cdrom.insert(pendingMedia.disc);if(media!=null)media.close();media=pendingMedia;pendingMedia=null;}
            cpu.runToFrame(20000);guestClocks=cpu.instructions;lastPc=cpu.pc;
            if(gpu.vblanks==last)continue;last=gpu.vblanks;bus.flushClocks();bus.spu.flushAudio();
            long wait=fast?0:pacer.waitNanos(cpu.instructions,System.nanoTime(),output.bufferedFrames());if(wait>0)TimeUnit.NANOSECONDS.sleep(wait);
            Surface target=surface;
            if(target!=null&&target.isValid()){
                boolean trace=traceStartup;long requested=trace?System.nanoTime():0,clock=cpu.instructions;
                gpu.requestSurfaceFrame(target,true,frame->{if(frame!=null)draw(target,frame);frames++;if(trace)startupTiming.record(clock,requested,System.nanoTime());});
            }
            long now=System.nanoTime();if(now-measured>=500000000L){fps=(float)((frames-measuredFrames)*1e9/(now-measured));nominalFps=videoRate(gpu.status());lastAudio=output.diagnostic();lastCore=cpu.executionDiagnostic()+"\n"+bus.cdrom.diagnostic()+"\n"+bus.dma.diagnostic()+"\n"+bus.spu.diagnostic();measured=now;measuredFrames=frames;}
        }
    }
    private void clearSurface(){Surface target=surface;if(target==null||!target.isValid()||gpu==null)return;gpu.control(0x03000001);gpu.requestSurfaceFrame(target,false,frame->{if(frame!=null)draw(target,frame);});gpu.sync();}
    private void draw(Surface target,Gpu.Frame frame){
        if(surface!=target||!target.isValid())return;Bitmap bitmap=Bitmap.createBitmap(frame.pixels(),frame.width(),frame.height(),Bitmap.Config.ARGB_8888);
        try{Canvas canvas=target.lockCanvas(null);try{canvas.drawColor(Color.BLACK);float width=Math.min(canvas.getWidth(),canvas.getHeight()*4f/3),height=width*3/4;float x=(canvas.getWidth()-width)/2,y=(canvas.getHeight()-height)/2;canvas.drawBitmap(bitmap,null,new RectF(x,y,x+width,y+height),new Paint(Paint.FILTER_BITMAP_FLAG));}finally{target.unlockCanvasAndPost(canvas);}}catch(IllegalArgumentException|IllegalStateException ignored){}finally{bitmap.recycle();}
    }
    public void surface(Surface value){surface=value;if(value==null)resetButtons();event(value==null?"surface-lost":"surface-attached");wake();}
    public void pause(boolean value){paused=value;if(value)resetButtons();AudioOutput out=audio;if(out!=null)out.setPaused(blocked()||fast);pauseCallback.accept(value);event(value?"pause":"resume");wake();}
    public void suspendAudio(boolean value){audioSuspended=value;event("audio-suspended="+value);wake();}
    public void suspendPresentation(boolean value){presentationSuspended=value;event("presentation-suspended="+value);wake();}
    public void stop(){stop=true;resetButtons();AudioOutput out=audio;if(out!=null)out.close();wake();}
    public void fastForward(boolean value){fast=value;wake();}
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
    public long guestClocks(){return guestClocks;}public String renderer(){return renderer;}public String lastError(){return lastError;}
    public String audioDiagnostic(){AudioOutput out=audio;try{return out==null?lastAudio:out.diagnostic();}catch(IllegalStateException ignored){return lastAudio;}}
    private synchronized void event(String value){if(events.size()==128)events.removeFirst();events.addLast(android.os.SystemClock.elapsedRealtime()+" "+value);android.util.Log.i("PSXLabBackend",value);}
    /** No BIOS/RAM/game data or authentication settings are included in a report. */
    public synchronized String diagnosticReport(){
        org.json.JSONObject json=new org.json.JSONObject();try{json.put("schema",1);json.put("backend","PSX Lab ArmSX1 adapter 1");json.put("gameId",MigrationBackup.digest(gamePath.getBytes(StandardCharsets.UTF_8)));json.put("renderer",renderer);org.json.JSONObject settings=new org.json.JSONObject();for(String key:List.of("video.renderer","video.internal_scale","cpu.fast_boot","cpu.execution_mode","console.region"))if(options.containsKey(key))settings.put(key,options.get(key));json.put("options",settings);json.put("biosSha256",bios==null?"":MigrationBackup.digest(bios));json.put("active",active);json.put("paused",blocked());json.put("frames",frames);json.put("guestClocks",guestClocks);json.put("pc",String.format(Locale.ROOT,"%08X",lastPc));json.put("fps",fps);json.put("nominalFps",nominalFps);json.put("audioNonzero",nonzero);json.put("audio",audioDiagnostic());json.put("core",lastCore);json.put("lastError",lastError);json.put("events",new org.json.JSONArray(events));return json.toString(2);}catch(org.json.JSONException e){throw new IllegalStateException(e);}
    }
    private void writeReport(){try{if(root!=null)atomic(new File(root,"last-session.json"),diagnosticReport().getBytes(StandardCharsets.UTF_8));}catch(IOException e){android.util.Log.e("PSXLabBackend","Cannot write diagnostic report",e);}}
    private void error(Exception e){lastError=e.toString();event("error: "+e.getClass().getSimpleName());android.util.Log.e("PSXLabBackend",lastError,e);writeReport();notice.accept(lastError);}
    private static void atomic(File target,byte[] bytes)throws IOException{BackendStateStore.atomic(target,bytes);}
}
