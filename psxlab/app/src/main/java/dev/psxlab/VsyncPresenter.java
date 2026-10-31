package dev.psxlab;

import android.view.Choreographer;
import java.util.function.Consumer;
import dev.psxlab.core.LatestFrame;

/** Android display-clock consumer. Start/stop and drawing run on the UI thread. */
final class VsyncPresenter<T> implements Choreographer.FrameCallback {
    private final LatestFrame<T> mailbox=new LatestFrame<>();
    private final Consumer<T> draw;
    private final Choreographer choreographer=Choreographer.getInstance();
    private boolean active;
    volatile long presented,callbacks;
    VsyncPresenter(Consumer<T> draw){this.draw=draw;}
    synchronized void start(){if(active)return;active=true;choreographer.postFrameCallback(this);}
    synchronized void stop(){active=false;choreographer.removeFrameCallback(this);mailbox.clear();}
    synchronized void offer(T frame){if(active)mailbox.offer(frame);}
    long replaced(){return mailbox.replaced();}
    @Override public void doFrame(long frameTimeNanos){
        if(!active)return;callbacks++;
        T frame=mailbox.take();if(frame!=null){draw.accept(frame);presented++;}
        if(active)choreographer.postFrameCallback(this);
    }
}
