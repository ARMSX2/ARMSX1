package dev.psxlab.core;

import java.io.*;
import java.util.*;
import java.util.regex.*;

/** Original CUE/BIN reader. File indexes are file-relative; exposed LBAs are disc-relative. */
public final class CueDisc implements SectorSource, AutoCloseable {
    @FunctionalInterface public interface Opener { ByteSource open(String name) throws IOException; }
    private static final Pattern FILE=Pattern.compile("(?i)^FILE\\s+(?:\"([^\"]+)\"|(\\S+))\\s+(\\S+)\\s*$");
    private static final Set<String> METADATA=Set.of("REM","TITLE","PERFORMER","SONGWRITER","CATALOG","FLAGS","ISRC","CDTEXTFILE");
    private enum Mode {
        AUDIO(2352,0),MODE1_RAW(2352,1),MODE1_COOKED(2048,1),MODE2_RAW(2352,2),MODE2_2336(2336,2),MODE2_COOKED(2048,2);
        final int stride,headerMode;
        Mode(int stride,int headerMode){this.stride=stride;this.headerMode=headerMode;}
        static Mode parse(String value)throws IOException{return switch(value.toUpperCase(Locale.ROOT)){
            case "AUDIO"->AUDIO;case "MODE1/2352"->MODE1_RAW;case "MODE1/2048"->MODE1_COOKED;
            case "MODE2/2352"->MODE2_RAW;case "MODE2/2336"->MODE2_2336;case "MODE2/2048"->MODE2_COOKED;
            default->throw new IOException("Unsupported CUE track mode: "+value);
        };}
    }
    private static final class TrackFile {
        final String name;final List<Track> tracks=new ArrayList<>();ByteSource source;
        TrackFile(String name){this.name=name;}
    }
    private static final class Track {
        final int number;final Mode mode;final TrackFile file;
        final TreeMap<Integer,Integer> indexes=new TreeMap<>();
        int pregap,postgap,fileBegin,storedSectors,index0,index1,storedLba,end;
        boolean hasPregap,hasPostgap;
        long byteBegin;
        Track(int number,Mode mode,TrackFile file){this.number=number;this.mode=mode;this.file=file;}
        int first(){return indexes.containsKey(0)?indexes.get(0):indexes.get(1);}
    }
    private final List<TrackFile> files=new ArrayList<>();
    private final List<Track> tracks=new ArrayList<>();
    private int sectors;
    private boolean closed;

    /** Takes ownership of sources returned by opener, including on a later failure. */
    public CueDisc(String text,Opener opener)throws IOException{
        try{parse(text);layout(opener);}catch(IOException|RuntimeException e){try{close();}catch(IOException closing){e.addSuppressed(closing);}throw e;}
    }
    public static boolean isCueName(String name){return name!=null&&name.toLowerCase(Locale.ROOT).endsWith(".cue");}
    /** Reject absolute/parent references; ordinary nested track folders remain supported. */
    public static String relativeName(String name)throws IOException{
        String normalized=name.replace('\\','/');
        if(normalized.isEmpty()||normalized.startsWith("/")||normalized.contains(":"))throw new IOException("CUE track must use a relative file name");
        for(String part:normalized.split("/",-1))if(part.equals("..")||part.isEmpty())throw new IOException("Invalid CUE track path: "+name);
        return normalized;
    }
    private static int number(String text,int minimum,int maximum,String description)throws IOException{
        try{int n=Integer.parseInt(text);if(n<minimum||n>maximum)throw new NumberFormatException();return n;}
        catch(NumberFormatException e){throw new IOException("Invalid CUE "+description+": "+text);}
    }
    private static int time(String value)throws IOException{
        String[] msf=value.split(":",-1);if(msf.length!=3)throw new IOException("Invalid CUE time: "+value);
        return number(msf[0],0,99,"minutes")*4500+number(msf[1],0,59,"seconds")*75+number(msf[2],0,74,"frames");
    }
    private void parse(String text)throws IOException{
        if(text.length()>1024*1024)throw new IOException("CUE text exceeds 1 MiB");
        TrackFile file=null;Track track=null;int lineNumber=0;
        for(String row:text.replaceFirst("^\\uFEFF","").split("\\r?\\n")){
            lineNumber++;String line=row.trim();if(line.isEmpty())continue;
            String[] words=line.split("\\s+");String command=words[0].toUpperCase(Locale.ROOT);
            if(METADATA.contains(command))continue;
            try{
                switch(command){
                    case "FILE"->{
                        Matcher match=FILE.matcher(line);if(!match.matches())throw new IOException("Invalid FILE entry");
                        if(!match.group(3).equalsIgnoreCase("BINARY"))throw new IOException("Unsupported CUE file type: "+match.group(3)+"; use BINARY BIN tracks or CHD");
                        if(files.size()>=99)throw new IOException("Too many CUE files");
                        String name=match.group(1)!=null?match.group(1):match.group(2);file=new TrackFile(relativeName(name));files.add(file);track=null;
                    }
                    case "TRACK"->{
                        if(file==null||words.length!=3)throw new IOException("TRACK requires a FILE and mode");
                        int n=number(words[1],1,99,"track number");if(n!=tracks.size()+1)throw new IOException("CUE tracks must be numbered consecutively from 01");
                        track=new Track(n,Mode.parse(words[2]),file);tracks.add(track);file.tracks.add(track);
                    }
                    case "INDEX"->{
                        if(track==null||words.length!=3)throw new IOException("INDEX requires a TRACK");
                        int n=number(words[1],0,99,"index"),at=time(words[2]);
                        if(track.indexes.containsKey(n)||(!track.indexes.isEmpty()&&(n<=track.indexes.lastKey()||at<track.indexes.lastEntry().getValue())))throw new IOException("CUE indexes must be unique and ordered");
                        track.indexes.put(n,at);
                    }
                    case "PREGAP","POSTGAP"->{
                        if(track==null||words.length!=2)throw new IOException(command+" requires a TRACK and time");
                        if(command.equals("PREGAP")){if(track.hasPregap)throw new IOException("Duplicate PREGAP");track.hasPregap=true;track.pregap=time(words[1]);}
                        else{if(track.hasPostgap)throw new IOException("Duplicate POSTGAP");track.hasPostgap=true;track.postgap=time(words[1]);}
                    }
                    default->throw new IOException("Unsupported CUE directive: "+command);
                }
            }catch(IOException e){throw new IOException("CUE line "+lineNumber+": "+e.getMessage(),e);}
        }
        if(tracks.isEmpty())throw new IOException("CUE contains no tracks");
        for(TrackFile f:files)if(f.tracks.isEmpty())throw new IOException("CUE FILE has no tracks: "+f.name);
        for(Track t:tracks)if(!t.indexes.containsKey(1))throw new IOException("CUE track "+t.number+" is missing INDEX 01");
    }
    private void layout(Opener opener)throws IOException{
        for(TrackFile f:files){
            f.source=Objects.requireNonNull(opener.open(f.name),"CUE opener returned null");long length=f.source.length();
            if(length<0)throw new IOException("CUE track file is not seekable: "+f.name);
            long byteAt=0;int previousFrame=0;Mode previousMode=f.tracks.get(0).mode;
            for(int i=0;i<f.tracks.size();i++){
                Track t=f.tracks.get(i);t.fileBegin=t.first();
                if(t.fileBegin<previousFrame)throw new IOException("Overlapping CUE track indexes in "+f.name);
                byteAt+=(long)(t.fileBegin-previousFrame)*previousMode.stride;t.byteBegin=byteAt;
                int stored;
                if(i+1<f.tracks.size()){
                    int end=f.tracks.get(i+1).first();if(end<=t.indexes.get(1))throw new IOException("Overlapping or empty CUE track "+t.number);
                    stored=end-t.fileBegin;
                }else{
                    long remaining=length-byteAt;
                    if(remaining<=0||remaining%t.mode.stride!=0||remaining/t.mode.stride>Integer.MAX_VALUE)throw new IOException("Truncated or misaligned CUE track: "+f.name);
                    stored=(int)(remaining/t.mode.stride);
                }
                if(byteAt<0||byteAt+(long)stored*t.mode.stride>length)throw new IOException("CUE index exceeds track file: "+f.name);
                if(t.indexes.lastEntry().getValue()>=t.fileBegin+(long)stored)throw new IOException("CUE INDEX points past track "+t.number);
                t.storedSectors=stored;previousFrame=t.fileBegin;previousMode=t.mode;
            }
        }
        Track first=tracks.get(0);long cursor=-(long)first.pregap-(first.indexes.get(1)-first.fileBegin);
        for(Track t:tracks){
            int storedGap=t.indexes.get(1)-t.fileBegin;
            if(storedGap<0)throw new IOException("Invalid CUE INDEX 00/01 ordering");
            t.index0=checked(cursor);cursor+=t.pregap;t.storedLba=checked(cursor);cursor+=storedGap;t.index1=checked(cursor);
            cursor+=(long)t.storedSectors-storedGap+t.postgap;t.end=checked(cursor);
        }
        if(cursor<=0)throw new IOException("Empty CUE disc");sectors=checked(cursor);
    }
    private static int checked(long value)throws IOException{if(value<Integer.MIN_VALUE||value>Integer.MAX_VALUE)throw new IOException("CUE disc is too large");return (int)value;}
    private Track track(int lba){for(int i=tracks.size()-1;i>=0;i--)if(lba>=tracks.get(i).index0)return tracks.get(i);return tracks.get(0);}
    @Override public int sectorCount(){return sectors;}
    @Override public int lastTrack(){return tracks.size();}
    @Override public int trackStart(int number){if(number==0)return sectors;if(number<1||number>tracks.size())throw new IllegalArgumentException("No such CUE track");return tracks.get(number-1).index1;}
    @Override public int trackNumber(int lba){return track(lba).number;}
    @Override public boolean audioTrack(int lba){return track(lba).mode==Mode.AUDIO;}
    @Override public int trackIndex(int lba){
        Track t=track(lba);if(lba<t.index1)return 0;int result=1;
        int fileFrame=t.fileBegin+(lba-t.storedLba);
        for(var index:t.indexes.entrySet())if(index.getKey()>1&&fileFrame>=index.getValue())result=index.getKey();
        return result;
    }
    @Override public int trackRelative(int lba){Track t=track(lba);return lba<t.index1?t.index1-lba:lba-t.index1;}
    @Override public byte[] readSector(int lba)throws IOException{
        if(closed)throw new IOException("CUE disc is closed");if(lba<0||lba>=sectors)throw new EOFException("Sector outside CUE disc: "+lba);
        Track t=track(lba);int offset=lba-t.storedLba;
        byte[] bytes=offset>=0&&offset<t.storedSectors?t.file.source.read(t.byteBegin+(long)offset*t.mode.stride,t.mode.stride):new byte[t.mode.stride];
        if(bytes.length!=t.mode.stride)throw new EOFException("Truncated CUE sector in "+t.file.name);
        if(t.mode.stride==2352)return bytes;
        byte[] raw=new byte[2352];Arrays.fill(raw,1,11,(byte)255);int absolute=lba+150;
        raw[12]=(byte)CdRom.bcd(absolute/4500);raw[13]=(byte)CdRom.bcd(absolute/75%60);raw[14]=(byte)CdRom.bcd(absolute%75);raw[15]=(byte)t.mode.headerMode;
        int at=t.mode==Mode.MODE2_COOKED?24:16;if(at==24)raw[18]=raw[22]=8;
        System.arraycopy(bytes,0,raw,at,bytes.length);return raw;
    }
    @Override public void close()throws IOException{
        if(closed)return;closed=true;IOException failure=null;
        for(TrackFile f:files)if(f.source!=null)try{f.source.close();}catch(IOException e){if(failure==null)failure=e;else failure.addSuppressed(e);}
        if(failure!=null)throw failure;
    }
    /** Presents raw sectors as bytes solely for bounded ISO boot-directory extraction. */
    static ByteSource rawBytes(SectorSource source){return new ByteSource(){
        public long length(){return (long)source.sectorCount()*2352;}
        public byte[] read(long offset,int size)throws IOException{
            if(offset<0||size<0||size>4*1024*1024||offset>length()-size)throw new EOFException("Sector byte range outside disc");
            byte[] result=new byte[size];int done=0;while(done<size){long position=offset+done;int at=(int)(position%2352),n=Math.min(size-done,2352-at);byte[] sector=source.readSector((int)(position/2352));if(sector.length!=2352)throw new IOException("Invalid raw sector length");System.arraycopy(sector,at,result,done,n);done+=n;}return result;
        }
        public void close(){} // Ownership stays with the mounted media.
    };}
}
