package dev.psxlab.core;
final class NativeCore {
    static final boolean AVAILABLE;
    static {boolean available;try{System.loadLibrary("psxcore");available=true;}catch(UnsatisfiedLinkError e){available=false;}AVAILABLE=available;}
    private NativeCore(){}
}
