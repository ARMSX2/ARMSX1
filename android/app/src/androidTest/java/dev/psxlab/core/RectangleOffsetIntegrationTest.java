package dev.psxlab.core;

import java.util.Arrays;
import org.junit.Test;
import static org.junit.Assert.*;

/** Signed-11-bit rectangle origin wrap, including Linda3's lower-buffer tile coordinates. */
public class RectangleOffsetIntegrationTest {
    private static int xy(int x,int y){return (x&65535)|(y<<16);}
    private static int[][] cases(){return new int[][]{{-256,1280,376,-912,120,368},{1280,-256,-912,376,368,120}};}
    private static int[] environment(int[] c){return new int[]{0,0x10a,0,0,1023|(511<<10),(c[2]&2047)|((c[3]&2047)<<11),0};}
    private static int[] seed(){int[] ram=new int[524288];for(int y=0;y<16;y++)Arrays.fill(ram,y*1024+640,y*1024+656,0x3e0);return ram;}
    private static int[] expected(int[] c){int[] ram=seed();for(int y=c[5];y<c[5]+16;y++)Arrays.fill(ram,y*1024+c[4],y*1024+c[4]+16,0x3e0);return ram;}
    private static int[] packet(int[] c){return new int[]{0x7d000000,xy(c[0],c[1]),0};}
    private static void batch(HardwareRasterizer gpu,int[] c){int[] p=packet(c),e=environment(c),words=new int[p.length+8];words[0]=p.length;System.arraycopy(p,0,words,1,p.length);System.arraycopy(e,0,words,p.length+1,7);gpu.drawBatch(words);}
    @Test public void nativeSoftwareWrapsTexturedRectangleOrigins(){
        assertTrue("Native rasterizer required",NativeRasterizer.AVAILABLE);
        for(int[] c:cases()){int[] actual=seed();new SoftwareRasterizer(actual,true).draw(packet(c),environment(c));assertArrayEquals(expected(c),actual);}
    }
    @Test public void glesNativeAndHdAndNativeVulkanWrapRectangleOrigins(){
        for(int[] mode:new int[][]{{0,1},{0,3},{1,1}}){
            boolean vulkan=mode[0]!=0;int scale=mode[1];
            try(VulkanPresenter output=vulkan?new VulkanPresenter(1):null;
                HardwareRasterizer gpu=vulkan?new VulkanHardwareRasterizer(scale,output):new HardwareRasterizer(scale)){
                for(int[] c:cases()){gpu.setVram(seed());batch(gpu,c);int[] actual=new int[524288];gpu.readVram(actual);assertArrayEquals("scale="+scale+" native Vulkan="+vulkan,expected(c),actual);}
            }
        }
    }
}
