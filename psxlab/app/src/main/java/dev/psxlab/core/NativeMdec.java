package dev.psxlab.core;

/** Stateless acceleration of the existing integer IDCT; RLE and output packing remain shared. */
final class NativeMdec {
    static final boolean AVAILABLE=NativeCore.AVAILABLE;
    static native void transform(int[] coefficients,int[] scale,int[] destination);
    private NativeMdec(){}
}
