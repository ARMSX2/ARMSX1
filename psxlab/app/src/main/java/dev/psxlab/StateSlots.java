package dev.psxlab;
import android.content.Context;
import java.io.*;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.security.*;
final class StateSlots {
    static File file(Context c,String game,int slot){if(slot<1 || slot>5)throw new IllegalArgumentException("State slot");try{byte[] bytes=MessageDigest.getInstance("SHA-256").digest(game.getBytes(StandardCharsets.UTF_8));StringBuilder key=new StringBuilder();for(byte b:bytes)key.append(String.format(java.util.Locale.ROOT,"%02x",b&255));return new File(new File(c.getFilesDir(),"states"),key+"-"+slot+".psxs");}catch(NoSuchAlgorithmException e){throw new AssertionError(e);}}
    static void write(File target,byte[] bytes)throws IOException{File dir=target.getParentFile();if(!dir.isDirectory() && !dir.mkdirs())throw new IOException("Cannot create state folder");File temp=new File(target.getPath()+".tmp");try(FileOutputStream out=new FileOutputStream(temp)){out.write(bytes);out.getFD().sync();}Files.move(temp.toPath(),target.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);}
}
