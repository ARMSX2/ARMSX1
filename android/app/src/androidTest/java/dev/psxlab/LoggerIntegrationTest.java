package dev.psxlab;

import android.app.Instrumentation;
import android.content.*;
import android.net.Uri;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import com.armsx2.*;
import kr.co.iefriends.pcsx2.NativeApp;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import org.json.*;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

@RunWith(AndroidJUnit4.class)
public class LoggerIntegrationTest {
    private static String read(File f)throws Exception{return new String(Files.readAllBytes(f.toPath()),java.nio.charset.StandardCharsets.UTF_8);}
    @Test public void captureAndExport()throws Exception{
        assertEquals("true",InstrumentationRegistry.getArguments().getString("loggerAudit"));
        Instrumentation inst=InstrumentationRegistry.getInstrumentation();Context c=inst.getTargetContext();
        SharedPreferences prefs=c.getSharedPreferences("ARMSX2",0);boolean enabled=prefs.getBoolean("diagnostics.generateLog",false);String destination=prefs.getString("diagnostics.reportUri",null);
        java.lang.reflect.Field field=NativeApp.class.getDeclaredField("lab");field.setAccessible(true);Object previous=field.get(null);
        assertTrue(previous==null||!((ArmSxBackend)previous).active());
        EngineAuditActivity activity=(EngineAuditActivity)inst.startActivitySync(new Intent(c,EngineAuditActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        long until=SystemClock.uptimeMillis()+10000;while((activity.gameSurface()==null||!activity.hasWindowFocus())&&SystemClock.uptimeMillis()<until)SystemClock.sleep(25);assertTrue(activity.hasWindowFocus());
        File dir=new File(c.getExternalFilesDir(null),"performance/logger-"+System.currentTimeMillis());assertTrue(dir.mkdirs());File fixtures=new File(c.getExternalFilesDir(null),"fixtures");
        String renderer=InstrumentationRegistry.getArguments().getString("renderer","hardware");
        File config=new File(dir,"settings.toml");Files.write(config.toPath(),("[cpu]\nfast_boot=false\n[video]\nrenderer=\""+renderer+"\"\n[runtime]\nlogging_enabled=false\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
        ArmSxBackend backend=new ArmSxBackend(c,p->{},m->{});Thread vm=null;
        try{
            field.set(null,backend);assertTrue(backend.initialize(dir.getAbsolutePath(),fixtures.getAbsolutePath(),config.getAbsolutePath()));backend.selectBios("SCPH1001.BIN");backend.surface(activity.gameSurface());backend.displayMetadata(activity.bufferWidth(),activity.bufferHeight(),activity.getWindowManager().getDefaultDisplay().getRefreshRate());
            DiagnosticsReport.INSTANCE.enable(c);assertTrue(DiagnosticsReport.INSTANCE.isEnabled(c));assertTrue(activity.hasWindowFocus());assertEquals(destination,prefs.getString("diagnostics.reportUri",null));
            vm=new Thread(()->backend.run(new File(fixtures,"game.chd").getAbsolutePath()),"PSX-Logger-CPU");vm.start();SystemClock.sleep(18000);assertTrue(backend.lastError(),backend.lastError().isEmpty());assertTrue(backend.frames()>600);
            backend.pause(true);SystemClock.sleep(200);backend.pause(false);SystemClock.sleep(1000);
            assertTrue(backend.flushDiagnosticLog(3000));File log=new File(backend.diagnosticDirectory(),"engine-current.jsonl");String rows=read(log);int frames=0,snapshots=0;boolean audio=false,present=false;
            for(String line:rows.split("\n")){JSONObject row=new JSONObject(line);if("frame".equals(row.optString("type"))){frames++;assertTrue(row.has("gpu_tid"));assertTrue(row.has("cpu_active_ns"));assertTrue(row.has("sleep_overshoot_ns"));present|=row.has("presentation_api_accepted");}if("engine.snapshot".equals(row.optString("path"))){snapshots++;JSONObject a=row.getJSONObject("audio");audio|=a.has("underrun_delta")&&a.has("track_sample_rate")&&a.has("producer_blocked_ns");}}
            assertTrue("Frame records",frames>100);assertTrue("Snapshots",snapshots>5);assertTrue("Structured audio",audio);assertTrue("Presentation",present);
            backend.stop();vm.join(10000);assertFalse(vm.isAlive());
            // A failed launch should be diagnosed without terminating the application.
            backend.run(new File(dir,"missing-game.chd").getAbsolutePath());assertFalse(backend.lastError().isEmpty());JSONObject failed=new JSONObject(backend.diagnosticReport());assertEquals("Not started",failed.getString("audio"));assertEquals("Not started",failed.getString("core"));assertEquals("00000000",failed.getString("pc"));assertTrue(backend.flushDiagnosticLog(3000));
            DiagnosticsReport.INSTANCE.setExportDestination(c,Uri.parse("content://"+c.getPackageName()+".logger-audit/report"));assertEquals(DiagnosticExportGate.Result.SUCCESS,DiagnosticsReport.INSTANCE.refresh(c,true));
            String report=read(new File(c.getCacheDir(),"logger-audit-report.txt"));assertTrue(report.contains("core: PSX Lab (original engine)"));assertTrue(report.contains("runtime.error"));assertTrue(report.contains("FileNotFoundException"));assertTrue(report.contains("sleep_overshoot_ns"));assertTrue(report.contains("audio"));assertTrue(report.contains("process exits"));assertTrue(report.contains("current engine thread stacks"));
            Files.write(new File(dir,"export.txt").toPath(),report.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            DiagnosticsReport.INSTANCE.disable(c);assertTrue(backend.flushDiagnosticLog(3000));long length=log.length();SystemClock.sleep(300);assertEquals(length,log.length());assertFalse(new JSONObject(backend.diagnosticReport()).getJSONObject("logging").getBoolean("enabled"));
            assertEquals("Can export after capture is disabled",DiagnosticExportGate.Result.SUCCESS,DiagnosticsReport.INSTANCE.refresh(c,true));
            DiagnosticsReport.INSTANCE.setExportDestination(c,Uri.parse("content://invalid.logger.test/report"));assertEquals(DiagnosticExportGate.Result.FAILED,DiagnosticsReport.INSTANCE.refresh(c,true));assertTrue(new File(c.getFilesDir(),"logs/diagnostic-report.txt").length()>1000);
            android.util.Log.i("PSXLoggerAudit","Saved "+dir+" frames="+frames+" snapshots="+snapshots);
        }finally{
            backend.stop();if(vm!=null)vm.join(10000);backend.captureDiagnostics(false);backend.configureLogging(false,2);backend.flushDiagnosticLog(2000);field.set(null,previous);
            prefs.edit().putBoolean("diagnostics.generateLog",enabled).putString("diagnostics.reportUri",destination).commit();if(previous!=null)((ArmSxBackend)previous).captureDiagnostics(enabled);inst.runOnMainSync(activity::finish);
        }
    }
}
