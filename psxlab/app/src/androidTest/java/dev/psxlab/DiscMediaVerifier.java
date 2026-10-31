package dev.psxlab;
import android.content.Context;
import android.net.Uri;
import android.provider.DocumentsContract;
import dev.psxlab.core.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;

public final class DiscMediaVerifier {
    private static void require(boolean condition,String message){if(!condition)throw new AssertionError(message);}
    private static Uri document(String id){return DocumentsContract.buildDocumentUriUsingTree(Uri.parse("content://"+DiscFixtureProvider.AUTHORITY+"/tree/root"),id);}
    private static void executable(DiscMedia media)throws IOException{require(Arrays.equals(DiscFixtureData.executable(),media.executable),"Boot executable mismatch");try(Gpu gpu=new Gpu(false)){Bus bus=new Bus(gpu);Cpu cpu=ExeLoader.load(media.executable,bus);cpu.step();require(cpu.r[1]==42,"Original executable did not execute");}}
    public static String verify(Context context)throws Exception{
        for(String name:new String[]{"GAME.CUE","EBOOT.PBP","game.chd","game.iso","game.bin","game.img","demo.exe"})require(DiscMedia.supportedName(name),"Library rejected "+name);require(!DiscMedia.supportedName("notes.txt"),"Library accepted a non-game");
        try(DiscMedia media=DiscMedia.open(context,document("cue-17"),"game.cue")){executable(media);require(media.disc.lastTrack()==3&&media.disc.trackStart(2)==32&&media.disc.trackStart(3)==34&&media.disc.sectorCount()==36,"CUE TOC incorrect");require(media.disc.audioTrack(32),"CDDA metadata missing");}
        try(DiscMedia media=DiscMedia.open(context,document("cue-18"),"nested.cue")){executable(media);}
        Set<Uri> tracks=DiscMedia.referencedTracks(context,document("cue-17"),document("root"));require(tracks.equals(new HashSet<>(Arrays.asList(document("data-30"),document("audio-31"),document("audio-32")))),"Library CUE track filtering incorrect");
        for(String id:new String[]{"pbp-88","iso-15","exe-33"})try(DiscMedia media=DiscMedia.open(context,document(id),"no-extension")){executable(media);}
        int before=new File("/proc/self/fd").list().length;for(int i=0;i<12;i++)try(DiscMedia ignored=DiscMedia.open(context,document("cue-17"),"game.cue",document("root"))){}int after=new File("/proc/self/fd").list().length;require(after<=before+8,"Track descriptors leaked: "+before+" -> "+after);
        try(DiscMedia ignored=DiscMedia.open(context,document("bad-77"),"missing.cue")){throw new AssertionError("Missing BIN accepted");}catch(FileNotFoundException expected){require(expected.getMessage().contains("missing.bin"),"Missing-track error not actionable");}
        File directory=new File(context.getCacheDir(),"local-cue-check");require(directory.mkdirs()||directory.isDirectory(),"Cannot create fixture directory");
        try{Files.write(new File(directory,"Track One.bin").toPath(),DiscFixtureData.raw());Files.write(new File(directory,"MusicA.bin").toPath(),new byte[2352*2]);Files.write(new File(directory,"MusicB.bin").toPath(),new byte[2352*2]);File cue=new File(directory,"game.cue");Files.write(cue.toPath(),DiscFixtureData.cue().getBytes(StandardCharsets.UTF_8));try(DiscMedia media=DiscMedia.open(context,Uri.fromFile(cue),"game.cue")){executable(media);require(media.disc.lastTrack()==3,"Local CUE metadata missing");}}
        finally{for(File file:directory.listFiles())require(file.delete(),"Could not delete fixture");require(directory.delete(),"Could not delete fixture directory");}
        return "PASS: CUE/BIN, nested/case-matched tracks with opaque SAF IDs, PBP signature detection, ISO/EXE regressions, executable execution, track filtering and descriptor cleanup.\n";
    }
}
