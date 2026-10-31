package dev.psxlab;
import android.content.SharedPreferences;
final class Osd {
    static final String[] MODES={"Off","Minimal","Full"};
    // Deliberately do not inherit the old diagnostic build's enabled flag.
    static int mode(SharedPreferences p){return Math.max(0,Math.min(2,p.getInt("osdMode",0)));}
}
