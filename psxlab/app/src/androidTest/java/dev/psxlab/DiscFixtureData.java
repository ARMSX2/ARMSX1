package dev.psxlab;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.zip.Deflater;

/** Original tiny ISO/executable fixtures; no BIOS or commercial game content. */
public final class DiscFixtureData {
    private static void word(byte[] bytes,int at,int value){for(int i=0;i<4;i++)bytes[at+i]=(byte)(value>>>(i*8));}
    private static int entry(byte[] bytes,int at,String name,int sector,int length,int flags){byte[] n=name.getBytes(StandardCharsets.US_ASCII);int size=33+n.length+(n.length%2==0?1:0);bytes[at]=(byte)size;word(bytes,at+2,sector);word(bytes,at+10,length);bytes[at+25]=(byte)flags;bytes[at+32]=(byte)n.length;System.arraycopy(n,0,bytes,at+33,n.length);return size;}
    public static byte[] executable(){byte[] bytes=new byte[4096];System.arraycopy("PS-X EXE".getBytes(StandardCharsets.US_ASCII),0,bytes,0,8);word(bytes,16,0x80010000);word(bytes,24,0x80010000);word(bytes,28,2048);word(bytes,2048,0x2401002a);word(bytes,2052,0x08004001);return bytes;}
    public static byte[] iso(){byte[] bytes=new byte[32*2048];int p=16*2048;bytes[p]=1;System.arraycopy("CD001".getBytes(StandardCharsets.US_ASCII),0,bytes,p+1,5);bytes[p+6]=1;entry(bytes,p+156,"\0",20,2048,2);String cnf="BOOT = cdrom:\\SLUS_999.99;1\r\n";p=20*2048;p+=entry(bytes,p,"SYSTEM.CNF;1",21,cnf.length(),0);entry(bytes,p,"SLUS_999.99;1",22,4096,0);System.arraycopy(cnf.getBytes(StandardCharsets.US_ASCII),0,bytes,21*2048,cnf.length());System.arraycopy(executable(),0,bytes,22*2048,4096);return bytes;}
    public static byte[] raw(){byte[] cooked=iso(),raw=new byte[32*2352];for(int i=0;i<32;i++){raw[i*2352+15]=2;System.arraycopy(cooked,i*2048,raw,i*2352+24,2048);}return raw;}
    public static String cue(){return "FILE \"Track One.bin\" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\nFILE MusicA.bin BINARY\nTRACK 02 AUDIO\nINDEX 01 00:00:00\nFILE MusicB.bin BINARY\nTRACK 03 AUDIO\nINDEX 01 00:00:00\n";}
    public static byte[] pbp(){byte[] raw=raw();byte[][] parts=new byte[2][];int size=0;for(int i=0;i<2;i++){Deflater encoder=new Deflater(6,true);try{encoder.setInput(raw,i*37632,37632);encoder.finish();byte[] buffer=new byte[40000];int count=encoder.deflate(buffer);if(!encoder.finished())throw new AssertionError();parts[i]=Arrays.copyOf(buffer,count);size+=count;}finally{encoder.end();}}int base=256;byte[] bytes=new byte[base+0x100000+size];bytes[1]='P';bytes[2]='B';bytes[3]='P';word(bytes,36,base);System.arraycopy("PSISOIMG0000".getBytes(StandardCharsets.US_ASCII),0,bytes,base,12);word(bytes,base+0xbfc,0x100000);int toc=base+0x800;bytes[toc+2]=(byte)0xa0;bytes[toc+7]=1;bytes[toc+12]=(byte)0xa1;bytes[toc+17]=1;bytes[toc+22]=(byte)0xa2;bytes[toc+28]=2;bytes[toc+29]=0x32;bytes[toc+30]=0x41;bytes[toc+32]=1;bytes[toc+38]=2;int offset=0;for(int i=0;i<2;i++){word(bytes,base+0x4000+i*32,offset);bytes[base+0x4000+i*32+4]=(byte)parts[i].length;bytes[base+0x4000+i*32+5]=(byte)(parts[i].length>>>8);System.arraycopy(parts[i],0,bytes,base+0x100000+offset,parts[i].length);offset+=parts[i].length;}return bytes;}
}
