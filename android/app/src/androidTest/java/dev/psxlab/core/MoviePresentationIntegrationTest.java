package dev.psxlab.core;

import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.SystemClock;
import java.nio.ByteBuffer;
import java.util.*;
import java.lang.reflect.Field;
import org.junit.Test;
import static org.junit.Assert.*;

public class MoviePresentationIntegrationTest {
    @Test public void croppedBorderIsRemovedAcrossSoftwareOpenGlVulkanAndHd()throws Exception{
        ShaderChain.request(false,"");Field field=Gpu.class.getDeclaredField("vram");field.setAccessible(true);
        for(int api:new int[]{0,1,2})for(int scale:new int[]{1,4})
            try(Gpu gpu=new Gpu(true,api,scale,api!=0);ImageReader reader=ImageReader.newInstance(192,144,PixelFormat.RGBA_8888,4)){
                gpu.configureDisplay(4f/3f,false,false);
                for(boolean rgb24:new boolean[]{false,true}){
                    gpu.sync();int[] ram=(int[])field.get(gpu);Arrays.fill(ram,0);
                    for(int y=0;y<244;y++)for(int x=0;x<320;x++){
                        boolean stale=y>=240;
                        if(rgb24){byteAt(ram,x*3,y,stale?255:0);byteAt(ram,x*3+1,y,stale?0:255);byteAt(ram,x*3+2,y,stale?255:0);}
                        else ram[y*1024+x]=stale?0x7c1f:0x3e0;
                    }
                    gpu.restoreSnapshot();gpu.control(rgb24?0x08000011:0x08000001);gpu.control(0x05001000);
                    gpu.control(0x06000000|615|(3175<<12));gpu.control(0x07000000|16|(256<<10));gpu.control(0x03000000);
                    for(boolean filter:new boolean[]{false,true})for(int crop:new int[]{0,1,2,0}){
                        gpu.configureCrop(crop);gpu.requestSurfaceFrame(reader.getSurface(),filter,frame->{if(frame!=null)throw new AssertionError("Texture presenter unavailable");});gpu.sync();
                        int[] pixels=image(reader);String context="api="+api+" scale="+scale+" rgb24="+rgb24+" crop="+crop+" filter="+filter;
                        assertEquals(context,0xff00ff00,pixels[72*192+96]);
                        assertEquals(context,crop==0?0xffff00ff:0xff00ff00,pixels[143*192+96]);
                        if(crop!=0)for(int p:pixels)assertEquals(context,0xff00ff00,p);
                    }
                }
            }
    }
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
