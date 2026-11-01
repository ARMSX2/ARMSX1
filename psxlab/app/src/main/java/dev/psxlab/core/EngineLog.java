package dev.psxlab.core;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicLong;

/** Opt-in, bounded, asynchronous diagnostics. Emulation threads never wait for disk. */
public final class EngineLog implements AutoCloseable {
    public interface Clock { long wall();long cpu();long tid(); }
    public static final int TRACE=0,DEBUG=1,INFO=2,WARN=3,ERROR=4,FATAL=5;
    private final File directory;
    private final Clock clock;
    private final int limit;
    private final AtomicLong dropped=new AtomicLong(),written=new AtomicLong(),sequence=new AtomicLong();
    private final ThreadLocal<Long> frameScope=new ThreadLocal<>();
    private volatile boolean enabled,closing;
    private volatile int level=INFO;
    private volatile String failure="";
    private volatile String session=UUID.randomUUID().toString();
    private volatile Map<String,Object> latestFrame=Map.of();
    private final AtomicLong previousCompletion=new AtomicLong();
    private volatile ArrayBlockingQueue<Object> queue;
    private final Map<String,Object> threads=new ConcurrentHashMap<>();
    private Thread worker;
    public EngineLog(File directory,Clock clock){this(directory,clock,4*1024*1024);}
    public EngineLog(File directory,Clock clock,int limit){if(limit<16384)throw new IllegalArgumentException("Log limit");this.directory=directory;this.clock=clock;this.limit=limit;}
    public boolean enabled(){return enabled;}
    public boolean accepts(int severity){return enabled&&severity>=level;}
    public void startSession(String gameId){session=UUID.randomUUID().toString();previousCompletion.set(0);latestFrame=Map.of();event(INFO,"session.launch",Map.of("game_id",gameId));}
    public File directory(){return directory;}
    public synchronized void configure(boolean value,int severity){
        if(closing)return;
        level=Math.max(TRACE,Math.min(5,severity));enabled=value;
        if(value&&worker==null){queue=new ArrayBlockingQueue<>(2048);worker=new Thread(this::writeLoop,"PSX-LogWriter");worker.setDaemon(true);worker.start();}
        if(value)event(INFO,"logging.enabled",Map.of("level",level));else if(queue!=null)queue.offer(new Flush(null));
    }
    private void registerThread(){String name=Thread.currentThread().getName();long id=Thread.currentThread().getId();Object previous=threads.get(name);if(previous instanceof Map<?,?> map&&Objects.equals(map.get("java_thread_id"),id))return;threads.put(name,Map.of("tid",clock.tid(),"java_thread_id",id));}
    public long currentFrame(){Long id=frameScope.get();return id==null?0:id;}
    public void enterFrame(long id){if(enabled)frameScope.set(id);}
    public void leaveFrame(){frameScope.remove();}
    private Map<String,Object> envelope(String type,String path,long frame){
        LinkedHashMap<String,Object> value=new LinkedHashMap<>();value.put("session",session);value.put("type",type);value.put("path",path);value.put("source",source(path));value.put("time_ns",clock.wall());value.put("frame",frame);
        String name=Thread.currentThread().getName();long tid=clock.tid();value.put("thread",name);value.put("tid",tid);value.put("java_thread_id",Thread.currentThread().getId());threads.put(name,Map.of("tid",tid,"java_thread_id",Thread.currentThread().getId()));return value;
    }
    private void submit(int severity,Map<String,Object> value){if(!enabled||severity<level)return;value.put("severity",severity);ArrayBlockingQueue<Object> target=queue;if(target==null||!target.offer(value))dropped.incrementAndGet();}
    public void event(int severity,String path,Map<String,?> fields){if(!enabled||severity<level)return;Map<String,Object> row=envelope("event",path,currentFrame());row.putAll(fields);submit(severity,row);}
    public void error(String path,Throwable error){error(ERROR,path,error);}
    public void error(int severity,String path,Throwable error){if(!enabled||severity<level)return;StringWriter text=new StringWriter();error.printStackTrace(new PrintWriter(text));event(severity,path,Map.of("error",error.toString(),"stack",text.toString()));}
    public Span begin(String path,int severity){return !enabled||severity<level?null:new Span(path,severity,0,null);}
    public Span watch(String path,int severity,long slowNanos){return !enabled?null:new Span(path,severity,slowNanos,null);}
    public final class Span implements AutoCloseable {
        private final String path;private final int severity;private final long start,cpuStart,frame,slow;private final Frame owner;
        private boolean done;
        private Span(String path,int severity,long slow,Frame owner){this.path=path;this.severity=severity;this.slow=slow;this.owner=owner;registerThread();frame=currentFrame();start=clock.wall();cpuStart=clock.cpu();}
        @Override public void close(){if(done)return;done=true;long wall=Math.max(0,clock.wall()-start),cpu=Math.max(0,clock.cpu()-cpuStart);
            if(owner!=null){owner.stages.add(Map.of("path",path,"source",source(path),"wall_ns",wall,"cpu_ns",cpu));return;}
            int actual=slow>0&&wall>=slow?Math.max(WARN,severity):severity;
            if(actual<level)return;Map<String,Object> row=envelope("span",path,frame);row.put("wall_ns",wall);row.put("cpu_ns",cpu);submit(actual,row);
        }
    }
    public Frame frame(long guestClocks,int pc){if(!enabled)return null;Frame result=new Frame(sequence.incrementAndGet(),guestClocks,pc);enterFrame(result.id);return result;}
    public final class Frame {
        public final long id;private final long start,cpuStart,guestStart;private final int pc;
        private final Map<String,Object> values;private final List<Map<String,Object>> stages=new ArrayList<>();
        private long queued,gpuStart,gpuCpuStart,cpuWall,sleepRequested,sleepActual,budget=16_715_113L;
        private boolean complete;
        private Frame(long id,long guest,int pc){this.id=id;guestStart=guest;this.pc=pc;start=clock.wall();cpuStart=clock.cpu();values=envelope("frame","frame.complete",id);}
        public void cpuDone(long guest,int endPc){cpuWall=Math.max(0,clock.wall()-start);values.put("cpu_wall_ns",cpuWall);values.put("cpu_active_ns",Math.max(0,clock.cpu()-cpuStart));values.put("guest_clocks_start",guestStart);values.put("guest_clocks",guest);values.put("pc_start",Integer.toHexString(pc));values.put("pc",Integer.toHexString(endPc));}
        public void pacing(long requested,long actual,long buffered){sleepRequested=requested;sleepActual=actual;values.put("sleep_requested_ns",requested);values.put("sleep_actual_ns",actual);values.put("sleep_overshoot_ns",Math.max(0,actual-requested));values.put("audio_buffered_frames",buffered);}
        public void queued(){queued=clock.wall();}
        public void gpuBegin(String renderer,int[] state){gpuStart=clock.wall();gpuCpuStart=clock.cpu();enterFrame(id);values.put("gpu_thread",Thread.currentThread().getName());values.put("gpu_tid",clock.tid());threads.put(Thread.currentThread().getName(),Map.of("tid",clock.tid(),"java_thread_id",Thread.currentThread().getId()));values.put("renderer",renderer);values.put("gpu_status",Integer.toHexString(state[0]));values.put("display",state.clone());budget=(state[0]&0x100000)!=0?20_000_000L:16_715_113L;}
        public Span stage(String path){return enabled?new Span(path,INFO,0,this):null;}
        public void presentationSequence(String mode,int count){values.put("presentation_mode",mode);values.put("requested_presentations_per_field",count);}
        public void extraPresentation(int phase,boolean accepted){values.put("last_extra_phase",phase);values.put("last_extra_api_accepted",accepted);}
        public void presentation(boolean accepted){values.put("presentation_api_accepted",accepted);values.put("presentation_timing_domain","host/API completion, not physical scanout");}
        public void complete(){if(complete)return;complete=true;long end=clock.wall(),queueWait=Math.max(0,gpuStart-queued),gpuWall=Math.max(0,end-gpuStart);long previous=previousCompletion.getAndSet(end);values.put("completion_interval_ns",previous==0?0:Math.max(0,end-previous));values.put("completed_ns",end);values.put("gpu_queue_ns",queueWait);values.put("gpu_wall_ns",gpuWall);values.put("gpu_active_ns",Math.max(0,clock.cpu()-gpuCpuStart));values.put("stages",stages);values.put("budget_ns",budget);
            String largest="cpu.emulate";long largestTime=cpuWall;if(queueWait>largestTime){largest="gpu.queue";largestTime=queueWait;}
            long overshoot=Math.max(0,sleepActual-sleepRequested);if(overshoot>largestTime){largest="pacing.sleep_overshoot";largestTime=overshoot;}
            for(Map<String,Object> stage:stages){long n=((Number)stage.get("wall_ns")).longValue();if(n>largestTime){largestTime=n;largest=stage.get("path").toString();}}
            values.put("largest_phase",largest);values.put("largest_phase_ns",largestTime);values.put("interpretation","largest measured phase is a candidate, not proof; API wall time includes waits");latestFrame=Collections.unmodifiableMap(new LinkedHashMap<>(values));submit(largestTime>budget*3/2?WARN:INFO,values);leaveFrame();
        }
    }
    public Map<String,Object> summary(){Map<String,Object> value=new LinkedHashMap<>();value.put("session",session);value.put("enabled",enabled);value.put("level",level);value.put("written_records",written.get());value.put("dropped_records",dropped.get());value.put("writer_error",failure);value.put("threads",new TreeMap<>(threads));value.put("latest_frame",latestFrame);return value;}
    private record Flush(CompletableFuture<Boolean> done){}
    public boolean flush(long timeoutMillis){ArrayBlockingQueue<Object> target=queue;if(target==null)return true;CompletableFuture<Boolean> done=new CompletableFuture<>();if(!target.offer(new Flush(done)))return false;try{return done.get(timeoutMillis,TimeUnit.MILLISECONDS);}catch(Exception e){return false;}}
    @Override public void close(){enabled=false;closing=true;ArrayBlockingQueue<Object> target=queue;if(target!=null)target.offer(new Flush(null));if(worker!=null)try{worker.join(2000);}catch(InterruptedException e){Thread.currentThread().interrupt();}}
    private void writeLoop(){OutputStream out=null;long size=0,lastFlush=0,retryAfter=0;File current=new File(directory,"engine-current.jsonl"),previous=new File(directory,"engine-previous.jsonl");
        registerThread();try{while(!closing||!queue.isEmpty()){
            Object item;try{item=queue.take();}catch(InterruptedException e){Thread.currentThread().interrupt();break;}
            if(item instanceof Flush flush){try{if(out!=null)out.flush();if(!enabled&&out!=null){out.close();out=null;}if(flush.done()!=null)flush.done().complete(failure.isEmpty());}catch(IOException e){failure=e.toString();if(flush.done()!=null)flush.done().complete(false);}continue;}
            long now=System.nanoTime();if(now<retryAfter){dropped.incrementAndGet();continue;}
            try{
                if(out==null){if(!directory.isDirectory()&&!directory.mkdirs())throw new IOException("Cannot create diagnostic directory");size=current.length();out=new BufferedOutputStream(new FileOutputStream(current,true));}
                byte[] bytes=(json(item)+"\n").getBytes(StandardCharsets.UTF_8);
                if(bytes.length>limit){dropped.incrementAndGet();continue;}
                if(size+bytes.length>limit){out.close();out=null;Files.move(current.toPath(),previous.toPath(),StandardCopyOption.REPLACE_EXISTING);out=new BufferedOutputStream(new FileOutputStream(current));size=0;}
                out.write(bytes);size+=bytes.length;written.incrementAndGet();failure="";
                if(now-lastFlush>=1_000_000_000L){out.flush();lastFlush=now;}
                if(!enabled&&queue.isEmpty()){out.close();out=null;}
            }catch(IOException|RuntimeException e){failure=e.toString();dropped.incrementAndGet();retryAfter=now+5_000_000_000L;if(out!=null)try{out.close();}catch(IOException ignored){}out=null;}
        }}finally{if(out!=null)try{out.close();}catch(IOException ignored){}}
    }
    public static String json(Object value){StringBuilder out=new StringBuilder();append(out,value);return out.toString();}
    private static void append(StringBuilder out,Object value){
        if(value==null){out.append("null");return;}if(value instanceof Boolean||value instanceof Number){if(value instanceof Double d&&!Double.isFinite(d)||value instanceof Float f&&!Float.isFinite(f))out.append("null");else out.append(value);return;}
        if(value instanceof Map<?,?> map){out.append('{');boolean first=true;for(var entry:map.entrySet()){if(!first)out.append(',');first=false;append(out,entry.getKey().toString());out.append(':');append(out,entry.getValue());}out.append('}');return;}
        if(value instanceof Iterable<?> list){out.append('[');boolean first=true;for(Object entry:list){if(!first)out.append(',');first=false;append(out,entry);}out.append(']');return;}
        if(value instanceof int[] array){out.append('[');for(int i=0;i<array.length;i++){if(i>0)out.append(',');out.append(array[i]);}out.append(']');return;}
        String text=value.toString();out.append('"');for(int i=0;i<Math.min(text.length(),8192);i++){char c=text.charAt(i);switch(c){case '"'->out.append("\\\"");case '\\'->out.append("\\\\");case '\n'->out.append("\\n");case '\r'->out.append("\\r");case '\t'->out.append("\\t");default->{if(c<32)out.append(String.format(Locale.ROOT,"\\u%04x",(int)c));else out.append(c);}}}out.append('"');
    }
    public static String source(String path){
        if(path.equals("audio.guest.snapshot"))return "psxlab/app/src/main/java/dev/psxlab/ArmSxBackend.java#guestAudioSnapshot";
        if(path.startsWith("audio.guest.spu"))return "psxlab/app/src/main/java/dev/psxlab/core/Spu.java#write/loggingSnapshot";
        if(path.startsWith("audio.guest.cd"))return "psxlab/app/src/main/java/dev/psxlab/core/CdRom.java#process/tick/audioFrame";
        if(path.startsWith("cheats."))return "psxlab/app/src/main/java/dev/psxlab/core/Cheats.java#load/arm/apply";
        if(path.startsWith("texture."))return "psxlab/app/src/main/java/dev/psxlab/core/TexturePacks.java#upload/decode/dump";
        if(path.equals("cpu.compile"))return "psxlab/app/src/main/cpp/cpu_arm64.cpp#compileArm64";
        if(path.equals("cpu.cache.check"))return "psxlab/app/src/main/cpp/cpu_jni.cpp#CpuCore::revalidate";
        if(path.equals("cpu.cache.clear"))return "psxlab/app/src/main/cpp/cpu_jni.cpp#CpuCore::clearCache";
        if(path.equals("cpu.block.build"))return "psxlab/app/src/main/cpp/cpu_jni.cpp#CpuCore::findBlock";
        if(path.equals("gpu.raster.dispatch"))return "psxlab/app/src/main/cpp/hardware_gpu.cpp#HardwareGpu::flush";
        if(path.equals("gpu.vram.transfer"))return "psxlab/app/src/main/cpp/hardware_gpu.cpp#HardwareGpu::transfer";
        if(path.equals("gpu.vram.readback"))return "psxlab/app/src/main/cpp/hardware_gpu.cpp#HardwareGpu::download";
        if(path.equals("gpu.vram.resolve"))return "psxlab/app/src/main/cpp/hardware_gpu.cpp#HardwareGpu::resolveNative";
        if(path.equals("gpu.scanout.native"))return "psxlab/app/src/main/cpp/hardware_gpu.cpp#HardwareGpu::renderShared";
        if(path.equals("gpu.present.gles.api"))return "psxlab/app/src/main/cpp/hardware_gpu.cpp#HardwareGpu::present";
        if(path.equals("gpu.commands.upload"))return "psxlab/app/src/main/cpp/hardware_gpu.cpp#HardwareGpu::streamCommands";
        if(path.equals("gpu.present.gles.window"))return "psxlab/app/src/main/cpp/hardware_gpu.cpp#HardwareGpu::beginWindow";
        if(path.equals("gpu.present.gles.swap"))return "psxlab/app/src/main/cpp/hardware_gpu.cpp#HardwareGpu::swapWindow";
        if(path.equals("gpu.sync.producer"))return "psxlab/app/src/main/cpp/hardware_gpu.cpp#HardwareGpu::renderShared/glFinish";
        if(path.equals("gpu.vulkan.fence_wait"))return "psxlab/app/src/main/cpp/vulkan_output.cpp#Output::waitFence";
        if(path.equals("gpu.vulkan.acquire"))return "psxlab/app/src/main/cpp/vulkan_output.cpp#Output::acquireImage";
        if(path.equals("gpu.vulkan.submit"))return "psxlab/app/src/main/cpp/vulkan_output.cpp#Output::submit";
        if(path.equals("gpu.vulkan.present"))return "psxlab/app/src/main/cpp/vulkan_output.cpp#Output::presentImage";
        if(path.equals("gpu.memory.staging_allocate"))return "psxlab/app/src/main/cpp/vulkan_output.cpp#Output::ensureStaging";

        if(path.startsWith("dma.gpu.linked"))return "psxlab/app/src/main/java/dev/psxlab/core/Dma.java#transfer/tick";
        if(path.equals("gpu.present.bfi"))return "psxlab/app/src/main/cpp/vulkan_output.cpp#Output::presentStored or hardware_gpu.cpp#HardwareGpu::presentBlack";
        if(path.equals("gpu.present.shader_subframe"))return "psxlab/app/src/main/cpp/shader_chain.cpp#armsx_shader_vk_render + vulkan_output.cpp#Output::presentStored";
        if(path.equals("presentation.mode"))return "psxlab/app/src/main/java/dev/psxlab/core/PresentationPolicy.java#evaluate";
        if(path.startsWith("gpu.raster"))return "psxlab/app/src/main/java/dev/psxlab/core/Gpu.java#flushDrawBatch";
        if(path.startsWith("gpu.present.vulkan"))return "psxlab/app/src/main/cpp/vulkan_output.cpp#Output::presentOnce/presentSharedOnce";
        if(path.startsWith("gpu.present.gles"))return "psxlab/app/src/main/cpp/hardware_gpu.cpp#HardwareGpu::present";
        if(path.startsWith("gpu"))return "psxlab/app/src/main/java/dev/psxlab/core/Gpu.java#requestSurfaceFrame/sync";
        if(path.equals("cd.cue.mounted")||path.equals("cd.pbp.mounted"))return "psxlab/app/src/main/java/dev/psxlab/ArmSxBackend.java#openMedia";
        if(path.startsWith("cd.chd"))return "psxlab/app/src/main/java/dev/psxlab/ChdSource.java#read";
        if(path.startsWith("cd"))return "psxlab/app/src/main/java/dev/psxlab/FileSource.java#read";
        if(path.startsWith("pacing"))return "psxlab/app/src/main/java/dev/psxlab/core/AudioAwareFramePacer.java#waitNanos";
        if(path.startsWith("audio"))return "psxlab/app/src/main/java/dev/psxlab/AudioOutput.java#process/play";
        return "psxlab/app/src/main/java/dev/psxlab/ArmSxBackend.java#loop";
    }
}
