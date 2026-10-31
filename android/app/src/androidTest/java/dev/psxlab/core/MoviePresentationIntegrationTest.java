package dev.psxlab.core;

import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.SystemClock;
import java.nio.ByteBuffer;
import java.util.*;
import org.junit.Test;
import static org.junit.Assert.*;

public class MoviePresentationIntegrationTest {
    private static int[] image(ImageReader reader)throws Exception{
        long end=SystemClock.uptimeMillis()+5000;
        while(SystemClock.uptimeMillis()<end){try(Image image=reader.acquireLatestImage()){
            if(image!=null){int[] pixels=new int[image.getWidth()*image.getHeight()];Image.Plane p=image.getPlanes()[0];ByteBuffer bytes=p.getBuffer();
                for(int y=0;y<image.getHeight();y++)for(int x=0;x<image.getWidth();x++){int at=y*p.getRowStride()+x*p.getPixelStride();pixels[y*image.getWidth()+x]=0xff000000|((bytes.get(at)&255)<<16)|((bytes.get(at+1)&255)<<8)|(bytes.get(at+2)&255);}return pixels;}
        }SystemClock.sleep(2);}throw new AssertionError("No movie frame");
    }
    private static void byteAt(int[] ram,int x,int y,int value){int at=(y&511)*1024+((x>>>1)&1023),shift=(x&1)*8;ram[at]=(ram[at]&~(255<<shift))|(value<<shift);}
    @Test public void movieFilteringAndLastRowsMatchNativeAcrossHdAndModeChanges()throws Exception{
        ShaderChain.request(false,"");int[] ram=new int[1024*512];Arrays.fill(ram,0x7c1f);
        for(int y=0;y<48;y++)for(int x=0;x<64;x++){int at=1004*2+x*3;byteAt(ram,at,490+y,(x*37+y*3)&255);byteAt(ram,at+1,490+y,(y*61+x*5)&255);byteAt(ram,at+2,490+y,((x^y)*53)&255);}
        for(int api:new int[]{1,2})try(HardwareRasterizer nativeGpu=new HardwareRasterizer(1);HardwareRasterizer hd=new HardwareRasterizer(4);ImageReader nr=ImageReader.newInstance(192,144,PixelFormat.RGBA_8888,4);ImageReader hr=ImageReader.newInstance(192,144,PixelFormat.RGBA_8888,4);VulkanPresenter np=api==2?new VulkanPresenter(1):null;VulkanPresenter hp=api==2?new VulkanPresenter(4):null){
            for(boolean filter:new boolean[]{false,true})for(int mode:new int[]{0,0x200000,0,0x200000}){
                int[] state={mode,1004,490,0,640,0,48,filter?1:0};nativeGpu.setVram(ram);hd.setVram(ram);
                assertTrue(api==2?nativeGpu.presentResolved(nr.getSurface(),state,np.nativeHandle()):nativeGpu.present(nr.getSurface(),state));int[] expected=image(nr);
                assertTrue(api==2?hd.presentResolved(hr.getSurface(),state,hp.nativeHandle()):hd.present(hr.getSurface(),state));int[] actual=image(hr);
                if(mode!=0){int mismatch=Arrays.mismatch(expected,actual);assertTrue("24-bit api="+api+" filter="+filter+" mismatch at "+(mismatch%192)+","+(mismatch/192),mismatch==-1);}
            }
        }
    }
}
