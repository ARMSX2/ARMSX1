// SPDX-License-Identifier: GPL-3.0-or-later
package dev.psxlab;

import dev.psxlab.core.SectorSource;
import java.nio.ByteBuffer;

/**
 * RetroAchievements for PSX Lab sessions (rcheevos rc_client in libpsxachievements). Softcore only.
 *
 * <p>The host supplies a {@link Transport} for HTTPS, token storage, options and notifications.
 * {@link ArmSxBackend} reports sessions, machines (boot, reset, state load) and guest frames.
 * Every entry point is a no-op until {@link #initialize} has loaded the native module.
 */
public final class Achievements {
    public interface Transport {
        /** Blocking HTTPS request; returns {status, body}. */
        String[] request(String url, String post, String contentType);
        void store(String user, String token);
        /** {user, token} or empty strings. */
        String[] restore();
        int optionValue(String key, int fallback);
        void option(String key, int value);
        void unlocked(String title, int points, int seconds);
        /** kind 0 = notice, 1 = challenge indicator, 2 = leaderboard tracker, 3 = clear indicators. */
        void event(int kind, int id, String text, boolean visible);
        void unlockSound(String path);
    }

    private static volatile Transport transport;
    private static volatile boolean ready;
    private Achievements() {}

    public static synchronized boolean initialize(Transport host) {
        transport = host;
        if (ready) return true;
        try { System.loadLibrary("psxachievements"); }
        catch (UnsatisfiedLinkError missing) { log("native module unavailable: " + missing.getMessage()); return false; }
        ready = nativeInitialize("");
        if (!ready) log("rc_client initialisation failed");
        return ready;
    }
    public static boolean available() { return ready; }

    /** RetroAchievements PlayStation hash of a mounted disc; empty when it has no PS1 executable. */
    public static String hash(SectorSource disc) { return ready && disc != null ? nativeHash(disc) : ""; }
    public static String login(String user, String password) {
        return ready ? nativeLogin(user, password) : "RetroAchievements is unavailable in this build.";
    }
    public static void logout() { if (ready) nativeLogout(); }
    public static String json() { return ready ? nativeJson() : "{}"; }
    public static String presence() { return ready ? nativePresence() : ""; }
    public static void option(String key, boolean enabled) { if (ready) nativeOption(key, enabled); }
    public static void optionInt(String key, int value) { if (ready) nativeOptionInt(key, value); }
    public static void setUnlockSound(String path) { if (ready) nativeUnlockSound(path == null ? "" : path); }

    static void sessionBegin() { if (ready) nativeSessionBegin(); }
    static void sessionEnd() { if (ready) nativeSessionEnd(); }
    static void machine(String hash, boolean restored) { if (ready) nativeMachine(hash == null ? "" : hash, restored); }
    /** Once per guest frame on the VM thread. {@code memory} is the recompiler's buffer (RAM at 0, scratchpad at 0x280000) or null. */
    static void frame(ByteBuffer memory, byte[] ram, byte[] scratch, boolean cheats) { if (ready) nativeFrame(memory, ram, scratch, cheats); }

    // ---- Called from native ----
    static String[] request(String url, String post, String contentType) {
        Transport host = transport;
        try {
            String[] result = host == null ? null : host.request(url, post, contentType);
            return result != null && result.length == 2 ? result : new String[]{"-2", ""};
        } catch (RuntimeException e) { return new String[]{"-2", ""}; }
    }
    static void store(String user, String token) { Transport host = transport; if (host != null) try { host.store(user, token); } catch (RuntimeException ignored) {} }
    static String[] restore() {
        Transport host = transport;
        try { String[] saved = host == null ? null : host.restore(); return saved != null && saved.length == 2 ? saved : new String[]{"", ""}; }
        catch (RuntimeException e) { return new String[]{"", ""}; }
    }
    static int optionValue(String key, int fallback) { Transport host = transport; try { return host == null ? fallback : host.optionValue(key, fallback); } catch (RuntimeException e) { return fallback; } }
    static void option(String key, int value) { Transport host = transport; if (host != null) try { host.option(key, value); } catch (RuntimeException ignored) {} }
    static void unlocked(String title, int points, int seconds) { Transport host = transport; if (host != null) try { host.unlocked(title, points, seconds); } catch (RuntimeException ignored) {} }
    static void event(int kind, int id, String text, boolean visible) { Transport host = transport; if (host != null) try { host.event(kind, id, text, visible); } catch (RuntimeException ignored) {} }
    static void unlockSound(String path) { Transport host = transport; if (host != null) try { host.unlockSound(path); } catch (RuntimeException ignored) {} }
    static byte[] sector(SectorSource source, int lba) { try { return source.readSector(lba); } catch (Exception e) { return null; } }
    static void log(String line) { System.out.println("@@PSX_ACHIEVEMENTS@@ " + line); }

    private static native boolean nativeInitialize(String userAgent);
    private static native String nativeHash(SectorSource disc);
    private static native String nativeLogin(String user, String password);
    private static native void nativeLogout();
    private static native void nativeSessionBegin();
    private static native void nativeSessionEnd();
    private static native void nativeMachine(String hash, boolean restored);
    private static native void nativeFrame(ByteBuffer memory, byte[] ram, byte[] scratch, boolean cheats);
    private static native String nativeJson();
    private static native String nativePresence();
    private static native void nativeOption(String key, boolean enabled);
    private static native void nativeOptionInt(String key, int value);
    private static native void nativeUnlockSound(String path);
}
