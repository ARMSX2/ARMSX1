package dev.psxlab.core;

import java.awt.image.BufferedImage;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import javax.imageio.ImageIO;

/** Offline command/VRAM comparison using the engine's independent Java rasterizer. */
public final class ReplayGpu {
    private static void png(Path path,GpuReplay.Picture picture)throws IOException{
        BufferedImage image=new BufferedImage(picture.width(),picture.height(),BufferedImage.TYPE_INT_ARGB);
        image.setRGB(0,0,picture.width(),picture.height(),picture.pixels(),0,picture.width());ImageIO.write(image,"png",path.toFile());
    }
    public static void main(String[] args)throws Exception{
        if(args.length!=2)throw new IllegalArgumentException("Usage: ReplayGpu capture.armgfx output-directory");
        Path out=Path.of(args[1]);Files.createDirectories(out);GpuReplay.Document document=GpuReplay.read(new File(args[0]));GpuReplay.Capture capture=document.capture();int[] expected=GpuReplay.replay(capture);
        GpuReplay.Picture actualPicture=GpuReplay.picture(capture.after(),capture.display()),expectedPicture=GpuReplay.picture(expected,capture.display());int[] difference=new int[actualPicture.pixels().length];int visible=0,vram=0;
        for(int i=0;i<expected.length;i++)if(expected[i]!=capture.after()[i])vram++;
        for(int i=0;i<difference.length;i++){boolean same=actualPicture.pixels()[i]==expectedPicture.pixels()[i];difference[i]=same?0xff000000:0xffff3030;if(!same)visible++;}
        png(out.resolve("captured.png"),actualPicture);png(out.resolve("replayed.png"),expectedPicture);png(out.resolve("difference.png"),new GpuReplay.Picture(actualPicture.width(),actualPicture.height(),difference));
        int[] wholeVram={0,0,0,0,10240,0,512,0};png(out.resolve("captured-vram.png"),GpuReplay.picture(capture.after(),wholeVram));png(out.resolve("replayed-vram.png"),GpuReplay.picture(expected,wholeVram));
        Files.writeString(out.resolve("metadata.json"),document.metadata());
        // Raw little-endian guest memory: ram.bin is 0x80000000-0x801FFFFF, scratchpad.bin 0x1F800000-0x1F8003FF.
        if(document.ram()!=null){Files.write(out.resolve("ram.bin"),document.ram());Files.write(out.resolve("scratchpad.bin"),document.scratchpad());}
        String summary="Captured frames "+capture.firstFrame()+".."+capture.lastFrame()+", operations "+capture.operations().size()+"\nVRAM halfword differences: "+vram+" / "+GpuReplay.VRAM_WORDS+"\nVisible pixel differences: "+visible+" / "+difference.length+"\nDisplay: "+Arrays.toString(capture.display())+"\nDrawing environment: "+Arrays.toString(capture.environment())+"\nGuest memory: "+(document.ram()==null?"not recorded (version-1 capture)":"ram.bin (2 MiB) and scratchpad.bin (1 KiB) written")+"\nComparison is the original Java rasterizer against captured canonical VRAM. Texture feedback, HD detail, texture replacements, and driver/presentation behavior require separate interpretation. A mismatch is evidence to investigate, not proof of the game's root cause.\n";
        Files.writeString(out.resolve("comparison.txt"),summary);
        try(PrintWriter writer=new PrintWriter(Files.newBufferedWriter(out.resolve("commands.txt")))){
            int index=0;for(GpuReplay.Operation operation:capture.operations()){
                writer.println("operation "+index+++" kind="+operation.kind()+" words="+operation.words().length);
                for(int word:operation.words())writer.printf(Locale.ROOT,"%08X ",word);writer.println();
                if(operation.kind()==GpuReplay.BATCH){int[] words=operation.words();for(int at=0;at<words.length;){int count=words[at++],env=at+count,offset=words[env+5];
                    writer.printf(Locale.ROOT,"  GP0 %02X draw area %d,%d..%d,%d offset %d,%d page=%04X mask=%d%n",words[at]>>>24,words[env+3]&1023,(words[env+3]>>>10)&511,words[env+4]&1023,(words[env+4]>>>10)&511,(offset<<21)>>21,((offset>>>11)<<21)>>21,words[env+1],words[env+6]);at+=count+7;
                }}
            }
        }
        System.out.print(summary);System.out.println("Wrote replay images and commands to "+out.toAbsolutePath());
    }
}
