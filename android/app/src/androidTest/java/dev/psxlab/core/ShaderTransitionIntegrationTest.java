package dev.psxlab.core;

import androidx.test.platform.app.InstrumentationRegistry;
import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import java.io.File;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.junit.Test;
import static org.junit.Assert.*;

/** Exercises the live shader across FMV, menu, interlaced and HD scanout changes. */
public class ShaderTransitionIntegrationTest {
    private static String preset(boolean history)throws Exception{
        File folder=new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getCacheDir(),"shader-transition-tests");assertTrue(folder.isDirectory()||folder.mkdirs());
        String name=history?"history":"copy";
        String shader="#version 450\nlayout(push_constant) uniform P { mat4 MVP; } p;\n#pragma stage vertex\nlayout(location=0) in vec4 Position;layout(location=1) in vec2 TexCoord;layout(location=0) out vec2 uv;void main(){gl_Position=p.MVP*Position;uv=TexCoord;}\n#pragma stage fragment\nlayout(location=0) in vec2 uv;layout(location=0) out vec4 FragColor;layout(binding=2) uniform sampler2D Source;\n"+
            (history?"layout(binding=3) uniform sampler2D OriginalHistory1;\n":"")+
            "void main(){vec3 color=texture(Source,uv).rgb;"+(history?"color=(color+texture(OriginalHistory1,uv).rgb)*0.5;":"")+"FragColor=vec4(color,1.0);}";
        File source=new File(folder,name+".slang"),preset=new File(folder,name+".slangp");
        Files.write(source.toPath(),shader.getBytes(StandardCharsets.UTF_8));Files.write(preset.toPath(),("shaders=1\nshader0="+name+".slang\nfilter_linear0=false\n").getBytes(StandardCharsets.UTF_8));return preset.getAbsolutePath();
    }
    private static int[] image(ImageReader reader)throws Exception{
        Image image=null;long deadline=android.os.SystemClock.uptimeMillis()+3000;
        while(image==null&&android.os.SystemClock.uptimeMillis()<deadline){image=reader.acquireLatestImage();if(image==null)Thread.sleep(1);}assertNotNull("Presented image",image);
        try{Image.Plane plane=image.getPlanes()[0];ByteBuffer data=plane.getBuffer();int[] result=new int[image.getWidth()*image.getHeight()];
            for(int y=0;y<image.getHeight();y++)for(int x=0;x<image.getWidth();x++){int at=y*plane.getRowStride()+x*plane.getPixelStride();result[y*image.getWidth()+x]=0xff000000|((data.get(at)&255)<<16)|((data.get(at+1)&255)<<8)|(data.get(at+2)&255);}return result;
        }finally{image.close();}
    }
    private static int[] state(int mode){
        int width=mode==0?384:mode==1?640:mode==2?256:320,height=mode==1?480:240;
        int status=(mode==0||mode==4?0x200000:0)|(mode==1?0x4e0000:mode==2?0:0x20000),divider=mode==1?4:mode==2?10:8;
        return new int[]{status,0,0,600,600+width*divider,16,16+height/(height==480?2:1),0};
    }
    private static int[] ram(int[] state,int color,int pattern){
        int width=GpuReplay.picture(new int[GpuReplay.VRAM_WORDS],state).width(),height=GpuReplay.picture(new int[GpuReplay.VRAM_WORDS],state).height();int[] ram=new int[GpuReplay.VRAM_WORDS];
        for(int y=0;y<height;y++)for(int x=0;x<width;x++){
            int rgb=pattern<0?color:switch((x/(width/4)+y/(height/4)+pattern)%3){case 0->0xff0000;case 1->0x00ff00;default->0x0000ff;};
            if((state[0]&0x200000)!=0){for(int c=0;c<3;c++){int at=x*3+c;ram[y*1024+at/2]|=((rgb>>((2-c)*8))&255)<<((at&1)*8);}}
            else ram[y*1024+x]=((rgb>>19)&31)|(((rgb>>11)&31)<<5)|(((rgb>>3)&31)<<10);
        }return ram;
    }
    private static void activate(String preset){ShaderChain.backend(2);ShaderChain.request(true,preset);ShaderChain.phase(1,1,60,4f/3);ShaderChain.prepare();assertTrue(ShaderChain.status(),ShaderChain.active());}
    private static HardwareRasterizer raster(int scale,VulkanPresenter output){
        try{return new VulkanHardwareRasterizer(scale,output);}
        catch(IllegalStateException error){
            // Follow production's HD compatibility path on GPUs without ordered
            // Vulkan color attachments; still exercise the shared shader path.
            if(scale==1||error.getMessage()==null||!error.getMessage().contains("Ordered Vulkan HD unavailable"))throw error;
            return new HardwareRasterizer(scale);
        }
    }

    @Test public void activeShaderSurvivesMovieAndMenuDisplayFormatChanges()throws Exception{
        ShaderChain.request(false,"");String preset=preset(false);
        for(int scale:new int[]{1,3})try(VulkanPresenter output=new VulkanPresenter(scale);HardwareRasterizer gpu=raster(scale,output);ImageReader reader=ImageReader.newInstance(320,240,PixelFormat.RGBA_8888,3)){
            output.configureDisplay(4f/3f,true,false);activate(preset);
            for(int trial=0;trial<24;trial++){
                int[] state=state(trial%6),ram=ram(state,0,trial);gpu.setVram(ram);assertTrue(gpu.presentResolved(reader.getSurface(),state,output.nativeHandle()));int[] actual=image(reader);GpuReplay.Picture expected=GpuReplay.picture(ram,state);
                for(int y=15;y<240;y+=30)for(int x=20;x<320;x+=40){int want=expected.pixels()[(y*expected.height()/240)*expected.width()+x*expected.width()/320];assertEquals("Scale "+scale+" transition "+trial+" at "+x+","+y+" "+ShaderChain.status(),want,actual[y*320+x]);}
                assertTrue("Shader must remain active across transition "+trial+": "+ShaderChain.status(),ShaderChain.active());
            }
        }finally{ShaderChain.request(false,"");}
    }

    @Test public void historyShadersRefreshTheirInputAndClearOldSizedMovieFrames()throws Exception{
        ShaderChain.request(false,"");String preset=preset(true);
        for(int scale:new int[]{1,3})try(VulkanPresenter output=new VulkanPresenter(scale);HardwareRasterizer gpu=raster(scale,output);ImageReader reader=ImageReader.newInstance(320,240,PixelFormat.RGBA_8888,3)){
            output.configureDisplay(4f/3f,true,false);activate(preset);int previous=0,lastWidth=0,lastHeight=0;
            for(int trial=0;trial<30;trial++){
                int[] state=state((trial/2)%6);int color=switch(trial%3){case 0->0xff0000;case 1->0x00ff00;default->0x0000ff;};int[] ram=ram(state,color,-1);GpuReplay.Picture expected=GpuReplay.picture(ram,state);
                if(expected.width()!=lastWidth||expected.height()!=lastHeight)previous=0;
                gpu.setVram(ram);assertTrue(gpu.presentResolved(reader.getSurface(),state,output.nativeHandle()));int actual=image(reader)[120*320+160];
                for(int shift:new int[]{0,8,16}){int want=(((color>>shift)&255)+((previous>>shift)&255))/2;assertTrue("History scale "+scale+" transition "+trial+" channel "+shift+" expected "+want+" got "+((actual>>shift)&255),Math.abs(want-((actual>>shift)&255))<=1);}
                assertTrue(ShaderChain.status(),ShaderChain.active());previous=color;lastWidth=expected.width();lastHeight=expected.height();
            }
        }finally{ShaderChain.request(false,"");}
    }
}
