package dev.psxlab.core;

import android.view.Surface;

/** A display that keeps a frame generation history (Vulkan display, OpenGL display). GPU worker only. */
interface FrameGenerationTarget {
    /** Make the next ordinary present call copy its picture into the history instead of showing it. */
    void requestCapture(boolean estimate,boolean reset);
    /** True when that present call took the capture; clears a request it did not take. */
    boolean finishCapture();
    /** Show the picture at time t (t below 1 is interpolated, 1 is the newest captured frame). */
    boolean presentGenerated(Surface surface,float t);
    /** Why the history could not be set up on this device; empty when it works. */
    String frameGenerationError();
}
