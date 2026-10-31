package dev.psxlab;
import android.content.SharedPreferences;
final class DisplayConfig {
    static final String[] RENDERERS={"Software","OpenGL","Vulkan"};
    static final String[] RESOLUTIONS={"Auto — match device display","1× — 240p output","2× — 480p output","3× — 720p output","4× — 960p output","5× — 1080p output"};
    static final int FULL_HD_OUTPUT=5;
    static final String[] INTERNAL_RESOLUTIONS={"Native","720p HD","1080p Full HD"};
    private static final int[] INTERNAL_SCALES={1,3,5};
    static int internalMode(SharedPreferences p){int scale=p.getInt("internalScale",1);return scale>=4?2:scale>=2?1:0;}
    static int internalScale(SharedPreferences p){return scaleForInternalMode(internalMode(p));}
    static int scaleForInternalMode(int mode){return INTERNAL_SCALES[Math.max(0,Math.min(INTERNAL_SCALES.length-1,mode))];}
    static int renderer(SharedPreferences p){return Math.max(0,Math.min(2,p.getInt("renderer",p.getBoolean("hardwareGpu",true)?1:0)));}
    static int mode(SharedPreferences p){int saved=Math.max(0,Math.min(6,p.getInt("displayResolution",0)));return saved==5?3:saved==6?FULL_HD_OUTPUT:saved;}
    // Preserve saved output IDs: legacy 5 was the duplicate 720p choice, 6 was 1080p.
    static int preferenceMode(int selection){return selection==FULL_HD_OUTPUT?6:selection;}
    static int height(int mode){return mode>=1 && mode<=4?mode*240:mode==FULL_HD_OUTPUT?1080:0;}
}
