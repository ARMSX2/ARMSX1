package dev.psxlab.core;

/** Single-frame mailbox: a slow display receives the newest completed frame. */
public final class LatestFrame<T> {
    private T pending;
    private long replaced;
    public synchronized void offer(T frame){if(frame==null)throw new IllegalArgumentException("Null frame");if(pending!=null)replaced++;pending=frame;}
    public synchronized T take(){T frame=pending;pending=null;return frame;}
    public synchronized void clear(){pending=null;}
    public synchronized long replaced(){return replaced;}
}
