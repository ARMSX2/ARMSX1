package android.util;

/** JVM-only warning sink. Shader index recovery tests do not need Android logging. */
public final class Log {
    private Log() {}
    public static int w(String tag,String message){return 0;}
    public static int w(String tag,String message,Throwable error){return 0;}
}
