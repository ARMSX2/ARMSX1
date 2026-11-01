package dev.psxlab;

import androidx.test.platform.app.InstrumentationRegistry;
import dev.psxlab.core.*;
import java.io.File;
import java.lang.reflect.*;
import java.nio.file.Files;
import org.junit.Test;
import static org.junit.Assert.*;

/** Verifies the firmware LoadExe/Exec route with the explicitly supplied gameUri. */
public class BiosBootstrapIntegrationTest {
    @Test public void returningFromTheShellLetsTheBiosLoadTheDiscExecutable()throws Exception{
        var context=InstrumentationRegistry.getInstrumentation().getTargetContext();File fixtures=new File(context.getExternalFilesDir(null),"fixtures");
        ArmSxBackend adapter=new ArmSxBackend(context,p->{},m->{});Method mount=ArmSxBackend.class.getDeclaredMethod("openMedia",String.class);mount.setAccessible(true);
        String source=InstrumentationRegistry.getArguments().getString("gameUri");assertNotNull("Pass a licensed gameUri for BIOS disc boot",source);
        try(AutoCloseable media=(AutoCloseable)mount.invoke(adapter,source);Gpu gpu=new Gpu(false)){
            Field discField=media.getClass().getDeclaredField("disc");discField.setAccessible(true);IsoDisc disc=(IsoDisc)discField.get(media);
            byte[] exe=disc.bootExecutable(),bios=Files.readAllBytes(new File(fixtures,"SCPH1001.BIN").toPath());int entry=ExeLoader.le(exe,16),dest=ExeLoader.le(exe,24);
            Bus bus=new Bus(gpu);bus.cdrom.insert(disc);Cpu cpu=BiosBoot.discBoot(bios,bus,()->false);int caller=cpu.r[31];
            assertTrue(String.format("Shell caller %08X",caller),(caller&0x1fffffff)>=0x1fc00000);
            assertTrue(cpu.enableNative(true,true,true));boolean loaded=false;long begin=cpu.instructions,deadline=android.os.SystemClock.uptimeMillis()+20000;
            try{
                while(cpu.instructions-begin<33868800L*30&&android.os.SystemClock.uptimeMillis()<deadline){cpu.run(10000);if(bus.read32(dest)==ExeLoader.le(exe,2048)&&cpu.pc>=entry&&cpu.pc<entry+ExeLoader.le(exe,28)){loaded=true;break;}}
                assertTrue(cpu.diagnostic()+"\n"+bus.cdrom.diagnostic(),loaded);
            }finally{cpu.closeNative();}
        }
    }
}
