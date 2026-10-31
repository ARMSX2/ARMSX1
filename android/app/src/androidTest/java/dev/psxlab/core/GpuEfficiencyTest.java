package dev.psxlab.core;

import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import java.nio.ByteBuffer;
import java.util.Arrays;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

/** Private output buffers: validates allocation policy and real posted pixels. */
@RunWith(AndroidJUnit4.class)
public class GpuEfficiencyTest {
    private static int pixel(ImageReader reader)throws Exception{
        long end=SystemClock.uptimeMillis()+5000;
        while(SystemClock.uptimeMillis()<end){
            try(Image image=reader.acquireLatestImage()){
                if(image!=null){Image.Plane p=image.getPlanes()[0];ByteBuffer bytes=p.getBuffer();int at=image.getHeight()/2*p.getRowStride()+image.getWidth()/2*p.getPixelStride();return 0xff000000|((bytes.get(at)&255)<<16)|((bytes.get(at+1)&255)<<8)|(bytes.get(at+2)&255);}
            }
            SystemClock.sleep(2);
        }
        throw new AssertionError("No presented image");
    }
    @Test public void sharedFramesAvoidCpuUploadAllocation()throws Exception{
        ShaderChain.request(false,"");
        for(int scale:new int[]{1,3,5})try(ImageReader reader=ImageReader.newInstance(320,240,PixelFormat.RGBA_8888,4);HardwareRasterizer raster=new HardwareRasterizer(scale);VulkanPresenter output=new VulkanPresenter(scale)){
            assertEquals(0,output.counters()[6]);assertEquals(0,output.counters()[7]);assertEquals(0,output.counters()[8]);
            raster.fill(0x0000ff,0,320|(240<<16));int[] state={0,0,0,0,3200,0,240,0};
            assertTrue(raster.presentResolved(reader.getSurface(),state,output.nativeHandle()));assertEquals(0xffff0000,pixel(reader));
            if(raster.statistics().contains("sharedFrames=1 ")){
                assertEquals("Shared frames need no upload buffer",0,output.counters()[7]);
            }else assertTrue("CPU fallback still allocates an upload buffer",output.counters()[7]>0);
            android.util.Log.i("GpuEfficiencyTest","scale="+scale+" "+raster.statistics()+" "+output.statistics());
        }
    }
    @Test public void cpuUploadsGrowOnDemandAndReuseTheirAllocation()throws Exception{
        try(ImageReader reader=ImageReader.newInstance(320,240,PixelFormat.RGBA_8888,4);VulkanPresenter output=new VulkanPresenter(5)){
            int[] small=new int[64*64];Arrays.fill(small,0xff00ff00);
            assertEquals(0,output.counters()[7]);assertTrue(output.present(reader.getSurface(),new Gpu.Frame(64,64,small),false));assertEquals(0xff00ff00,pixel(reader));
            long[] first=output.counters();assertTrue(first[6]>=small.length*4L);assertTrue(first[6]<1024*1024);assertEquals(1,first[8]);
            int[] large=new int[256*128];Arrays.fill(large,0xff0000ff);
            assertTrue(output.present(reader.getSurface(),new Gpu.Frame(256,128,large),false));assertEquals(0xff0000ff,pixel(reader));
            long[] grown=output.counters();assertTrue(grown[6]>=large.length*4L);assertEquals(2,grown[8]);
            assertTrue(output.present(reader.getSurface(),new Gpu.Frame(64,64,small),false));assertEquals(0xff00ff00,pixel(reader));
            assertEquals(grown[6],output.counters()[6]);assertEquals(2,output.counters()[8]);
            android.util.Log.i("GpuEfficiencyTest",output.statistics());
        }
        assertTrue(VulkanPresenter.verify().contains("six fixtures"));
    }
}
