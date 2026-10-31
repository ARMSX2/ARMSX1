package dev.psxlab.core;

import org.junit.Test;
import java.util.Map;
import static org.junit.Assert.*;

public class RendererSelectionTest {
    @Test public void presentationIsIndependentOfRasterizer(){
        for(String raster:new String[]{"hardware","software"}){
            RendererSelection selected=RendererSelection.from(Map.of("video.gpu_backend","vulkan","video.renderer",raster));
            assertEquals(2,selected.presentation());assertEquals(raster.equals("hardware"),selected.hardware());
        }
        assertEquals(1,RendererSelection.from(Map.of("video.gpu_backend","opengl","video.renderer","software")).presentation());
        assertEquals(0,RendererSelection.from(Map.of("video.gpu_backend","software","video.renderer","hardware")).presentation());
    }
    @Test public void standaloneLegacyRendererRemainsSupported(){
        assertEquals(2,RendererSelection.from(Map.of("video.renderer","vulkan")).presentation());
        assertEquals(new RendererSelection(1,true),RendererSelection.from(Map.of("video.renderer","hardware")));
        assertEquals(new RendererSelection(0,false),RendererSelection.from(Map.of()));
    }
}
