package dev.psxlab.core;

import org.junit.Test;
import static org.junit.Assert.*;

/** DMA owns the RAM bus independently of COP0's CPU cache-isolation mode. */
public class DmaMemoryCoherenceTest {
    private static void command(Bus bus,int command,int... parameters){
        bus.write8(0x1f801800,0);for(int value:parameters)bus.write8(0x1f801802,value);
        bus.write8(0x1f801801,command);bus.cdrom.tick(CdRom.COMMAND_TICKS);
        bus.write8(0x1f801800,1);bus.write8(0x1f801803,31);bus.write8(0x1f801800,0);
    }
    private static void readyCd(Bus bus){
        bus.cdrom.insert(new SectorSource(){
            public int sectorCount(){return 100;}
            public char region(){return 'A';}
            public byte[] readSector(int lba){byte[] raw=new byte[2352];raw[15]=2;for(int i=24;i<2072;i++)raw[i]=(byte)(lba+i-24);return raw;}
        });
        command(bus,0x0e,0);command(bus,2,0,2,9);command(bus,6);
        for(int i=0;i<64&&bus.cdrom.sectorsRead==0;i++)bus.cdrom.tick(bus.cdrom.clocksToEvent());
        assertEquals(1,bus.cdrom.sectorsRead);bus.write8(0x1f801803,0x80);
    }
    @Test public void orderingTableDmaWritesRamWhileCpuStoresAreIsolated(){
        for(boolean nativeRam:new boolean[]{false,true})try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);if(nativeRam)bus.enableNativeMemory();
            bus.write32(0x100,0x12345678);bus.cacheIsolated=true;
            bus.write32(0x100,0);assertEquals("CPU store remains isolated",0x12345678,bus.read32(0x100));
            bus.write32(0x1f8010f0,0x0f654321);bus.write32(0x1f8010e0,0x100);
            bus.write32(0x1f8010e4,3);bus.write32(0x1f8010e8,0x11000002);
            assertEquals(0xfc,bus.read32(0x100));assertEquals(0xf8,bus.read32(0xfc));assertEquals(0xffffff,bus.read32(0xf8));
            assertTrue(bus.cacheIsolated);
        }
    }
    @Test public void gpuReadbackDmaWritesRamWhileCpuStoresAreIsolated(){
        for(boolean nativeRam:new boolean[]{false,true})try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);if(nativeRam)bus.enableNativeMemory();
            gpu.write(0xa0000000);gpu.write(0);gpu.write(0x10002);gpu.write(0x7c00001f);
            gpu.write(0xc0000000);gpu.write(0);gpu.write(0x10002);bus.cacheIsolated=true;
            bus.write32(0x1f8010f0,0x07654b21);bus.write32(0x1f8010a0,0x100);
            bus.write32(0x1f8010a4,0x10001);bus.write32(0x1f8010a8,0x01000200);
            assertEquals(0x7c00001f,bus.read32(0x100));assertTrue(bus.cacheIsolated);
        }
    }
    @Test public void cdDmaWritesRamWhileCpuStoresAreIsolated(){
        for(boolean nativeRam:new boolean[]{false,true})try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);if(nativeRam)bus.enableNativeMemory();
            readyCd(bus);bus.cacheIsolated=true;
            bus.write32(0x1f8010f0,0x0765c321);bus.write32(0x1f8010b0,0x10000);
            bus.write32(0x1f8010b4,512);bus.write32(0x1f8010b8,0x11000000);
            for(int i=0;i<2048;i++)assertEquals("native RAM="+nativeRam+" byte="+i,(9+i)&255,bus.read8(0x10000+i));
            assertTrue(bus.cacheIsolated);
        }
    }
    @Test public void spuReadbackDmaWritesRamWhileCpuStoresAreIsolated(){
        for(boolean nativeRam:new boolean[]{false,true})try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);if(nativeRam)bus.enableNativeMemory();
            bus.spu.write(0x1f801da6,0);for(int value:new int[]{0x1111,0x2222,0x3333,0x4444})bus.spu.writeTransfer(value);
            bus.spu.write(0x1f801da6,0);bus.cacheIsolated=true;
            bus.write32(0x1f8010f0,0x076d4321);bus.write32(0x1f8010c0,0x100);
            bus.write32(0x1f8010c4,0x10002);bus.write32(0x1f8010c8,0x01000200);
            assertEquals(0x22221111,bus.read32(0x100));assertEquals(0x44443333,bus.read32(0x104));assertTrue(bus.cacheIsolated);
        }
    }
    @Test public void scheduledMdecDmaWritesRamIfCpuIsolatesCacheBeforeItFinishes(){
        for(boolean nativeRam:new boolean[]{false,true})try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);if(nativeRam)bus.enableNativeMemory();
            bus.write32(0x1f801824,0x20000000);bus.write32(0x1f801820,0x28000001);bus.write32(0x1f801820,0xfe000000);
            assertEquals(16,bus.mdec.availableWords());
            bus.write32(0x1f8010f0,0x076543a1);bus.write32(0x1f801090,0x1000);
            bus.write32(0x1f801094,0x10010);bus.write32(0x1f801098,0x01000200);
            bus.cacheIsolated=true;bus.dma.tick(16);
            for(int i=0;i<16;i++)assertEquals(0x80808080,bus.read32(0x1000+i*4));
            assertEquals(0,bus.mdec.availableWords());assertTrue(bus.cacheIsolated);
        }
    }
}
