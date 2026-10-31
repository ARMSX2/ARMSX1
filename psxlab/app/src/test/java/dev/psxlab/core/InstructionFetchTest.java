package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
public class InstructionFetchTest {
    @Test public void storesImmediatelyReplacePreviouslyFetchedInstructions(){try(Gpu g=new Gpu(false)){Bus b=new Bus(g);Cpu c=new Cpu(b);b.write32(c.pc,0x24010001);c.step();assertEquals(1,c.r[1]);b.write32(0xa0010000,0x24010002);c.jump(0x80010000);c.step();assertEquals(2,c.r[1]);}}
    @Test public void byteAndHalfwordStoresInvalidateWordFetches(){try(Gpu g=new Gpu(false)){Bus b=new Bus(g);b.write32(0x10000,0x24010001);assertEquals(0x24010001,b.fetchInstruction(0x10000));b.write8(0x80010000,3);assertEquals(0x24010003,b.fetchInstruction(0xa0010000));b.write16(0x60010002,0x3402);assertEquals(0x34020003,b.fetchInstruction(0x80010000));}}
    @Test public void physicalRamMirrorsShareInvalidation(){try(Gpu g=new Gpu(false)){Bus b=new Bus(g);b.write32(0x10000,0x12345678);assertEquals(0x12345678,b.fetchInstruction(0x210000));b.write32(0x610000,0xabcdef01);assertEquals(0xabcdef01,b.fetchInstruction(0x80010000));}}
    @Test public void isolatedCacheStoresDoNotChangeRamOrFetch(){try(Gpu g=new Gpu(false)){Bus b=new Bus(g);b.write32(0x10000,7);assertEquals(7,b.fetchInstruction(0x10000));b.cacheIsolated=true;b.write32(0x10000,8);assertEquals(7,b.fetchInstruction(0x10000));}}
    @Test public void directRamEditsCanExplicitlyInvalidateFetch(){try(Gpu g=new Gpu(false)){Bus b=new Bus(g);assertEquals(0,b.fetchInstruction(0));b.ram[0]=12;b.invalidateInstructionFetch();assertEquals(12,b.fetchInstruction(0));}}
    @Test public void biosFetchAndAlignmentStillUseBusRules(){try(Gpu g=new Gpu(false)){Bus b=new Bus(g);byte[] bios=new byte[512*1024];bios[0]=17;b.loadBios(bios);assertEquals(17,b.fetchInstruction(0xbfc00000));try{b.fetchInstruction(0x80010001);fail();}catch(Bus.AddressError e){assertFalse(e.store);assertEquals(0x80010001,e.address);}}}
}
