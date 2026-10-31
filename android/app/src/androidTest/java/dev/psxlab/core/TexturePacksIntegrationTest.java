package dev.psxlab.core;

import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.SystemClock;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.file.*;
import java.util.*;
import org.junit.Test;
import static org.junit.Assert.*;

/** Original generated images, no commercial game assets. Exercises real GPU output. */
public class TexturePacksIntegrationTest {
    private static File folder()throws IOException{return Files.createTempDirectory(InstrumentationRegistry.getInstrumentation().getTargetContext().getCacheDir().toPath(),"texture-audit-").toFile();}
    private static void remove(File root)throws IOException{try(var paths=Files.walk(root.toPath())){for(Path p:paths.sorted(Comparator.reverseOrder()).collect(java.util.stream.Collectors.toList()))Files.delete(p);}}
    private static void png(File file,int w,int h,int color)throws IOException{
        Bitmap bitmap=Bitmap.createBitmap(w,h,Bitmap.Config.ARGB_8888);bitmap.eraseColor(color);
        try(OutputStream out=new FileOutputStream(file)){assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG,100,out));}finally{bitmap.recycle();}
    }
    private static void upload(Gpu gpu,int color){gpu.write(0xa0000000);gpu.write(640|(256<<16));gpu.write(4|(4<<16));for(int i=0;i<8;i++)gpu.write(color|(color<<16));}
    private static void draw(Gpu gpu){
        gpu.control(0x03000000);gpu.control(0x05000000);gpu.control(0x06000000|(40<<12));gpu.control(0x07000000|(4<<10));
        gpu.write(0xe1000000|0x11a);gpu.write(0xe3000000);gpu.write(0xe4000000|1023|(511<<10));
        gpu.write(0x65000000);gpu.write(0);gpu.write(0);gpu.write(4|(4<<16));gpu.sync();
    }
    private static int posted(ImageReader reader)throws Exception{
        long end=SystemClock.uptimeMillis()+5000;
        while(SystemClock.uptimeMillis()<end){try(Image image=reader.acquireLatestImage()){
            if(image!=null){Image.Plane plane=image.getPlanes()[0];ByteBuffer b=plane.getBuffer();int at=image.getHeight()/4*plane.getRowStride()+image.getWidth()/4*plane.getPixelStride();return 0xff000000|((b.get(at)&255)<<16)|((b.get(at+1)&255)<<8)|(b.get(at+2)&255);}
        }SystemClock.sleep(2);}throw new AssertionError("No posted texture frame");
    }
    @Test public void dumpRoundTripAndWorkerCleanup()throws Exception{
        assertTrue(NativeRasterizer.AVAILABLE);File root=folder();
        try{
            short[] original=new short[65536];for(int i=0;i<original.length;i++)original[i]=(short)i;
            int[] words=new int[32768];for(int i=0;i<words.length;i++)words[i]=(original[i*2]&65535)|(original[i*2+1]<<16);
            try(TexturePacks service=new TexturePacks(true,false,root.toString(),null)){
                for(int i=0;i<words.length;i+=512)service.upload(Arrays.copyOfRange(words,i,i+512),i*2,256,65536,0);
                long end=SystemClock.uptimeMillis()+10000;while(service.metrics()[3]==0&&SystemClock.uptimeMillis()<end)SystemClock.sleep(5);
                assertEquals(service.status(),1,service.metrics()[3]);
                File dump=new File(root,"dump/vram-write-"+TexturePacks.hashNative(original)+".png");assertTrue(dump.toString(),dump.isFile());
                TexturePacks.Image image=TexturePacks.decode(dump,256,256);
                for(int i=0;i<65536;i++){int p=image.argb()[i],alpha=p>>>24,value=alpha==0?0:((p>>>19)&31)|(((p>>>11)&31)<<5)|(((p>>>3)&31)<<10)|(alpha<192?32768:0);assertEquals("VRAM word "+i,i,value);}
            }
            long end=SystemClock.uptimeMillis()+2000;while(Thread.getAllStackTraces().keySet().stream().anyMatch(t->t.isAlive()&&t.getName().equals("PSX-TextureIO"))&&SystemClock.uptimeMillis()<end)SystemClock.sleep(5);
            assertFalse(Thread.getAllStackTraces().keySet().stream().anyMatch(t->t.isAlive()&&t.getName().equals("PSX-TextureIO")));
        }finally{remove(root);}
    }
    @Test public void replacementsChangePostedPixelsWithoutChangingGuestVram()throws Exception{
        assertTrue(NativeRasterizer.AVAILABLE);ShaderChain.request(false,"");File root=folder();
        try{
            File replacements=new File(root,"replacements");assertTrue(replacements.mkdir());short[] source=new short[16];Arrays.fill(source,(short)31);
            png(new File(replacements,"vram-write-"+TexturePacks.hashNative(source)+".png"),12,12,0xff00ff00);
            for(int output:new int[]{1,2})for(int scale:new int[]{1,3})try(Gpu gpu=new Gpu(false,output,scale,true);ImageReader reader=ImageReader.newInstance(64,64,PixelFormat.RGBA_8888,4)){
                gpu.configureTextures(false,true,root.toString());upload(gpu,31);draw(gpu);
                assertEquals(gpu.textureStatus(),1,gpu.textureMetrics()[4]);assertTrue(gpu.textureMetrics()[5]<=TexturePacks.CACHE_BYTES);
                assertEquals("Enhanced capture",0xff00ff00,gpu.displayFrame().pixels()[0]);
                gpu.requestSurfaceFrame(reader.getSurface(),false,frame->{});gpu.sync();assertEquals("output="+output+" scale="+scale,0xff00ff00,posted(reader));
                assertEquals("Canonical guest VRAM stays red",0xffff0000,gpu.snapshot()[0]);
                gpu.configureTextures(false,false,root.toString());
                assertEquals(0,gpu.textureMetrics()[5]);assertEquals(0xffff0000,gpu.displayFrame().pixels()[0]);
                // Repeated enable/disable must recreate and release the optional 1× HD layer.
                for(int i=0;i<3;i++){gpu.configureTextures(false,true,root.toString());upload(gpu,31);draw(gpu);assertEquals(0xff00ff00,gpu.displayFrame().pixels()[0]);gpu.configureTextures(false,false,root.toString());}
            }
        }finally{remove(root);}
    }
    @Test public void higherResolutionPackKeepsItsSubpixelTextureDetail()throws Exception{
        assertTrue(NativeRasterizer.AVAILABLE);File root=folder();
        try{
            File replacements=new File(root,"replacements");assertTrue(replacements.mkdir());short[] source=new short[16];Arrays.fill(source,(short)31);
            File file=new File(replacements,"vram-write-"+TexturePacks.hashNative(source)+".png");
            int[] colors={0xffff0000,0xff00ff00,0xff0000ff},pixels=new int[12*12];for(int i=0;i<pixels.length;i++)pixels[i]=colors[i%3];
            Bitmap bitmap=Bitmap.createBitmap(12,12,Bitmap.Config.ARGB_8888);bitmap.setPixels(pixels,0,12,0,0,12,12);
            try(OutputStream out=new FileOutputStream(file)){assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG,100,out));}finally{bitmap.recycle();}
            try(Gpu gpu=new Gpu(false,1,3,true)){
                gpu.configureTextures(false,true,root.toString());upload(gpu,31);draw(gpu);Gpu.Frame frame=gpu.displayFrame();assertEquals(12,frame.width());
                for(int x=0;x<12;x++)assertEquals("HD replacement sample "+x,colors[x%3],frame.pixels()[x]);
            }
        }finally{remove(root);}
    }
    @Test public void invalidPacksAndSoftwareRequirementsAreVisible()throws Exception{
        assertTrue(NativeRasterizer.AVAILABLE);File root=folder();
        try{
            try(Gpu software=new Gpu(false)){software.configureTextures(false,true,root.toString());assertTrue(software.textureStatus(),software.textureStatus().contains("require hardware"));}
            File replacements=new File(root,"replacements");assertTrue(replacements.mkdir());short[] source=new short[16];Arrays.fill(source,(short)31);
            png(new File(replacements,"vram-write-"+TexturePacks.hashNative(source)+".png"),13,12,0xff00ff00);
            try(Gpu gpu=new Gpu(false,1,1,true)){gpu.configureTextures(false,true,root.toString());upload(gpu,31);draw(gpu);assertEquals(1,gpu.textureMetrics()[6]);assertTrue(gpu.textureStatus(),gpu.textureStatus().contains("uniform"));assertEquals(0xffff0000,gpu.displayFrame().pixels()[0]);}
        }finally{remove(root);}
    }
}
