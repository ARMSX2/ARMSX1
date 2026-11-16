package dev.psxlab.core;

import org.junit.Test;
import java.util.Arrays;
import java.util.Random;
import static org.junit.Assert.*;

/** Native Vulkan checks for the backend selected in the Samsung reports. */
public class VulkanReportRegressionTest {
    private void draw(HardwareRasterizer gpu,int[] packet,int[] env){
        int[] words=new int[packet.length+8];words[0]=packet.length;
        System.arraycopy(packet,0,words,1,packet.length);
        System.arraycopy(env,0,words,packet.length+1,7);gpu.drawBatch(words);
    }

    @Test public void nativeVulkanMatchesIndependentNonFeedbackRasterFixtures(){
        Random random=new Random(0x505358);int[] expected=new int[524288];
        for(int i=0;i<expected.length;i++)expected[i]=random.nextInt(65536);
        int[] actual=expected.clone();SoftwareRasterizer reference=new SoftwareRasterizer(expected,false);
        int[] ops={0x20,0x22,0x24,0x25,0x26,0x28,0x2c,0x30,0x34,0x38,0x3c,0x40,0x42,0x50,0x52,0x60,0x62,0x64,0x65,0x66,0x68,0x6c,0x70,0x74,0x78,0x7c};
        int fixtures=0,clut=(300<<6)|32;
        try(VulkanPresenter output=new VulkanPresenter(1);HardwareRasterizer gpu=new VulkanHardwareRasterizer(1,output)){
            assertTrue(gpu.nativeVulkan());gpu.setVram(actual);
            for(int depth=0;depth<3;depth++)for(int blend=0;blend<4;blend++)for(int op:ops){
                // Texture pages and palette stay outside the destination, so live and
                // pre-draw-snapshot feedback contracts do not contaminate this oracle.
                int[] env={0,8|16|(depth<<7)|(blend<<5)|512,random.nextInt(1<<20),0,127|(127<<10),0,fixtures%4};
                int[] packet=new int[12];int at=1;packet[0]=(op<<24)|random.nextInt(1<<24);
                int[] xy={8|(12<<16),100|(18<<16),20|(104<<16),110|(112<<16)};
                if(op<0x40){
                    int n=(op&8)!=0?4:3;
                    for(int i=0;i<n;i++){
                        if(i>0&&(op&16)!=0)packet[at++]=random.nextInt(1<<24);
                        packet[at++]=xy[i];if((op&4)!=0)packet[at++]=(random.nextInt(65536))|(i==0?clut<<16:0);
                    }
                }else if(op<0x60){packet[at++]=xy[0];if((op&16)!=0)packet[at++]=random.nextInt(1<<24);packet[at++]=xy[3];}
                else{packet[at++]=xy[0];if((op&4)!=0)packet[at++]=random.nextInt(65536)|(clut<<16);if((op&24)==0)packet[at++]=64|(48<<16);}
                packet=Arrays.copyOf(packet,at);reference.draw(packet,env);draw(gpu,packet,env);gpu.readVram(actual);
                assertArrayEquals("Native Vulkan op="+Integer.toHexString(op)+" depth="+depth+" blend="+blend,expected,actual);fixtures++;
            }
        }
        android.util.Log.i("VulkanReportRegressionTest","Native Vulkan independent raster fixtures passed: "+fixtures);
    }

    @Test public void nativeVulkanFeedbackUsesItsPreDrawSnapshot(){
        int[] vram=new int[524288];for(int x=0;x<16;x++)vram[x]=x+1;
        int[] expected=vram.clone();for(int x=0;x<8;x++)expected[x+2]=vram[x];
        try(VulkanPresenter output=new VulkanPresenter(1);HardwareRasterizer gpu=new VulkanHardwareRasterizer(1,output)){
            gpu.setVram(vram);draw(gpu,new int[]{0x65000000,2,0,8|(1<<16)},new int[]{0,256,0,0,1023|(511<<10),0,0});
            gpu.readVram(vram);assertArrayEquals(expected,vram);
        }
    }

    @Test public void clippedAndOffsetDrawsStayOrderedAcrossBothGameFramebuffers(){
        Random random=new Random(0x544f464c);int[] expected=new int[524288];
        for(int i=0;i<expected.length;i++)expected[i]=random.nextInt(65536);
        SoftwareRasterizer reference=new SoftwareRasterizer(expected,false);int[] actual=expected.clone();
        int[] ops={0x20,0x22,0x24,0x25,0x26,0x28,0x2c,0x30,0x34,0x38,0x3c,0x60,0x62,0x64,0x65,0x66};
        try(VulkanPresenter output=new VulkanPresenter(1);HardwareRasterizer gpu=new VulkanHardwareRasterizer(1,output)){
            gpu.setVram(actual);
            for(int frame=0;frame<8;frame++){
                int baseY=(frame&1)*256;
                for(int n=0;n<96;n++){
                    int op=ops[random.nextInt(ops.length)],depth=random.nextInt(3),blend=random.nextInt(4);
                    int offsetX=random.nextInt(97)-48,offsetY=baseY+random.nextInt(65)-32;
                    int[] env={0,10|16|(depth<<7)|(blend<<5)|512,random.nextInt(1<<20),
                        random.nextInt(32)|((baseY+random.nextInt(16))<<10),511|((baseY+207)<<10),
                        (offsetX&2047)|((offsetY&2047)<<11),random.nextInt(4)};
                    int[] packet=new int[12];int at=1;packet[0]=(op<<24)|random.nextInt(1<<24);
                    int count=op<0x40?((op&8)!=0?4:3):1;
                    for(int v=0;v<count;v++){
                        if(v>0&&(op&16)!=0)packet[at++]=random.nextInt(1<<24);
                        int x=random.nextInt(768)-128,y=random.nextInt(336)-64;
                        packet[at++]=(x&65535)|((y&65535)<<16);
                        if((op&4)!=0)packet[at++]=random.nextInt(65536)|(v==0?((480<<6)|48)<<16:0);
                    }
                    if(op>=0x60)packet[at++]=(1+random.nextInt(256))|((1+random.nextInt(128))<<16);
                    packet=Arrays.copyOf(packet,at);reference.draw(packet,env);draw(gpu,packet,env);
                }
                gpu.readVram(actual);assertArrayEquals("Clipped/offset framebuffer "+frame,expected,actual);
                int[] state={0x40000,0,baseY,615,3175,30,238,0};Gpu.Frame captured=gpu.capture(state);
                assertEquals(512,captured.width());assertEquals(208,captured.height());
                for(int y=0;y<208;y++)for(int x=0;x<512;x++){
                    int pixel=expected[(baseY+y)*1024+x],r=pixel&31,g=(pixel>>>5)&31,b=(pixel>>>10)&31;
                    int argb=0xff000000|((r<<3)|(r>>>2))<<16|((g<<3)|(g>>>2))<<8|(b<<3)|(b>>>2);
                    assertEquals("Scanout "+frame+" at "+x+","+y,argb,captured.pixels()[y*512+x]);
                }
            }
        }
    }
}
