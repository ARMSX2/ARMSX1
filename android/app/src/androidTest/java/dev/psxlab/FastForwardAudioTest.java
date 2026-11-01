package dev.psxlab;

import android.content.Context;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.lang.reflect.Field;
import java.util.Map;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

@RunWith(AndroidJUnit4.class)
public class FastForwardAudioTest {
    @Test public void fastForwardRetainsAudioUnlessMuteIsRequested()throws Exception{
        Context c=InstrumentationRegistry.getInstrumentation().getTargetContext();ArmSxBackend b=new ArmSxBackend(c,p->{},m->{});
        Field audio=ArmSxBackend.class.getDeclaredField("audio"),options=ArmSxBackend.class.getDeclaredField("options");audio.setAccessible(true);options.setAccessible(true);
        try(AudioOutput output=new AudioOutput(true)){
            audio.set(b,output);options.set(b,Map.of("audio.mute_fast_forward","false","audio.fast_forward_volume","60","runtime.fast_forward_speed","2"));
            b.fastForward(true);assertEquals(2f,output.playbackSpeed(),0.001);assertEquals(0.6f,((Number)output.loggingSnapshot().get("gain")).floatValue(),0.001);
            b.fastForward(false);assertEquals(1f,output.playbackSpeed(),0.001);assertEquals(1f,((Number)output.loggingSnapshot().get("gain")).floatValue(),0.001);
            options.set(b,Map.of("audio.mute_fast_forward","true"));b.fastForward(true);assertEquals(0f,((Number)output.loggingSnapshot().get("gain")).floatValue(),0.001);
            b.fastForward(false);assertEquals(1f,((Number)output.loggingSnapshot().get("gain")).floatValue(),0.001);b.audioMuted(true);assertEquals(0f,((Number)output.loggingSnapshot().get("gain")).floatValue(),0.001);
            b.audioMuted(false);options.set(b,Map.of());
            for(float speed:new float[]{1.5f,2,3,4,0}){
                b.fastForwardSpeed(speed);b.fastForward(true);
                assertEquals("Fast-forward is audible by default",1f,((Number)output.loggingSnapshot().get("gain")).floatValue(),0.001);
                assertEquals(speed==0?2:speed,output.playbackSpeed(),0.001);
                b.fastForward(false);assertEquals(1f,output.playbackSpeed(),0.001);
            }
        }finally{audio.set(b,null);}
    }
    @Test public void doubleTempoProducesPcmAtHalfTheDurationWithFixedAndroidRate()throws Exception{
        try(AudioOutput output=new AudioOutput(true)){
            output.setGain(0);output.setTempoFactor(2);short[] block=new short[2048];int produced=0;
            while(produced<44100){int frames=Math.min(1024,44100-produced);for(int i=0;i<frames;i++){short value=(short)(12000*Math.sin((produced+i)*2*Math.PI*440/44100));block[i*2]=block[i*2+1]=value;}output.samples(block,frames*2);produced+=frames;}
            output.finishInput();long until=SystemClock.uptimeMillis()+5000;while(output.writtenFrames<20000&&SystemClock.uptimeMillis()<until)SystemClock.sleep(10);
            assertEquals(44100,output.sourceFrames);assertTrue("Fast-forward PCM was not written",output.writtenFrames>=20000);assertTrue("Output was not time-compressed",output.writtenFrames<26000);assertEquals(1f,output.deviceSpeed(),0.001);assertEquals(44100,((Number)output.loggingSnapshot().get("track_sample_rate")).intValue());
        }
    }
    @Test public void realGameKeepsWritingAudioDuringFastForward()throws Exception{
        assertEquals("true",InstrumentationRegistry.getArguments().getString("gameAudioAudit"));
        android.app.Instrumentation inst=InstrumentationRegistry.getInstrumentation();Context c=inst.getTargetContext();
        com.armsx2.EngineAuditActivity a=(com.armsx2.EngineAuditActivity)inst.startActivitySync(new android.content.Intent(c,com.armsx2.EngineAuditActivity.class).addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK));
        long until=SystemClock.uptimeMillis()+10000;while((a.gameSurface()==null||!a.hasWindowFocus())&&SystemClock.uptimeMillis()<until)SystemClock.sleep(25);assertTrue(a.hasWindowFocus());
        java.io.File fixtures=new java.io.File(c.getExternalFilesDir(null),"fixtures"),dir=new java.io.File(c.getExternalFilesDir(null),"performance/fast-audio-"+System.currentTimeMillis());assertTrue(dir.mkdirs());java.io.File config=new java.io.File(dir,"settings.toml");
        java.nio.file.Files.write(config.toPath(),"[video]\nrenderer=\"hardware\"\ndisplay_aspect=\"wide16x9\"\n[audio]\nmute_fast_forward=false\nfast_forward_volume=100\n[runtime]\nfast_forward_speed=2\n".getBytes(java.nio.charset.StandardCharsets.UTF_8));
        ArmSxBackend b=new ArmSxBackend(c,p->{},m->{});assertTrue(b.initialize(dir.getAbsolutePath(),fixtures.getAbsolutePath(),config.getAbsolutePath()));b.selectBios("SCPH1001.BIN");b.surface(a.gameSurface());Thread vm=new Thread(()->b.run(new java.io.File(fixtures,"game.chd").getAbsolutePath()),"PSX-Fast-Audio-Test");vm.start();
        try{
            SystemClock.sleep(10000);assertTrue(b.lastError(),b.lastError().isEmpty());assertTrue(b.frames()>200);Field field=ArmSxBackend.class.getDeclaredField("audio");field.setAccessible(true);AudioOutput output=(AudioOutput)field.get(b);assertNotNull(output);
            long before=output.writtenFrames;b.fastForward(true);SystemClock.sleep(3000);assertEquals(2f,output.playbackSpeed(),0.01);assertTrue("PCM writes stopped while fast-forwarding",output.writtenFrames-before>44100);assertTrue(((Number)output.loggingSnapshot().get("gain")).floatValue()>0);
            b.fastForwardSpeed(0);SystemClock.sleep(2000);int under=output.underruns();before=output.writtenFrames;SystemClock.sleep(3000);
            assertTrue("Unlimited fast-forward continues PCM playback",output.writtenFrames-before>100000);
            assertEquals("Unlimited audio keeps up with available speed",under,output.underruns());
            assertTrue("Unlimited audio remains audible",((Number)output.loggingSnapshot().get("gain")).floatValue()>0);
            b.fastForward(false);SystemClock.sleep(2000);assertEquals(1f,output.playbackSpeed(),0.01);assertTrue(b.audioNonzero()>100);assertTrue(b.lastError(),b.lastError().isEmpty());
            java.nio.file.Files.write(new java.io.File(dir,"report.json").toPath(),b.diagnosticReport().getBytes(java.nio.charset.StandardCharsets.UTF_8));
        }finally{b.stop();vm.join(10000);assertFalse(vm.isAlive());inst.runOnMainSync(a::finish);}
    }
}
