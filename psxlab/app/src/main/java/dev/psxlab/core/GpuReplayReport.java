package dev.psxlab.core;

import java.io.*;
import java.security.*;
import java.util.*;

/** Lossless attachment inside the existing text report; never passed through log-tail truncation. */
public final class GpuReplayReport {
    public static final String BEGIN="-----BEGIN ARMSX1 GRAPHICS REPLAY-----",END="-----END ARMSX1 GRAPHICS REPLAY-----";
    private static final int MAX_BYTES=8*1024*1024;
    public static void append(Writer writer,File file)throws IOException{
        writer.write("\n===== last captured graphics replay =====\n");
        if(!file.isFile()){writer.write("Absent. Enable logging, reproduce the problem, pause, then export.\n\n");return;}
        byte[] bytes;try(InputStream in=new FileInputStream(file);ByteArrayOutputStream buffer=new ByteArrayOutputStream()){
            byte[] block=new byte[8192];int read;while(buffer.size()<=MAX_BYTES&&(read=in.read(block,0,Math.min(block.length,MAX_BYTES+1-buffer.size())))>0)buffer.write(block,0,read);bytes=buffer.toByteArray();
        }
        if(bytes.length==0||bytes.length>MAX_BYTES){writer.write("Capture unavailable: invalid attachment size.\n\n");return;}
        String digest;try{byte[] hash=MessageDigest.getInstance("SHA-256").digest(bytes);StringBuilder hex=new StringBuilder();for(byte value:hash)hex.append(String.format(Locale.ROOT,"%02x",value&255));digest=hex.toString();}catch(NoSuchAlgorithmException e){throw new IOException(e);}
        writer.write("Format: ArmSX1 graphics replay v3, gzip/base64; bytes="+bytes.length+"; sha256="+digest+"\n");
        writer.write("Capture metadata identifies its session/game; this is the last paused/stopped capture, not a live measurement.\n");
        writer.write("Includes guest RAM, scratchpad, sound RAM and SPU registers at the capture point (game memory only) for offline analysis.\n");
        writer.write(BEGIN+"\n");Base64.Encoder encoder=Base64.getEncoder();
        for(int at=0;at<bytes.length;at+=3072)writer.write(encoder.encodeToString(Arrays.copyOfRange(bytes,at,Math.min(bytes.length,at+3072)))+"\n");
        writer.write(END+"\n\n");
    }
    private GpuReplayReport(){}
}
