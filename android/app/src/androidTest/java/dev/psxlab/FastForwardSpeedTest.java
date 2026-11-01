package dev.psxlab;

import android.content.Context;
import android.os.SystemClock;
import androidx.test.platform.app.InstrumentationRegistry;
import com.armsx2.EngineAuditActivity;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.junit.Test;
import static org.junit.Assert.*;

/** Changing guest frames on a real display: offscreen/static surfaces hide FIFO throttling. */
public class FastForwardSpeedTest {
    private static void word(byte[] data,int at,int value){for(int i=0;i<4;i++)data[at+i]=(byte)(value>>>(8*i));}
    private static byte[] animation(){
        byte[] exe=new byte[4096];System.arraycopy("PS-X EXE".getBytes(StandardCharsets.US_ASCII),0,exe,0,8);
        word(exe,16,0x80010000);word(exe,24,0x80010000);word(exe,28,2048);
        // Original guest program: change the entire 320x240 picture on each guest vblank.
        int[] code={0x3c081f80,0x3c090800,0x35290001,0xad091814,
            0x3c090704,0x35290010,0xad091814,0x3c090300,0xad091814,
            0x240a0000,0x240c0400,0x258cffff,0x1580fffe,0,0x8d0b1070,0,0x316b0001,0x1160fff8,0,
            0xad001070,0x254a0101,0x3c090200,0x012a4825,0xad091810,
            0xad001810,0x3c0900f0,0x35290140,0xad091810,0x0800400a,0};
        for(int i=0;i<code.length;i++)word(exe,2048+i*4,code[i]);return exe;
    }
    private static double speed(ArmSxBackend b,long duration){
        long time=SystemClock.elapsedRealtimeNanos(),clocks=b.guestClocks();SystemClock.sleep(duration);
        return (b.guestClocks()-clocks)/33868800.0/((SystemClock.elapsedRealtimeNanos()-time)/1e9);
    }
    private static int underruns(ArmSxBackend b){
        var match=java.util.regex.Pattern.compile("(\\d+) underruns").matcher(b.audioDiagnostic());
        assertTrue(b.audioDiagnostic(),match.find());return Integer.parseInt(match.group(1));
    }
    @Test public void measuresGuestAccelerationAndNormalSpeedRecovery()throws Exception{
        var inst=InstrumentationRegistry.getInstrumentation();Context c=inst.getTargetContext();
        EngineAuditActivity a=(EngineAuditActivity)inst.startActivitySync(new android.content.Intent(c,EngineAuditActivity.class).addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK));
        long end=SystemClock.uptimeMillis()+10000;while((a.gameSurface()==null||!a.hasWindowFocus())&&SystemClock.uptimeMillis()<end)SystemClock.sleep(20);assertTrue(a.hasWindowFocus());
        File root=new File(c.getExternalFilesDir(null),"performance/fast-speed-"+System.currentTimeMillis());assertTrue(root.mkdirs());
        File fixtures=new File(c.getExternalFilesDir(null),"fixtures"),game=new File(root,"animation.exe");Files.write(game.toPath(),animation());
        boolean baseline="true".equals(InstrumentationRegistry.getArguments().getString("baseline"));
        boolean muteFast="true".equals(InstrumentationRegistry.getArguments().getString("muteFast"));
        float multiplier=Float.parseFloat(InstrumentationRegistry.getArguments().getString("multiplier","4"));
        String ownedGame=InstrumentationRegistry.getArguments().getString("gameUri");
        String source=ownedGame==null?game.toString():ownedGame;
        boolean allSpeeds="true".equals(InstrumentationRegistry.getArguments().getString("allSpeeds"));
        float reportedHz=Float.parseFloat(InstrumentationRegistry.getArguments().getString("reportedHz",Float.toString(a.getDisplay().getRefreshRate())));
        String selected=InstrumentationRegistry.getArguments().getString("renderer");
        try{for(String renderer:selected==null?new String[]{"vulkan","software","opengl"}:new String[]{selected}){
            File dir=new File(root,renderer);assertTrue(dir.mkdirs());File config=new File(dir,"settings.toml");
            Files.write(config.toPath(),("[video]\ngpu_backend=\""+renderer+"\"\nrenderer=\""+(renderer.equals("software")?"software":"hardware")+"\"\n[audio]\nmuted=true\nmute_fast_forward="+muteFast+"\n[runtime]\nfast_forward_speed="+multiplier+"\n").getBytes(StandardCharsets.UTF_8));
            ArmSxBackend b=new ArmSxBackend(c,p->{},m->{});assertTrue(b.initialize(dir.toString(),fixtures.toString(),config.toString()));b.selectBios("SCPH1001.BIN");b.displayMetadata(a.bufferWidth(),a.bufferHeight(),reportedHz);b.surface(a.gameSurface());
            Thread vm=new Thread(()->b.run(source),"PSX-Fast-Speed-Test");vm.start();
            try{
                end=SystemClock.uptimeMillis()+20000;while(b.frames()<120&&SystemClock.uptimeMillis()<end){assertEquals("",b.lastError());SystemClock.sleep(20);}assertTrue(b.diagnosticReport(),b.frames()>100);
                assertEquals("Actual presentation API",renderer.equals("vulkan")?"Vulkan":"OpenGL",b.displayRenderer());
                double normal=speed(b,2000);
                for(float requested:allSpeeds?new float[]{1.5f,2,3,4,0}:new float[]{multiplier}){
                    b.fastForwardSpeed(requested);b.fastForward(true);SystemClock.sleep(2000);int beforeFastUnderruns=underruns(b);double fast=speed(b,3000);
                    String report=renderer+" normal="+normal+" fast"+requested+"x="+fast+" muteFast="+muteFast+"\n"+b.diagnosticReport();Files.write(new File(dir,"speed-"+requested+".json").toPath(),report.getBytes(StandardCharsets.UTF_8));android.util.Log.i("FastSpeedTest",report);
                    assertEquals("",b.lastError());
                    if(!baseline)assertTrue(renderer+" expected "+requested+"x; actual="+fast,requested==0?fast>(ownedGame==null?4.5:1.2):fast>requested*.87&&fast<requested*1.1);
                    if(!baseline)assertEquals("Steady fast-forward audio underruns: "+b.audioDiagnostic(),beforeFastUnderruns,underruns(b));
                }
                b.fastForward(false);SystemClock.sleep(3000);int beforeUnderruns=underruns(b);double recovered=speed(b,2000);
                android.util.Log.i("FastSpeedTest",renderer+" recovered="+recovered+" "+b.audioDiagnostic());
                if(!baseline){assertTrue("Normal="+normal,normal>.9&&normal<1.1);assertTrue("Recovered="+recovered,recovered>.9&&recovered<1.1);}
                assertEquals("No new underruns after normal speed recovers",beforeUnderruns,underruns(b));
                if(ownedGame!=null)assertTrue("Owned game still produces audio",b.audioNonzero()>100);
            }finally{b.stop();vm.join(15000);assertFalse(vm.isAlive());}
        }}finally{inst.runOnMainSync(a::finish);}
    }
}
