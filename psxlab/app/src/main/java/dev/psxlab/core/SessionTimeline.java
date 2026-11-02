package dev.psxlab.core;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/** Session history for diagnostic reports, kept apart from the engine log tail so noisy guest tracing
 * cannot evict it. Rows summarise each interval; incidents give every problem and mode change its time,
 * and each late frame is split into emulation, other frame work and time spent off the CPU. */
public final class SessionTimeline {
    public static final long INTERVAL_NANOS=5_000_000_000L;
    /** A late frame is also an incident past this; shorter ones are counted per row. */
    public static final long STALL_NANOS=50_000_000L;
    private static final int ROWS=1440,INCIDENTS=1024; // Two hours of rows; oldest entries drop first.
    private final ArrayDeque<String> rows=new ArrayDeque<>(),incidents=new ArrayDeque<>();
    private long started=-1,droppedRows,droppedIncidents;
    private long windowStart,lastFrame,frames,late,worstGap,emulation,work;
    public synchronized void start(long now){rows.clear();incidents.clear();droppedRows=droppedIncidents=0;started=now;windowStart=now;lastFrame=0;clearWindow();}
    /** Pauses and backgrounding are not stutters; begin a fresh interval when frames resume. */
    public synchronized void resume(long now){lastFrame=0;windowStart=now;clearWindow();}
    private void clearWindow(){frames=late=worstGap=emulation=work=0;}
    /** Returns an incident description when this frame arrived too late, otherwise null.
     * expected is the paced frame period (0 when unpaced); wait is the pacing sleep requested. */
    public synchronized String frame(long now,long expected,long emulationNanos,long workNanos,long wait){
        String stall=null;
        if(lastFrame!=0&&expected>0){
            long gap=now-lastFrame;worstGap=Math.max(worstGap,gap);
            if(gap>expected+expected/2&&gap>expected+8_000_000L){
                late++;
                if(gap>=STALL_NANOS)stall=String.format(Locale.ROOT,"late frame %d ms (expected %d): emulation %d ms, other frame work %d ms, pacing sleep %d ms, off-CPU/descheduled %d ms",
                    gap/1_000_000,expected/1_000_000,emulationNanos/1_000_000,Math.max(0,workNanos-emulationNanos)/1_000_000,wait/1_000_000,Math.max(0,gap-workNanos-wait)/1_000_000);
            }
        }
        lastFrame=now;frames++;emulation+=emulationNanos;work+=workNanos;
        return stall;
    }
    public synchronized boolean due(long now){return started>=0&&now-windowStart>=INTERVAL_NANOS;}
    /** Close the interval: frame rate, late frames and the share of time spent emulating, plus the caller's host details. */
    public synchronized void row(long now,String details){
        long span=Math.max(1,now-windowStart);
        add(rows,String.format(Locale.ROOT,"%s fps %.1f late %d worst %d ms emu %d%% work %d%% | %s",stamp(now),frames*1e9/span,late,worstGap/1_000_000,emulation*100/span,work*100/span,details),ROWS,true);
        windowStart=now;clearWindow();
    }
    /** Append more detail to the row just written. */
    public synchronized void rowDetail(String detail){String last=rows.pollLast();if(last!=null)rows.addLast(last+" | "+detail);}
    public synchronized void incident(long now,String text){if(started>=0)add(incidents,stamp(now)+" "+text,INCIDENTS,false);}
    private String stamp(long now){return String.format(Locale.ROOT,"+%.1fs",(now-started)/1e9);}
    private void add(ArrayDeque<String> list,String value,int limit,boolean row){if(list.size()==limit){list.removeFirst();if(row)droppedRows++;else droppedIncidents++;}list.addLast(value);}
    public synchronized List<String> rows(){return new ArrayList<>(rows);}
    public synchronized List<String> incidents(){return new ArrayList<>(incidents);}
    public synchronized long droppedRows(){return droppedRows;}
    public synchronized long droppedIncidents(){return droppedIncidents;}
}
