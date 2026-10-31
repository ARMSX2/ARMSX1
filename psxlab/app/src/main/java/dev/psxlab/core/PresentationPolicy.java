package dev.psxlab.core;

/** Eligibility is separate from the saved choice: the physical display must match. */
public final class PresentationPolicy {
    public static final int OFF=0,BFI=1,SHADER=2,NO_RATE=3,RATE_MISMATCH=4,SUSPENDED=5,NO_NATIVE=6,NO_SHADER=7,NEEDS_VULKAN=8;
    private PresentationPolicy(){}
    public static int evaluate(int mode,int multiple,float displayHz,float gameHz,boolean allowed,boolean nativePresent,boolean vulkan,boolean shader){
        if(mode<=0)return OFF;if(!allowed)return SUSPENDED;if(!nativePresent)return NO_NATIVE;
        if(mode==2&&!vulkan)return NEEDS_VULKAN;if(mode==2)return NO_SHADER;
        if(!Float.isFinite(displayHz)||displayHz<=0)return NO_RATE;
        if(multiple<2||multiple>4||!Float.isFinite(gameHz)||gameHz<=0||Math.abs(displayHz-gameHz*multiple)>1f)return RATE_MISMATCH;
        return mode==1?BFI:SHADER;
    }
    public static String describe(int decision,int multiple,float displayHz,float gameHz){return switch(decision){
        case BFI->"BFI active ("+multiple+"x)";case SHADER->"Shader subframes active ("+multiple+"x)";
        case SUSPENDED->"Temporarily suspended while paused or fast-forwarding";
        case NO_NATIVE->"Inactive: native presentation is unavailable";
        case NEEDS_VULKAN->"Inactive: Vulkan shader presentation is unavailable";
        case NO_SHADER->"Inactive: enable a working shader chain first";
        case NO_RATE->"Waiting for the actual display refresh rate";
        case RATE_MISMATCH->String.format(java.util.Locale.ROOT,"Inactive: display %.1f Hz; requires approximately %.1f Hz",displayHz,gameHz*multiple);
        default->"High-refresh presentation: Off";
    };}
}
