package dev.psxlab;

import android.media.*;
import android.os.Process;
import dev.psxlab.core.Spu;
import dev.psxlab.core.EngineLog;
import dev.psxlab.core.AudioRateController;
import dev.psxlab.core.AudioClockPacer;
import dev.psxlab.core.AudioTimeline;

/** Bounded stereo PCM queue. Only the audio thread writes to Android's AudioTrack. */
final class AudioOutput implements AutoCloseable,Spu.AudioSink {
    private final AudioTrack track;
    private volatile EngineLog journal;
    private int loggedUnderruns;
    private final int configuredRate,configuredBuffer,configuredPerformance;
    private final String nativeOutputRate;
    void setJournal(EngineLog value){journal=value;}
    private final boolean lockTempo;
    private final AudioCapture capture;
    private final short[] queue=new short[16384],inputQueue=new short[16384];
    private final AudioRateController rateControl=new AudioRateController();
    private final AudioTimeline timeline=new AudioTimeline();
    private final AudioTimestamp timestamp=new AudioTimestamp();
    private final Object presentationLock=new Object();
    private long audibleSource,lastTimestamp=-1,lastClockRead;
    private volatile long timestampAfter;
    private NativeTempo tempo;
    private final short[] stretched=new short[4096];
    volatile long sourceFrames;
    private volatile long dspCpuNanos,dspInputFrames,sourceBlockedNanos;
    private final Thread worker,dspWorker;
    private int head,size,inFlightFrames,inputHead,inputSize;
    private boolean inputFinished,dspFinished;
    private volatile boolean running=true,paused;
    private boolean trackReleased;
    private int stoppedPlayed,stoppedUnderruns;
    private String lastDiagnostic="Audio starting";
    private volatile RuntimeException failure;
    private volatile float appliedSpeed=1f,tempoFactor=1f,configuredGain=1f;
    void setTempoFactor(float value){float next=Float.isFinite(value)?Math.max(1,Math.min(16,value)):1;tempoFactor=next;appliedSpeed=next;}
    synchronized void setGain(float gain){if(!trackReleased){configuredGain=Math.max(0,Math.min(1,gain));track.setVolume(configuredGain);}}
    volatile long writtenFrames,droppedFrames;
    volatile long recoveries,rateChanges;
    private volatile long maxProducerGapNanos,lastProducedNanos;
    private long productionAt,productionFrames;
    private double productionRate=1;
    AudioOutput(){this(false);}
    AudioOutput(boolean lockTempo){this(lockTempo,false);}
    AudioOutput(boolean lockTempo,boolean record){this(lockTempo,record,null);}
    AudioOutput(boolean lockTempo,boolean record,android.content.Context context){
        this.lockTempo=lockTempo;
        capture=record?(context==null?new AudioCapture():new AudioCapture(context)):null;
        int minimum=AudioTrack.getMinBufferSize(Spu.SAMPLE_RATE,AudioFormat.CHANNEL_OUT_STEREO,AudioFormat.ENCODING_PCM_16BIT);
        if(minimum<0)throw new IllegalStateException("44.1 kHz stereo audio is unavailable");
        track=new AudioTrack.Builder().setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
            .setAudioFormat(new AudioFormat.Builder().setSampleRate(Spu.SAMPLE_RATE).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
            // Default routing may select a deep-buffer output (about 250 ms on
            // Thor). Keep our jitter reserve, but request the interactive route.
            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
            .setTransferMode(AudioTrack.MODE_STREAM).setBufferSizeInBytes(Math.max(minimum*2,17640)).build();
        if(track.getState()!=AudioTrack.STATE_INITIALIZED){track.release();throw new IllegalStateException("Cannot initialize Android audio");}
        if(android.os.Build.VERSION.SDK_INT>=31)track.setStartThresholdInFrames(Math.min(1024,track.getBufferSizeInFrames()));
        configuredRate=track.getSampleRate();configuredBuffer=track.getBufferSizeInFrames();configuredPerformance=track.getPerformanceMode();
        android.media.AudioManager manager=context==null?null:(android.media.AudioManager)context.getSystemService(android.content.Context.AUDIO_SERVICE);
        nativeOutputRate=manager==null?"unknown":String.valueOf(manager.getProperty(android.media.AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE));
        tempo=new NativeTempo();worker=new Thread(this::play,"PSX-Audio");dspWorker=new Thread(this::process,"PSX-DSP");worker.start();dspWorker.start();
    }
    @Override public synchronized void samples(short[] pcm,int count){
        if(failure!=null)throw failure;
        if(!running)return;
        long now=System.nanoTime(),gap=lastProducedNanos==0?0:now-lastProducedNanos;if(lastProducedNanos!=0)maxProducerGapNanos=Math.max(maxProducerGapNanos,gap);lastProducedNanos=now;
        if(journal!=null&&journal.enabled()&&gap>30_000_000L)journal.event(EngineLog.WARN,"audio.producer_gap",java.util.Map.of("gap_ns",gap,"source_frames",sourceFrames,"input_queue_frames",inputSize/2,"ready_frames",size/2));
        if(count<0 || count>pcm.length || (count&1)!=0)throw new IllegalArgumentException("Stereo PCM length");
        if(inputFinished)throw new IllegalStateException("Audio input already finished");
        if(capture!=null)capture.source(pcm,count);
        sourceFrames+=count/2;
        int offset=0;
        while(offset<count && running){
            while(inputSize==inputQueue.length && running){long before=System.nanoTime();try{wait();sourceBlockedNanos+=System.nanoTime()-before;}catch(InterruptedException e){Thread.currentThread().interrupt();return;}}
            if(failure!=null)throw failure;if(!running)return;
            int tail=(inputHead+inputSize)%inputQueue.length;
            int n=Math.min(count-offset,Math.min(inputQueue.length-inputSize,inputQueue.length-tail));
            System.arraycopy(pcm,offset,inputQueue,tail,n);inputSize+=n;offset+=n;notifyAll();
        }
    }
    synchronized void finishInput(){
        inputFinished=true;notifyAll();
        while(running && !dspFinished){try{wait();}catch(InterruptedException e){Thread.currentThread().interrupt();return;}}
        if(failure!=null)throw failure;
    }
    private void process(){
        Process.setThreadPriority(Process.THREAD_PRIORITY_DISPLAY);
        short[] block=new short[1024];
        try{
            while(running){
                int count;
                synchronized(this){
                    while(inputSize==0 && !inputFinished && running)wait();
                    if(!running)return;
                    if(inputSize==0 && inputFinished)break;
                    count=Math.min(inputSize,block.length);
                    int first=Math.min(count,inputQueue.length-inputHead);
                    System.arraycopy(inputQueue,inputHead,block,0,first);
                    if(first<count)System.arraycopy(inputQueue,0,block,first,count-first);
                    inputHead=(inputHead+count)%inputQueue.length;inputSize-=count;notifyAll();
                }
                // Native search work runs without holding either queue's monitor.
                long workStarted=android.os.Debug.threadCpuTimeNanos();
                try(EngineLog.Span span=journal==null?null:journal.watch("audio.dsp.process",EngineLog.TRACE,8_000_000L)){float speed=appliedSpeed;timeline.feed(count/2,speed);tempo.feed(block,count,speed);drainTempo();}
                dspCpuNanos+=android.os.Debug.threadCpuTimeNanos()-workStarted;dspInputFrames+=count/2;
            }
            if(running){tempo.finish();drainTempo();}
        }catch(InterruptedException e){Thread.currentThread().interrupt();}
        catch(RuntimeException e){if(running){if(journal!=null)journal.error("audio.dsp.error",e);failure=e;running=false;try{track.pause();}catch(IllegalStateException ignored){}}}
        finally{tempo.close();synchronized(this){dspFinished=true;notifyAll();}}
    }
    private void drainTempo(){int count;while(running && (count=tempo.receive(stretched))>0){if(capture!=null)capture.processed(stretched,count);enqueue(stretched,count);}}
    private synchronized void enqueue(short[] pcm,int count){
        for(int i=0;i<count;i+=2){
            while(size==queue.length && running){notifyAll();try{wait();}catch(InterruptedException e){Thread.currentThread().interrupt();return;}}
            if(!running)return;
            int tail=(head+size)%queue.length;queue[tail]=pcm[i];queue[(tail+1)%queue.length]=pcm[i+1];size+=2;
        }
        notifyAll();
    }
    private void play(){
        Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);
        short[] block=new short[512];
        try{
            // Queue about 93 ms before starting, to absorb CPU/GPU scheduling jitter.
            synchronized(this){while(size<8192 && running && !(dspFinished && size>0))wait();if(!running)return;
                if(dspFinished && android.os.Build.VERSION.SDK_INT>=31)track.setStartThresholdInFrames(Math.max(1,Math.min(1024,size/2)));}
            track.play();
            while(running){
                synchronized(this){if(paused){track.pause();while(paused && running)wait();if(!running)break;productionAt=0;productionRate=1;rateControl.recovered();track.play();}}
                long queued=bufferedFrames();
                long now=System.nanoTime();
                if(productionAt==0){productionAt=now;productionFrames=sourceFrames;}
                else if(now-productionAt>=250000000){
                    double observed=(sourceFrames-productionFrames)*1000000000d/(44100d*(now-productionAt));
                    productionRate+=(Math.max(0.5,Math.min(1,observed))-productionRate)*0.5;
                    productionAt=now;productionFrames=sourceFrames;
                }
                float requested=tempoFactor*(lockTempo?1:rateControl.update(queued,productionRate,now));
                if(Math.abs(requested-appliedSpeed)>=0.0049f){
                    appliedSpeed=requested;rateChanges++;
                    // Tempo changes are handled by SoundTouch; Android stays at fixed 44.1 kHz.
                }
                int count;
                synchronized(this){
                    while(size==0 && running){
                        if(paused){track.pause();while(paused && running)wait();if(!running)break;productionAt=0;productionRate=1;rateControl.recovered();track.play();}
                        long pending=writtenFrames-Integer.toUnsignedLong(track.getPlaybackHeadPosition());
                        if(pending<=0){
                            // Retain queued PCM and playback position; don't flush or discard it.
                            track.pause();recoveries++;if(journal!=null)journal.event(EngineLog.WARN,"audio.refill",java.util.Map.of("refills",recoveries,"pending_frames",pending,"ready_frames",size/2));
                            while(size<6144 && running)wait();if(!running)break;
                            rateControl.recovered();track.play();
                        }else wait(2);
                    }
                    if(!running)break;
                    count=Math.min(size,block.length);
                    for(int i=0;i<count;i++)block[i]=queue[(head+i)%queue.length];
                    head=(head+count)%queue.length;size-=count;inFlightFrames=count/2;notifyAll();
                }
                int offset=0;
                while(offset<count && running){int n;try(EngineLog.Span span=journal==null?null:journal.watch("audio.track.write",EngineLog.TRACE,20_000_000L)){n=track.write(block,offset,count-offset,AudioTrack.WRITE_BLOCKING);}if(n<=0){if(running)throw new IllegalStateException("Android audio write failed: "+n);break;}offset+=n;synchronized(this){writtenFrames+=n/2;inFlightFrames-=n/2;}}
            }
        }catch(InterruptedException e){Thread.currentThread().interrupt();}
        catch(RuntimeException e){if(running){if(journal!=null)journal.error("audio.track.error",e);failure=e;}}
        finally{running=false;synchronized(this){
            try{stoppedPlayed=track.getPlaybackHeadPosition();stoppedUnderruns=track.getUnderrunCount();diagnostic();}catch(IllegalStateException ignored){}
            trackReleased=true;track.release();notifyAll();
        }}
    }
    synchronized void setPaused(boolean value){if(paused!=value){lastProducedNanos=0;if(capture!=null)traceEvent(-1,-1,-1,value?"PAUSE":"RESUME");}if(paused && !value)timestampAfter=System.nanoTime();paused=value;notifyAll();}
    synchronized long bufferedFrames(){if(trackReleased)return 0;long played=Integer.toUnsignedLong(track.getPlaybackHeadPosition());return size/2+inFlightFrames+Math.max(0,writtenFrames-played);}
    void pace()throws InterruptedException{
        while(running){if(failure!=null)throw failure;long wait=AudioClockPacer.waitNanos(bufferedFrames());if(wait==0)return;java.util.concurrent.TimeUnit.NANOSECONDS.sleep(wait);}
    }
    record Diagnostic(long source,long dspInput,long written,long played,int inputQueue,long ready,long sourceWait,long dspCpu,int underruns,long refills){}
    synchronized Diagnostic snapshotDiagnostic(){return new Diagnostic(sourceFrames,dspInputFrames,writtenFrames,Integer.toUnsignedLong(playbackFrames()),inputSize/2,bufferedFrames(),sourceBlockedNanos,dspCpuNanos,underruns(),recoveries);}
    synchronized java.util.Map<String,Object> loggingSnapshot(){
        java.util.Map<String,Object> values=new java.util.LinkedHashMap<>();int under=underruns(), delta=Math.max(0,under-loggedUnderruns);
        values.put("source_frames",sourceFrames);values.put("dsp_input_frames",dspInputFrames);values.put("written_frames",writtenFrames);values.put("played_frames",Integer.toUnsignedLong(playbackFrames()));values.put("input_queue_frames",inputSize/2);values.put("ready_queue_frames",size/2);values.put("in_flight_frames",inFlightFrames);values.put("buffered_frames",bufferedFrames());values.put("underruns",under);values.put("underrun_delta",delta);loggedUnderruns=under;values.put("refills",recoveries);values.put("dropped_frames",droppedFrames);values.put("stretch_ratio",appliedSpeed);values.put("tempo_factor",tempoFactor);values.put("gain",configuredGain);values.put("dsp_cpu_ns",dspCpuNanos);values.put("producer_blocked_ns",sourceBlockedNanos);values.put("worst_producer_gap_ns",maxProducerGapNanos);values.put("track_sample_rate",configuredRate);values.put("native_output_sample_rate",nativeOutputRate);values.put("track_buffer_frames",configuredBuffer);values.put("performance_mode",configuredPerformance);
        if(!trackReleased){android.media.AudioDeviceInfo device=track.getRoutedDevice();if(device!=null){values.put("route_type",device.getType());values.put("route_id",device.getId());}}
        if(journal!=null&&delta>0)journal.event(EngineLog.WARN,"audio.underrun_snapshot",java.util.Map.of("underruns",under));return values;
    }
    float playbackSpeed(){return appliedSpeed;}
    String diagnostic(){
        int underruns;long buffered;
        synchronized(this){if(trackReleased)return lastDiagnostic;underruns=track.getUnderrunCount();buffered=bufferedFrames();}
        // Formatting must not hold the PCM queue's monitor during playback.
        String snapshot=String.format(java.util.Locale.ROOT,"Audio DSP: %.0f%% stretch · %d underruns · %d refills · %d dropped\nBuffer %d ms · worst producer gap %d ms · DSP CPU %d%%",appliedSpeed*100,underruns,recoveries,droppedFrames,buffered*1000/44100,maxProducerGapNanos/1000000,dspInputFrames==0?0:dspCpuNanos*44100/(dspInputFrames*10000000))+(capture==null?"":"\n"+capture.status());
        synchronized(this){if(!trackReleased)lastDiagnostic=snapshot;return lastDiagnostic;}
    }
    synchronized int playbackFrames(){return trackReleased?stoppedPlayed:track.getPlaybackHeadPosition();}
    synchronized int underruns(){return trackReleased?stoppedUnderruns:track.getUnderrunCount();}
    /** AudioTimestamp includes the device presentation clock, unlike queue occupancy.
     * Wait for a valid timestamp at startup rather than guessing a fixed video offset.
     */
    long audibleSourceFrames(){synchronized(presentationLock){
        if(paused)return audibleSource;
        long now=System.nanoTime();
        if(track.getTimestamp(timestamp)){
            long age=now-timestamp.nanoTime;
            if(timestamp.nanoTime>=Math.max(lastTimestamp,timestampAfter) && age>=-20000000L && age<500000000L){
                lastTimestamp=timestamp.nanoTime;lastClockRead=now;
                long played=timestamp.framePosition+age*44100/1000000000L;
                audibleSource=Math.max(audibleSource,timeline.sourceAt(Math.min(writtenFrames,played)));
            }
        }
        return lastTimestamp<0 || now-lastClockRead>=500000000L?-1:audibleSource;
    }}
    String synchronizationDiagnostic(){long audible=audibleSourceFrames();return audible<0?"A/V: waiting for audio timestamp":"A/V source lead "+Math.max(0,sourceFrames-audible)*1000/44100+" ms";}
    String deviceDiagnostic(){return "Android audio: performance="+track.getPerformanceMode()+" buffer="+track.getBufferSizeInFrames()+" frames, sampleRate="+track.getSampleRate();}
    void traceEvent(long guestClocks,long spuFrames,long vblank,String event){if(capture!=null)capture.events.record(System.nanoTime(),guestClocks,spuFrames,sourceFrames,vblank,event);}
    java.io.File saveDiagnosis(android.content.Context context,String scene)throws java.io.IOException{if(capture==null)throw new IllegalStateException("Audio capture is not enabled");return capture.save(context,scene+"\n"+diagnostic()+"\n"+synchronizationDiagnostic()+"\n"+deviceDiagnostic());}
    float deviceSpeed(){return track.getPlaybackParams().getSpeed();}
    @Override public void close(){
        synchronized(this){running=false;size=inputSize=0;notifyAll();}
        // pause interrupts a blocking write; release belongs to the worker.
        try{track.pause();track.flush();}catch(IllegalStateException ignored){}
        worker.interrupt();dspWorker.interrupt();
        try{worker.join(1000);dspWorker.join(1000);}catch(InterruptedException e){Thread.currentThread().interrupt();}
        if(capture!=null)capture.close();
    }
}
