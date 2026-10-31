package dev.psxlab;
import android.content.Context;
import android.os.Debug;
import android.util.Log;
import dev.psxlab.core.*;
import java.io.*;
import java.util.*;

/** Bounded, silent runtime telemetry. Sampling profiles are kept out of A/B measurement windows. */
final class AudioDiagnostics implements AutoCloseable {
    private final BufferedWriter writer;
    private final File profileFile;
    private final Thread emulationThread;
    private final long started=System.nanoTime();
    private long previous=started,previousInstructions,previousCpu=Debug.threadCpuTimeNanos(),previousPace,previousGpuWait,previousDraw,previousScan;
    private AudioOutput.Diagnostic last;
    private volatile boolean closed;
    private Thread sampler;
    private boolean profiling;
    private int rows;
    AudioDiagnostics(Context context,String game,long instructions)throws IOException{
        File parent=context.getExternalFilesDir("diagnostics");if(parent==null)throw new IOException("Diagnostics storage unavailable");parent.mkdirs();String stem="audio-"+System.currentTimeMillis();
        File file=new File(parent,stem+".csv");profileFile=new File(parent,stem+"-profile.txt");writer=new BufferedWriter(new FileWriter(file));emulationThread=Thread.currentThread();previousInstructions=instructions;
        writer.write("# Game: "+game.replace('\n',' ')+"\n");
        writer.write("elapsed_s,dt_s,guest_ips,cpu_ms,pace_wait_ms,gpu_wait_ms,gpu_draw_ms,scanout_ms,gpu_queue,source_fps,dsp_fps,write_fps,play_fps,raw_frames,ready_frames,source_wait_ms,dsp_cpu_ms,underruns,refills,fetch_cache\n");writer.flush();Log.i("PSXDiag","Trace file "+file.getAbsolutePath());
    }
    void sample(Cpu cpu,Bus bus,Gpu gpu,AudioOutput output,long paceWait){
        if(closed)return;long now=System.nanoTime();if(now-previous<1000000000L)return;
        double elapsed=(now-started)/1e9,dt=(now-previous)/1e9;
        // ON/OFF/ON while the menu is active; restore the normal setting afterwards.
        bus.instructionCaching=!(elapsed>=45 && elapsed<65);
        AudioOutput.Diagnostic d=output.snapshotDiagnostic();long hostCpu=Debug.threadCpuTimeNanos();
        if(last!=null){
            String row=String.format(Locale.ROOT,"%.3f,%.6f,%.0f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.0f,%.0f,%.0f,%.0f,%d,%d,%.3f,%.3f,%d,%d,%s",elapsed,dt,(cpu.instructions-previousInstructions)/dt,(hostCpu-previousCpu)/1e6,(paceWait-previousPace)/1e6,(gpu.diagnosticQueueWait-previousGpuWait)/1e6,(gpu.diagnosticDraw-previousDraw)/1e6,(gpu.diagnosticScanout-previousScan)/1e6,gpu.queuedCommands(),(d.source()-last.source())/dt,(d.dspInput()-last.dspInput())/dt,(d.written()-last.written())/dt,(d.played()-last.played())/dt,d.inputQueue(),d.ready(),(d.sourceWait()-last.sourceWait())/1e6,(d.dspCpu()-last.dspCpu())/1e6,d.underruns(),d.refills(),bus.instructionCaching?"on":"off");
            try{writer.write(row);writer.newLine();writer.flush();}catch(IOException e){Log.w("PSXDiag","Trace write failed",e);close();}
            Log.i("PSXDiag",row);rows++;
        }
        previous=now;previousInstructions=cpu.instructions;previousCpu=hostCpu;previousPace=paceWait;previousGpuWait=gpu.diagnosticQueueWait;previousDraw=gpu.diagnosticDraw;previousScan=gpu.diagnosticScanout;last=d;
        if(!profiling && elapsed>=30){profiling=true;startSampler();}
        if(rows>=100){bus.instructionCaching=true;gpu.diagnosticsEnabled=false;close();}
    }
    private void startSampler(){sampler=new Thread(()->{
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND);
        Map<String,Integer> counts=new HashMap<>();
        for(int i=0;i<200 && !closed;i++){
            try{Thread.sleep(50);}catch(InterruptedException e){break;}
            StackTraceElement[] stack=emulationThread.getStackTrace();StringBuilder key=new StringBuilder(emulationThread.getState().toString());
            for(int j=0;j<Math.min(5,stack.length);j++)key.append(" | ").append(stack[j]);counts.merge(key.toString(),1,Integer::sum);
        }
        try(BufferedWriter out=new BufferedWriter(new FileWriter(profileFile))){List<Map.Entry<String,Integer>> entries=new ArrayList<>(counts.entrySet());entries.sort((a,b)->Integer.compare(b.getValue(),a.getValue()));for(Map.Entry<String,Integer> entry:entries){out.write(entry.getValue()+" "+entry.getKey());out.newLine();}}catch(IOException e){Log.w("PSXDiag","Profile write failed",e);}
        Log.i("PSXDiag","Profile complete "+profileFile.getAbsolutePath());
    },"PSX-DiagnosisSampler");sampler.setDaemon(true);sampler.start();}
    @Override public void close(){if(closed)return;closed=true;if(sampler!=null)sampler.interrupt();try{writer.close();}catch(IOException ignored){}}
}
