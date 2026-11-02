package dev.psxlab;

import android.os.PerformanceHintManager;

/** ADPF work reports: the governor clocks for each frame's measured work instead of a bursty load average.
 * Android 12+; a device without hint sessions is asked once and then left alone. */
final class FrameHints implements AutoCloseable {
    private final android.content.Context context;
    private PerformanceHintManager.Session session;
    private int[] threads=new int[0];
    private long target;
    private boolean unsupported;
    FrameHints(android.content.Context context){this.context=context;}
    void report(int[] tids,long targetNanos,long workNanos){
        if(unsupported||context==null||android.os.Build.VERSION.SDK_INT<31||targetNanos<=0)return;
        try{
            if(session!=null&&!java.util.Arrays.equals(threads,tids))close();
            if(session==null){
                PerformanceHintManager manager=context.getSystemService(PerformanceHintManager.class);
                session=manager==null?null:manager.createHintSession(tids,targetNanos);
                if(session==null){unsupported=true;return;}
                threads=tids.clone();target=targetNanos;
            }
            if(targetNanos!=target){session.updateTargetWorkDuration(targetNanos);target=targetNanos;}
            if(workNanos>0)session.reportActualWorkDuration(workNanos);
        }catch(RuntimeException e){close();unsupported=true;}
    }
    @Override public void close(){if(session!=null){try{session.close();}catch(RuntimeException ignored){}session=null;}}
}
