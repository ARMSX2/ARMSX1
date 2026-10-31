package dev.psxlab.core;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.regex.*;

/** Bounded ISO9660 boot extraction; first data track only. */
public final class IsoDisc implements SectorSource {
    private final ByteSource source;
    private final SectorSource mapped;
    private int stride,dataOffset;
    private Record root;
    private int sectors;
    public String bootPath;
    private record Record(int sector,int size,boolean directory){}
    public IsoDisc(ByteSource source)throws IOException{this(source,null);}
    public IsoDisc(SectorSource source)throws IOException{this(CueDisc.rawBytes(source),source);}
    private IsoDisc(ByteSource source,SectorSource mapped)throws IOException{
        this.source=source;this.mapped=mapped;
        int[][] layouts={{2048,0},{2352,24},{2352,16},{2448,24},{2448,16}};
        for(int[] layout:layouts){
            try{
                byte[] pvd=source.read(16L*layout[0]+layout[1],2048);
                if(pvd[0]==1 && new String(pvd,1,5,StandardCharsets.US_ASCII).equals("CD001") && pvd[6]==1){
                    stride=layout[0];dataOffset=layout[1];root=record(pvd,156);
                    long length=source.length();sectors=length>=0?(int)Math.min(Integer.MAX_VALUE,length/stride):ExeLoader.le(pvd,80);return;
                }
            }catch(EOFException ignored){}
        }
        throw new IOException("No ISO9660 filesystem at track 1. Use a first-data-track ISO/BIN or CD CHD; unusual track layouts are not supported yet.");
    }
    private Record record(byte[] b,int p)throws IOException{
        if(p+34>b.length || (b[p]&255)<34 || p+(b[p]&255)>b.length)throw new IOException("Invalid ISO directory record");
        int sector=ExeLoader.le(b,p+2),size=ExeLoader.le(b,p+10);
        if(sector<0 || size<0 || b[p+1]!=0 || b[p+26]!=0 || b[p+27]!=0 || (b[p+25]&128)!=0)throw new IOException("Unsupported ISO extent layout");
        return new Record(sector,size,(b[p+25]&2)!=0);
    }
    private byte[] contents(Record r,int maximum)throws IOException{
        if(r.size>maximum)throw new IOException("ISO entry exceeds size limit");
        byte[] out=new byte[r.size];
        for(int at=0;at<out.length;at+=2048){
            if(Thread.currentThread().isInterrupted())throw new IOException("Cancelled");
            int n=Math.min(2048,out.length-at);byte[] block=source.read(((long)r.sector+at/2048)*stride+dataOffset,n);System.arraycopy(block,0,out,at,n);
        }
        return out;
    }
    private static String normalize(String s){return s.replaceFirst(";[0-9]+$", "").toUpperCase(Locale.ROOT);}
    private Record find(String path)throws IOException{
        Record current=root;
        for(String part:path.replace('\\','/').split("/")){
            if(part.isEmpty())continue;
            if(part.equals(".")||part.equals(".."))throw new IOException("Invalid boot path");
            if(!current.directory)throw new IOException("Boot path crosses a file");
            byte[] directory=contents(current,2*1024*1024);Record found=null;
            for(int p=0;p<directory.length;){
                int n=directory[p]&255;if(n==0){p=((p/2048)+1)*2048;continue;}
                Record entry=record(directory,p);int namesize=directory[p+32]&255;
                if(33+namesize>n)throw new IOException("Invalid ISO filename");
                String name=new String(directory,p+33,namesize,StandardCharsets.US_ASCII);
                if(normalize(name).equals(normalize(part))){found=entry;break;}p+=n;
            }
            if(found==null)throw new FileNotFoundException("Disc entry not found: "+part);current=found;
        }
        return current;
    }
    public byte[] bootExecutable()throws IOException{
        bootPath="PSX.EXE";
        try{
            String cnf=new String(contents(find("SYSTEM.CNF"),65536),StandardCharsets.US_ASCII);
            Matcher match=Pattern.compile("(?im)^\\s*BOOT\\s*=\\s*cdrom:\\s*([^\\r\\n]+)").matcher(cnf);
            if(!match.find())throw new IOException("SYSTEM.CNF has no BOOT executable");
            bootPath=match.group(1).trim().split("\\s+",2)[0];
        }catch(FileNotFoundException ignored){}
        Record exe=find(bootPath);if(exe.directory)throw new IOException("Boot entry is a directory");
        return contents(exe,2*1024*1024+2048);
    }
    @Override public int sectorCount(){return mapped==null?sectors:mapped.sectorCount();}
    @Override public int lastTrack(){return mapped==null?1:mapped.lastTrack();}
    @Override public int trackStart(int number){return mapped==null?SectorSource.super.trackStart(number):mapped.trackStart(number);}
    @Override public int trackNumber(int lba){return mapped==null?1:mapped.trackNumber(lba);}
    @Override public int trackIndex(int lba){return mapped==null?1:mapped.trackIndex(lba);}
    @Override public int trackRelative(int lba){return mapped==null?Math.max(0,lba):mapped.trackRelative(lba);}
    @Override public boolean audioTrack(int lba){return mapped!=null&&mapped.audioTrack(lba);}
    @Override public char region(){
        String name=bootPath==null?"":bootPath.toUpperCase(Locale.ROOT);
        if(name.matches(".*S[CL]US_.*"))return 'A';
        if(name.matches(".*S[CL]ES_.*"))return 'E';
        if(name.matches(".*S[CL]PS_.*") || name.matches(".*S[CL]PM_.*"))return 'I';
        return ' ';
    }
    @Override public byte[] readSector(int lba)throws IOException{
        if(mapped!=null)return mapped.readSector(lba);
        if(lba<0 || sectors>0 && lba>=sectors)throw new EOFException("Sector outside disc: "+lba);
        if(stride!=2048)return source.read((long)lba*stride,2352);
        // Cooked ISO lacks sync/header/ECC. Rebuild the header, preserve all user data.
        byte[] raw=new byte[2352];java.util.Arrays.fill(raw,1,11,(byte)255);int frames=lba+150;
        raw[12]=(byte)CdRom.bcd(frames/4500);raw[13]=(byte)CdRom.bcd(frames/75%60);raw[14]=(byte)CdRom.bcd(frames%75);raw[15]=1;
        System.arraycopy(source.read((long)lba*2048,2048),0,raw,16,2048);return raw;
    }
}
