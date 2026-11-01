package dev.psxlab;
import android.graphics.PixelFormat;import android.media.*;import android.os.SystemClock;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.*;import java.nio.charset.StandardCharsets;import java.nio.file.Files;
import org.junit.Test;import static org.junit.Assert.*;

/** Inject display latency through the existing log clock, without production test hooks. */
public class PresentationBackpressureTest {
 @Test public void slowHostConsumerPreservesGuestClockAndAudio()throws Exception{
  var inst=InstrumentationRegistry.getInstrumentation();var c=inst.getTargetContext();var args=InstrumentationRegistry.getArguments();boolean baseline="true".equals(args.getString("baseline"));
  File ext=c.getExternalFilesDir(null),root=new File(ext,"performance/display-backpressure-"+System.currentTimeMillis());assertTrue(root.mkdirs());File exe=new File(root,"animation.exe");Files.write(exe.toPath(),FastForwardSpeedTest.animation());
  String selected=args.getString("renderer");
  for(String mode:selected==null?new String[]{"vulkan","software","opengl"}:new String[]{selected}){
   File dir=new File(root,mode);assertTrue(dir.mkdirs());File cfg=new File(dir,"settings.toml");Files.write(cfg.toPath(),("[video]\ngpu_backend=\""+mode+"\"\nrenderer=\""+(mode.equals("software")?"software":"hardware")+"\"\n[audio]\nmuted=true\n[runtime]\nlogging_enabled=true\nlog_level=2\n").getBytes(StandardCharsets.UTF_8));
   ArmSxBackend b=new ArmSxBackend(c,p->{},m->{});assertTrue(b.initialize(dir.toString(),new File(ext,"fixtures").toString(),cfg.toString()));b.selectBios("SCPH1001.BIN");
   final dev.psxlab.core.EngineLog[] log={null};
   dev.psxlab.core.EngineLog.Clock clock=new dev.psxlab.core.EngineLog.Clock(){long delayed=-1;
    public long wall(){
     if(Thread.currentThread().getName().equals("PSX-GPU")&&log[0]!=null){long frame=log[0].currentFrame();
      if(frame>0&&frame!=delayed)for(StackTraceElement at:Thread.currentThread().getStackTrace())if(at.getClassName().equals("dev.psxlab.core.EngineLog$Frame")&&at.getMethodName().equals("stage")){delayed=frame;SystemClock.sleep(32);break;}
     }return System.nanoTime();
    }
    public long cpu(){return android.os.Debug.threadCpuTimeNanos();}public long tid(){return android.os.Process.myTid();}
   };
   var journal=ArmSxBackend.class.getDeclaredField("journal");journal.setAccessible(true);((dev.psxlab.core.EngineLog)journal.get(b)).close();log[0]=new dev.psxlab.core.EngineLog(new File(dir,"logs"),clock);journal.set(b,log[0]);
   try(ImageReader reader=ImageReader.newInstance(640,480,PixelFormat.RGBA_8888,3)){
    b.surface(reader.getSurface());Thread vm=new Thread(()->b.run(exe.toString()),"PSX-Backpressure-CPU");vm.start();long start=SystemClock.elapsedRealtimeNanos(),sample=0,clocks=0,images=0,next=0;
    try{
     while(SystemClock.elapsedRealtimeNanos()-start<12_000_000_000L){
      long now=SystemClock.elapsedRealtimeNanos();if(now>=next){try(Image image=reader.acquireNextImage()){if(image!=null)images++;}next=now+33_333_333L;}
      if(sample==0&&now-start>=8_000_000_000L){sample=now;clocks=b.guestClocks();}
      assertEquals("",b.lastError());SystemClock.sleep(1);
     }
     double speed=(b.guestClocks()-clocks)/33868800d/((SystemClock.elapsedRealtimeNanos()-sample)/1e9);
     String report="mode="+mode+" guest_speed="+speed+" consumed_images="+images+"\n"+b.diagnosticReport();Files.write(new File(dir,"report.json").toPath(),report.getBytes(StandardCharsets.UTF_8));android.util.Log.i("BackpressureTest",report);
     if(!baseline)assertTrue(report,speed>.9&&speed<1.1);
    }finally{b.stop();long end=SystemClock.uptimeMillis()+15000;while(vm.isAlive()&&SystemClock.uptimeMillis()<end){try(Image image=reader.acquireLatestImage()){}vm.join(2);}assertFalse(vm.isAlive());}
   }finally{log[0].close();}
  }
 }
}
