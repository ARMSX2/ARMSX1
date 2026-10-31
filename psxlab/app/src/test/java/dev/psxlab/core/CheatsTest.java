package dev.psxlab.core;

import org.junit.Test;
import static org.junit.Assert.*;

public class CheatsTest {
    @Test public void writesConditionsSlidesAndSelectionStayWithinRam()throws Exception{
        Cheats cheats=new Cheats();assertEquals(2,cheats.loadText("[Health]\n80000100 0063\nD0000100 0063\n30000102 007F\nE1000102 007F\n30000103 00FF\n50000302 0001\n80000200 0010\n80200000 FFFF\n[Other]\n30000104 0042\n"));
        try(Gpu gpu=new Gpu(false)){Bus bus=new Bus(gpu);assertEquals(1,cheats.arm(true,new String[]{" health "}));cheats.apply(bus);
            assertEquals(99,bus.read16(0x100));assertEquals(127,bus.read8(0x102));assertEquals(0,bus.read8(0x103));assertEquals(0,bus.read8(0x104));
            assertEquals(16,bus.read16(0x200));assertEquals(17,bus.read16(0x202));assertEquals(18,bus.read16(0x204));assertEquals(0,bus.read16(0));
            assertEquals(0,cheats.arm(false,new String[]{"Health"}));bus.write16(0x100,1);cheats.apply(bus);assertEquals(1,bus.read16(0x100));
        }
    }
    @Test public void groupsDoNotShareConditionalOrSlideStateAndUnknownTypesAreVisible()throws Exception{
        Cheats cheats=new Cheats();cheats.loadText("[First]\nD0000100 0001\n[Second]\n30000110 0044\n[Slide]\n50000302 0001\n[Last]\n80000120 0005\nC0000000 0001\n@desc Test description\n");
        cheats.arm(true,new String[]{"First","Second","Slide","Last","Missing"});assertEquals(1,cheats.diagnostic().get("missing_names"));
        try(Gpu gpu=new Gpu(false)){Bus bus=new Bus(gpu);cheats.apply(bus);assertEquals(0x44,bus.read8(0x110));assertEquals(5,bus.read16(0x120));assertEquals(0,bus.read16(0x122));}
        assertTrue(cheats.describe(),cheats.describe().contains("Last\u001f2\u001f1\u001fTest description"));
    }
    @Test public void writesInvalidateCachedInstructionsAndDoNotRepeatUnchangedStores()throws Exception{
        Cheats cheats=new Cheats();cheats.loadText("[Patch]\n80010000 0009\n");cheats.arm(true,new String[]{"Patch"});
        try(Gpu gpu=new Gpu(false)){Bus bus=new Bus(gpu);Cpu cpu=new Cpu(bus);bus.write32(0x10000,0x24420001);bus.write32(0x10004,0x1000ffff);bus.write32(0x10008,0);cpu.jump(0x80010000);cpu.run(64);assertEquals(1,cpu.r[2]);
            cheats.apply(bus);cpu.jump(0x80010000);cpu.run(64);assertEquals(10,cpu.r[2]);
            cheats.loadText("[Unaligned]\n80000103 1234\n");cheats.arm(true,new String[]{"Unaligned"});cheats.apply(bus);assertEquals(0x34,bus.read8(0x103));assertEquals(0x12,bus.read8(0x104));
        }
    }
    @Test public void quotedNamesAndCatalogueBoundsAreRespected()throws Exception{
        assertArrayEquals(new String[]{"Health, Ammo","Say \"yes\""},Cheats.selectedNames("[\"Health, Ammo\", \"Say \\\"yes\\\"\"]"));
        Cheats cheats=new Cheats();try{cheats.loadText("[Too many]\n".repeat(4097));fail();}catch(java.io.IOException expected){assertEquals(0,cheats.armedCount());assertEquals("",cheats.describe());}
    }
}
