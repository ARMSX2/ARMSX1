package dev.psxlab;

import android.content.Context;
import dev.psxlab.core.PcmWindow;
import dev.psxlab.core.AudioEventTrace;
import dev.psxlab.core.SessionAudioRecorder;
import java.io.*;
import java.nio.charset.StandardCharsets;

/** Diagnostic PCM is copied in memory; WAV I/O runs only on an explicit save. */
final class AudioCapture implements AutoCloseable {
    final PcmWindow source=new PcmWindow(44100*20),processed=new PcmWindow(44100*20);
    final AudioEventTrace events=new AudioEventTrace(32768);
    private SessionAudioRecorder continuous;
    private String recordingFailure;
    AudioCapture(){}
    AudioCapture(Context context){
        try{
            File parent=context.getExternalFilesDir("diagnostics");if(parent==null)throw new IOException("Diagnosis storage unavailable");
            continuous=new SessionAudioRecorder(new File(parent,"dialogue-session-"+System.currentTimeMillis()+"-"+java.util.UUID.randomUUID().toString().substring(0,8)),44100*60*15,2048);
            events.setSink(continuous::event);android.util.Log.i("PSXCapture","Continuous session: "+continuous.folder().getAbsolutePath());
        }catch(IOException e){recordingFailure=e.toString();android.util.Log.e("PSXCapture","Continuous recording unavailable",e);}
    }
    void source(short[] pcm,int count){source.append(pcm,count);if(continuous!=null)continuous.append(false,pcm,count);}
    void processed(short[] pcm,int count){processed.append(pcm,count);if(continuous!=null)continuous.append(true,pcm,count);}
    String status(){return recordingFailure!=null?"Trace INCOMPLETE: "+recordingFailure:continuous==null?"Trace: memory capture":"Trace: "+continuous.status();}
    File sessionFolder(){return continuous==null?null:continuous.folder();}
    @Override public void close(){events.setSink(null);if(continuous!=null)continuous.close();}
    File save(Context context,String note)throws IOException{
        long snapshotWall=System.currentTimeMillis(),snapshotMono=System.nanoTime();
        PcmWindow.Snapshot raw=source.snapshot(),dsp=processed.snapshot();String eventSnapshot=events.snapshot();
        File parent=context.getExternalFilesDir("diagnostics");if(parent==null)throw new IOException("Diagnosis storage unavailable");
        File folder=new File(parent,"audio-"+System.currentTimeMillis());if(!folder.mkdirs())throw new IOException("Cannot create audio diagnosis folder");
        wave(new File(folder,"source.wav"),raw.samples());wave(new File(folder,"processed.wav"),dsp.samples());
        if(continuous!=null)try{continuous.checkpoint();}catch(IOException e){recordingFailure=e.toString();}
        String info=note+"\n44,100 Hz stereo PCM16. Source is before DSP; processed is after DSP.\nSource first/end frames: "+raw.firstFrame()+" / "+raw.endFrame()+"\nProcessed first/end frames: "+dsp.firstFrame()+" / "+dsp.endFrame()+"\nSnapshot wall ms / monotonic ns: "+snapshotWall+" / "+snapshotMono+"\nThis quick marker contains the last 20 seconds; its events.tsv is bounded to 32768 events.\nContinuous session: "+(continuous==null?"none":continuous.folder().getAbsolutePath())+"\n"+status()+"\nThe continuous session retains all PCM and events from game launch, with 15-minute WAV segments.\nGPU_READY means render completion, not physical scanout. Guest clocks at native MMIO events may lag by one CPU batch.\nThese captures do not include Android mixer or speaker output.\n";
        try(OutputStream out=new FileOutputStream(new File(folder,"events.tsv"))){out.write(eventSnapshot.getBytes(StandardCharsets.UTF_8));}
        try(OutputStream out=new FileOutputStream(new File(folder,"metadata.txt"))){out.write(info.getBytes(StandardCharsets.UTF_8));}return folder;
    }
    private static void number(OutputStream out,int value,int bytes)throws IOException{for(int i=0;i<bytes;i++)out.write(value>>>(i*8)&255);}
    private static void wave(File file,short[] pcm)throws IOException{
        try(OutputStream out=new BufferedOutputStream(new FileOutputStream(file))){
            out.write("RIFF".getBytes(StandardCharsets.US_ASCII));number(out,36+pcm.length*2,4);out.write("WAVEfmt ".getBytes(StandardCharsets.US_ASCII));number(out,16,4);number(out,1,2);number(out,2,2);number(out,44100,4);number(out,44100*4,4);number(out,4,2);number(out,16,2);out.write("data".getBytes(StandardCharsets.US_ASCII));number(out,pcm.length*2,4);
            for(short value:pcm)number(out,value&65535,2);
        }
    }
}
