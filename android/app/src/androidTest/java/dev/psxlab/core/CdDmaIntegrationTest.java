package dev.psxlab.core;

import org.junit.Test;
import static org.junit.Assert.*;

/** Start oversized CD DMA through the real CPU/MMIO bridge, including ARM64 compiled stores. */
public class CdDmaIntegrationTest {
    private static final int BASE=0x1f801800;
    private void ack(Bus b){b.write8(BASE,1);b.write8(BASE+3,31);b.write8(BASE,0);}
    private void command(Bus b,int command,int... values){b.write8(BASE,0);for(int v:values)b.write8(BASE+2,v);b.write8(BASE+1,command);b.cdrom.tick(CdRom.COMMAND_TICKS);}
    @Test public void allCpuModesCompleteCdBufferOverreadsWithoutHostExceptions(){
        for(int mode=0;mode<3;mode++)for(int words:new int[]{586,65536})try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);bus.cdrom.insert(new SectorSource(){
                public int sectorCount(){return 100;}
                public char region(){return 'A';}
                public byte[] readSector(int lba){byte[] sector=new byte[2352];sector[15]=2;for(int i=0;i<2048;i++)sector[24+i]=(byte)i;return sector;}
            });
            command(bus,0x0e,0);ack(bus);command(bus,2,0,2,0);ack(bus);command(bus,6);ack(bus);
            for(int i=0;i<64&&bus.cdrom.sectorsRead==0;i++)bus.cdrom.tick(bus.cdrom.clocksToEvent());assertEquals(1,bus.cdrom.sectorsRead);bus.write8(BASE+3,0x80);
            bus.write32(0x1f8010f0,0x0765c321);bus.write32(0x1f8010b0,0x20000);bus.write32(0x1f8010b4,words&65535);
            int[] code={0x3c081f80,0x350810b8,0x3c091100,0xad090000,0x1000ffff,0};for(int i=0;i<code.length;i++)bus.write32(0x10000+i*4,code[i]);
            Cpu cpu=new Cpu(bus);if(mode!=0)assertTrue(cpu.enableNative(mode==2,true,true));
            try{
                cpu.run(64);assertEquals(0,cpu.exceptions);assertEquals(0,bus.read32(0x1f8010b8)&0x1000000);assertEquals(words*4,bus.cdrom.dmaBytes);
                for(int i=0;i<words*4;i++)assertEquals("mode="+mode+" byte="+i,i<2048?i&255:0,bus.read8(0x20000+i));
            }finally{cpu.closeNative();}
        }
    }
}
