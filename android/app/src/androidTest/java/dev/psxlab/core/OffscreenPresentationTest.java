package dev.psxlab.core;

import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.nio.ByteBuffer;
import java.io.*;
import java.nio.file.*;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

/** Consumes window buffers privately, without flashing the physical display. */
@RunWith(AndroidJUnit4.class)
public class OffscreenPresentationTest {
    private static int[] lastSamples;
    private static int pixel(ImageReader reader){
        Image image=null;long end=SystemClock.uptimeMillis()+3000;
        while(image==null&&SystemClock.uptimeMillis()<end){image=reader.acquireLatestImage();if(image==null)SystemClock.sleep(10);}
        assertNotNull("Posted buffer",image);
        try{Image.Plane plane=image.getPlanes()[0];ByteBuffer bytes=plane.getBuffer();lastSamples=new int[4];int[][] points={{4,4},{1,1},{7,1},{1,7}};
            for(int i=0;i<points.length;i++){int offset=(image.getHeight()*points[i][1]/8)*plane.getRowStride()+(image.getWidth()*points[i][0]/8)*plane.getPixelStride();lastSamples[i]=((bytes.get(offset)&255)<<16)|((bytes.get(offset+1)&255)<<8)|(bytes.get(offset+2)&255);}return lastSamples[0];}finally{image.close();}
    }
    @Test public void vulkanHdRestoresStaticImageAfterBlack()throws Exception{
        assertEquals("true",InstrumentationRegistry.getArguments().getString("effectsAudit"));ShaderChain.request(false,"");
        try(ImageReader reader=ImageReader.newInstance(640,480,PixelFormat.RGBA_8888,4);Gpu gpu=new Gpu(false,2,3)){
            gpu.control(0x03000000);gpu.control(0x05000000);gpu.control(0x06000000|(3200<<12));gpu.control(0x07000000|(240<<10));gpu.write(0x02c0c0c0);gpu.write(0);gpu.write(320|(240<<16));
            gpu.requestSurfaceFrame(reader.getSurface(),false,value->assertNull(value));assertNotEquals(0,pixel(reader));
            gpu.configurePresentation(1,2,120,true);gpu.requestSurfaceFrame(reader.getSurface(),false,value->assertNull(value));assertEquals(1,gpu.blackFramesPresented());SystemClock.sleep(40);assertEquals(0,pixel(reader));
            gpu.restorePresentation(reader.getSurface(),false,value->assertNull(value));assertNotEquals("Pause restores the static image",0,pixel(reader));
            gpu.configurePresentation(1,2,60,true);gpu.requestSurfaceFrame(reader.getSurface(),false,value->assertNull(value));assertEquals(1,gpu.blackFramesPresented());
            gpu.configurePresentation(1,2,120,false);gpu.requestSurfaceFrame(reader.getSurface(),false,value->assertNull(value));assertEquals(1,gpu.blackFramesPresented());
            gpu.configurePresentation(1,2,120,true);gpu.requestSurfaceFrame(reader.getSurface(),false,value->assertNull(value));SystemClock.sleep(40);assertEquals(0,pixel(reader));
            gpu.configurePresentation(0,2,120,true);gpu.requestSurfaceFrame(reader.getSurface(),false,value->assertNull(value));assertNotEquals("Off restores the static image",0,pixel(reader));
        }
    }
}
