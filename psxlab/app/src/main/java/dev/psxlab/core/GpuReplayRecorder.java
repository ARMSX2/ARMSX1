package dev.psxlab.core;

import java.util.*;

/** GPU-worker-owned rolling window. At most two bounded windows, no disk I/O while playing. */
final class GpuReplayRecorder {
    static final long CHECKPOINT_NANOS=2_000_000_000L;
    private int[] before;private List<GpuReplay.Operation> operations=new ArrayList<>();private int words;
    private long firstFrame,started,nextCheckpoint;private GpuReplay.Capture previous;
    GpuReplayRecorder(int[] vram,long frame,long now){before=vram.clone();firstFrame=frame;started=now;nextCheckpoint=now+CHECKPOINT_NANOS;}
    boolean full(int count){return words+count>GpuReplay.MAX_WORDS||operations.size()>=GpuReplay.MAX_OPERATIONS-1;}
    boolean due(long now){return now>=nextCheckpoint;}
    /** Input arrays are already immutable submission copies; retain them without another per-draw allocation. */
    void add(int kind,int[] data){operations.add(new GpuReplay.Operation(kind,data));words+=data.length;}
    void checkpoint(int[] vram,int[] display,int[] environment,long frame,long now){
        previous=new GpuReplay.Capture(before,List.copyOf(operations),vram.clone(),display,environment,firstFrame,frame,started,now);
        before=previous.after();operations=new ArrayList<>();words=0;firstFrame=frame;started=now;nextCheckpoint=now+CHECKPOINT_NANOS;
    }
    GpuReplay.Capture capture(int[] vram,int[] display,int[] environment,long frame,long now){
        int[] initial=before;long first=firstFrame,start=started;List<GpuReplay.Operation> selected=new ArrayList<>();int total=words;
        if(previous!=null){for(GpuReplay.Operation operation:previous.operations())total+=operation.words().length;
            if(total<=GpuReplay.MAX_WORDS&&previous.operations().size()+operations.size()<=GpuReplay.MAX_OPERATIONS){initial=previous.before();first=previous.firstFrame();start=previous.startedNanos();selected.addAll(previous.operations());}}
        selected.addAll(operations);return new GpuReplay.Capture(initial,List.copyOf(selected),vram.clone(),display,environment,first,frame,start,now);
    }
}
