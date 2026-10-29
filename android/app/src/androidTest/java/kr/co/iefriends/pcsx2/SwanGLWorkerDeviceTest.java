package kr.co.iefriends.pcsx2;

import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.armsx2.Main;
import com.armsx2.config.Ps1Settings;
import com.armsx2.config.Ps1SettingsStore;
import com.armsx2.runtime.MainActivityRuntime;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Method;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

/** Private device probe only. Never runs against a release or existing user-data folder. */
@RunWith(AndroidJUnit4.class)
public class SwanGLWorkerDeviceTest {
    interface Ready { boolean get(); }
    static void until(String label, long timeout, Ready ready) {
        long end = SystemClock.uptimeMillis()+timeout;
        while (SystemClock.uptimeMillis()<end) {
            if (ready.get()) return;
            SystemClock.sleep(100);
        }
        assertTrue(label+"\n"+NativeApp.getSwanDiagnostics(), ready.get());
    }
    static String config(File bios, String backend, boolean worker) {
        return config(bios,backend,worker,false);
    }
    static String config(File bios, String backend, boolean worker, boolean full) {
        return "[bios]\nsearch_path=\""+bios.getParent()+"\"\npreferred_model=\"SCPH-1001\"\n"+
            "[cpu]\nexecution_mode=\"cached\"\nfast_boot=false\nfastmem_mode=\"LUT\"\n"+
            "[runtime]\nframe_limit=true\nspeed_percent=100\nlogging_enabled=true\nquiet=false\n"+
            "[video]\ngpu_backend=\""+backend+"\"\nrenderer=\"hardware\"\ninternal_scale=3\n"+
            "threaded_gl_submission="+(worker&&!full)+"\nthreaded_hardware_backend="+(worker&&full)+"\nshader_precompile=\"Lazy\"\npgxp=false\n"+
            "[emulation]\nrunahead=0\nrewind=false\n";
    }
    static Ps1Settings settings(String text) throws Exception {
        Method parse = Ps1SettingsStore.class.getDeclaredMethod("parse", String.class);
        parse.setAccessible(true);
        return (Ps1Settings)parse.invoke(Ps1SettingsStore.INSTANCE, text);
    }
    @Test public void baselineThreadedGLAngleAndVulkanWithStateRoundTrips() throws Exception {
        Instrumentation test = InstrumentationRegistry.getInstrumentation();
        Context context = test.getTargetContext();
        assertEquals("Refusing to touch another app", "com.aaron.armsx1.renderthreadtest", context.getPackageName());
        File root = context.getExternalFilesDir(null);
        assertNotNull(root);
        File bios = new File(root, "bios/SCPH1001.BIN");
        assertEquals("Copy the user's BIOS into this test app first", 524288L, bios.length());
        File library = new File(root, "test-roms");
        assertTrue(library.mkdirs() || library.isDirectory());
        String game = InstrumentationRegistry.getArguments().getString("game", "/storage/emulated/0/ROMs/psx/digimon world 3 (usa).chd");
        assertTrue("Test ROM is not readable", new File(game).canRead());
        context.getSharedPreferences("ARMSX2", Context.MODE_PRIVATE).edit()
            .putBoolean("setupComplete", true).putString("bios", bios.getAbsolutePath())
            .putString("biosDir", bios.getParent()).remove("systemDir")
            .putString("romsDirs", "[\""+library.getAbsolutePath()+"\"]").commit();
        Ps1SettingsStore.INSTANCE.save(context, settings(config(bios,"opengl",false)));
        test.startActivitySync(new Intent(context, Main.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        until("Native initialization failed", 60000, () -> MainActivityRuntime.Companion.getNativeReady().getValue());
        String[] backends = {"opengl", "opengl", "opengl", "angle", "vulkan"};
        // Opt-in probe for the new architecture. A previous GL-only smoke run
        // does not validate it. Running this test always requires device consent.
        boolean full = "backend".equals(InstrumentationRegistry.getArguments().getString("mode"));
        try {
            for (int run=0; run<backends.length; run++) {
                String backend = backends[run];
                boolean worker = run>0;
                Ps1SettingsStore.INSTANCE.save(context, settings(config(bios,backend,worker,full)));
                test.runOnMainSync(() -> MainActivityRuntime.Companion.launchGame("file://"+game, null, false));
                until("No active VM for "+backend, 45000, NativeApp::hasActiveVM);
                until("No advancing frames for "+backend, 45000, () -> NativeApp.getPresentedFrameCount()>150);
                String diagnostic = NativeApp.getSwanDiagnostics();
                boolean expected = worker && (full || !backend.equals("vulkan"));
                assertTrue("Worker state disagrees with requested backend: "+diagnostic,
                    diagnostic.contains("gl_submission_worker="+(expected?"1":"0")));
                assertTrue("Split hardware backend disagrees with request: "+diagnostic,
                    diagnostic.contains("hardware_backend_worker="+((worker&&full)?"1":"0")));
                int before = NativeApp.getPresentedFrameCount();
                SystemClock.sleep(4000);
                int advanced = NativeApp.getPresentedFrameCount()-before;
                assertTrue("Rendering stopped", advanced>30);
                assertTrue("Save failed for "+backend, NativeApp.saveStateToSlot(9));
                assertEquals("Load failed for "+backend, 0, NativeApp.loadStateFromSlotChecked(9,false));
                int loadedFrame = NativeApp.getPresentedFrameCount();
                until("No frames after load", 15000, () -> NativeApp.getPresentedFrameCount()>loadedFrame+30);
                File output = new File(root,"logs/render-worker-"+run+"-"+backend+".png");
                output.getParentFile().mkdirs();
                Bitmap screenshot = test.getUiAutomation().takeScreenshot();
                assertNotNull(screenshot);
                try (FileOutputStream stream = new FileOutputStream(output)) { screenshot.compress(Bitmap.CompressFormat.PNG,100,stream); }
                finally { screenshot.recycle(); }
                android.util.Log.i("SwanGLWorkerTest", "PASS run="+run+" backend="+backend+" worker="+expected+" presented_in_4s="+advanced);
                test.runOnMainSync(() -> MainActivityRuntime.Companion.closeGame(false));
                until("VM did not stop", 30000, () -> !NativeApp.hasActiveVM());
                SystemClock.sleep(500);
            }
        } finally {
            test.runOnMainSync(() -> MainActivityRuntime.Companion.closeGame(false));
            until("Final teardown failed",30000, () -> !NativeApp.hasActiveVM());
            Ps1SettingsStore.INSTANCE.save(context, settings(config(bios,"opengl",false)));
        }
    }
}
