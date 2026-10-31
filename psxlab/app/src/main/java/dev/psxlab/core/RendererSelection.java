package dev.psxlab.core;

import java.util.Map;

/** Presentation API and guest rasterizer are independent settings. */
public record RendererSelection(int presentation, boolean hardware) {
    public static RendererSelection from(Map<String,String> options) {
        String raster=options.getOrDefault("video.renderer","software");
        String output=options.get("video.gpu_backend");
        // Standalone/older configs have only renderer; retain their meaning.
        if(output==null)output=raster.equals("hardware")?"opengl":raster;
        int api=switch(output){case "vulkan"->2;case "opengl","angle","sdl-accelerated"->1;default->0;};
        return new RendererSelection(api,raster.equals("hardware"));
    }
}
