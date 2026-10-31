package dev.psxlab;
import android.content.Context;
import android.util.AtomicFile;
import java.io.*;
import java.security.MessageDigest;
import java.util.Locale;

public final class BiosStore {
    public static void importBios(Context context,InputStream in,String name)throws Exception{
        byte[] bytes=new byte[512*1024];int at=0;
        while(at<bytes.length){int n=in.read(bytes,at,bytes.length-at);if(n<0)throw new IOException("BIOS is smaller than 512 KiB");at+=n;}
        if(in.read()!=-1)throw new IOException("BIOS is larger than 512 KiB");
        boolean blank=true;for(byte b:bytes)if(b!=bytes[0]){blank=false;break;}if(blank)throw new IOException("BIOS contains no varying data");
        StringBuilder hash=new StringBuilder();for(byte b:MessageDigest.getInstance("SHA-256").digest(bytes))hash.append(String.format(Locale.ROOT,"%02x",b&255));
        AtomicFile file=new AtomicFile(new File(context.getFilesDir(),"bios.bin"));FileOutputStream out=null;
        try{out=file.startWrite();out.write(bytes);file.finishWrite(out);}catch(IOException e){file.failWrite(out);throw e;}
        context.getSharedPreferences("BiosActivity",0).edit().putString("name",name).putString("sha",hash.toString()).commit();
    }
}
