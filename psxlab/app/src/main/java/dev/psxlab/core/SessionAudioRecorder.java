package dev.psxlab.core;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.concurrent.*;

/** Continuous diagnostic recording; producers never wait for storage. */
public final class SessionAudioRecorder implements AutoCloseable {
    private record Packet(int stream,short[] pcm,String event,CompletableFuture<Void> checkpoint){}
    private final File folder;
    private final int segmentFrames;
    private final ArrayBlockingQueue<Packet> queue;
    private final Thread worker;
    private final long startedWall=System.currentTimeMillis(),startedMono=System.nanoTime();
    private volatile boolean accepting=true,finished;
    private volatile String failure;
    public SessionAudioRecorder(File folder,int segmentFrames,int queueCapacity)throws IOException{
        if(segmentFrames<1 || queueCapacity<1)throw new IllegalArgumentException("Recording capacity");
        this.folder=folder;this.segmentFrames=segmentFrames;queue=new ArrayBlockingQueue<>(queueCapacity);
        if(!folder.mkdirs())throw new IOException("Cannot create session folder: "+folder);
        state("STARTING",0,0,0);
        worker=new Thread(this::write,"PSX-TraceWriter");worker.setPriority(Thread.MIN_PRIORITY);worker.start();
    }
    public File folder(){return folder;}
    public String status(){return failure!=null?"INCOMPLETE: "+failure:finished?"saved":accepting?"recording":"finishing";}
    public synchronized void append(boolean processed,short[] pcm,int count){
        if(count<0 || count>pcm.length || (count&1)!=0)throw new IllegalArgumentException("Stereo PCM length");
        if(accepting && count!=0)offer(new Packet(processed?1:0,Arrays.copyOf(pcm,count),null,null));
    }
    public synchronized void event(String row){if(accepting)offer(new Packet(2,null,row,null));}
    private void offer(Packet packet){if(!queue.offer(packet)){failure="storage queue overflow; recording stopped";accepting=false;}}
    public void checkpoint()throws IOException{
        CompletableFuture<Void> done=new CompletableFuture<>();
        synchronized(this){if(failure!=null)throw new IOException(failure);if(finished)return;if(!accepting)throw new IOException("Recording is finishing");offer(new Packet(3,null,null,done));}
        try{done.get(10,TimeUnit.SECONDS);}catch(Exception e){throw new IOException("Session checkpoint failed: "+status(),e);}
    }
    @Override public synchronized void close(){accepting=false;}
    public void awaitFinished(long timeoutMillis)throws IOException{
        try{worker.join(timeoutMillis);}catch(InterruptedException e){Thread.currentThread().interrupt();throw new IOException(e);}
        if(!finished || failure!=null)throw new IOException("Session recording: "+status());
    }
    private void state(String status,long raw,long dsp,long events)throws IOException{
        String text="status="+status+"\nstarted_wall_ms="+startedWall+"\nstarted_monotonic_ns="+startedMono+"\nsource_frames="+raw+"\nprocessed_frames="+dsp+"\nevents="+events+"\nformat=44100 Hz stereo PCM16\nsegment_frames="+segmentFrames+"\nfailure="+(failure==null?"none":failure)+"\n";
        try(OutputStream out=new FileOutputStream(new File(folder,"session.txt"))){out.write(text.getBytes(StandardCharsets.UTF_8));}
    }
    private void write(){
        long events=0,lastFlush=System.nanoTime();
        try(BufferedWriter trace=new BufferedWriter(new OutputStreamWriter(new FileOutputStream(new File(folder,"events.tsv")),StandardCharsets.UTF_8));BufferedWriter index=new BufferedWriter(new OutputStreamWriter(new FileOutputStream(new File(folder,"segments.tsv")),StandardCharsets.UTF_8));WaveStream raw=new WaveStream("source",index);WaveStream dsp=new WaveStream("processed",index)){
            trace.write(AudioEventTrace.HEADER);index.write("stream\tfile\tfirst_frame\tend_frame\n");
            while(accepting || !queue.isEmpty()){
                Packet packet=queue.poll(100,TimeUnit.MILLISECONDS);
                if(packet!=null){
                    if(packet.stream==0)raw.append(packet.pcm);else if(packet.stream==1)dsp.append(packet.pcm);else if(packet.stream==2){trace.write(packet.event);trace.write('\n');events++;}
                }
                if((packet!=null && packet.checkpoint!=null) || System.nanoTime()-lastFlush>=1000000000L){
                    raw.flush();dsp.flush();trace.flush();index.flush();state(failure==null?"ACTIVE":"INCOMPLETE",raw.total,dsp.total,events);lastFlush=System.nanoTime();
                    if(packet!=null && packet.checkpoint!=null)packet.checkpoint.complete(null);
                }
            }
            raw.flush();dsp.flush();state(failure==null?"COMPLETE":"INCOMPLETE",raw.total,dsp.total,events);
        }catch(Exception e){failure=e.toString();accepting=false;try{state("INCOMPLETE",-1,-1,events);}catch(IOException ignored){}}
        finally{finished=true;Packet pending;while((pending=queue.poll())!=null)if(pending.checkpoint!=null)pending.checkpoint.completeExceptionally(new IOException(status()));}
    }
    private final class WaveStream implements AutoCloseable {
        private final String name;private final BufferedWriter index;
        private final byte[] bytes=new byte[8192];
        private BufferedOutputStream out;private File file;
        private int part,frames;private long first,total;
        WaveStream(String name,BufferedWriter index){this.name=name;this.index=index;}
        void append(short[] pcm)throws IOException{
            int at=0;
            while(at<pcm.length){
                if(out==null){file=new File(folder,name+"-"+String.format(java.util.Locale.ROOT,"%04d",++part)+".wav");out=new BufferedOutputStream(new FileOutputStream(file),65536);out.write(new byte[44]);frames=0;first=total;}
                int count=Math.min(pcm.length-at,Math.min(bytes.length/2,(segmentFrames-frames)*2));
                for(int i=0;i<count;i++){int sample=pcm[at+i];bytes[i*2]=(byte)sample;bytes[i*2+1]=(byte)(sample>>>8);}
                out.write(bytes,0,count*2);at+=count;frames+=count/2;total+=count/2;
                if(frames==segmentFrames)closePart();
            }
        }
        void flush()throws IOException{if(out!=null){out.flush();header(file,frames);}}
        private void closePart()throws IOException{if(out!=null){out.close();out=null;header(file,frames);index.write(name+"\t"+file.getName()+"\t"+first+"\t"+total+"\n");}}
        @Override public void close()throws IOException{closePart();}
    }
    private static void le(RandomAccessFile out,int value,int bytes)throws IOException{for(int i=0;i<bytes;i++)out.write(value>>>(i*8));}
    private static void header(File file,int frames)throws IOException{
        try(RandomAccessFile out=new RandomAccessFile(file,"rw")){out.writeBytes("RIFF");le(out,36+frames*4,4);out.writeBytes("WAVEfmt ");le(out,16,4);le(out,1,2);le(out,2,2);le(out,44100,4);le(out,44100*4,4);le(out,4,2);le(out,16,2);out.writeBytes("data");le(out,frames*4,4);}
    }
}
