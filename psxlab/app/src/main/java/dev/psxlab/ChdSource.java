package dev.psxlab;
import dev.psxlab.core.ByteSource;
import dev.psxlab.core.ChdTrackMap;
import dev.psxlab.core.EngineLog;
import dev.psxlab.core.SectorSource;
import android.os.ParcelFileDescriptor;
import java.io.IOException;
/** A CD CHD. With track metadata it is also a sector-level disc, so CD-audio tracks play; a CHD
 * without it keeps the flat single-data-track layout. */
public final class ChdSource implements ByteSource,SectorSource {
    private static final int UNIT=2448,SECTOR=2352;
    private final EngineLog journal;
    static { System.loadLibrary("psxdisc"); }
    private long handle;
    private final ChdTrackMap tracks;
    public ChdSource(ParcelFileDescriptor descriptor) throws IOException {this(descriptor,null);}
    public ChdSource(ParcelFileDescriptor descriptor,EngineLog journal) throws IOException {
        this.journal=journal;handle=open(descriptor.getFd());
        ChdTrackMap map=null;
        try{int[] table=tracksNative(handle);if(table!=null&&table.length>0)map=new ChdTrackMap(table);}
        catch(RuntimeException e){if(journal!=null)journal.event(EngineLog.WARN,"cd.chd.tracks_ignored",java.util.Map.of("reason",String.valueOf(e.getMessage())));}
        tracks=map;
    }
    /** True when the CHD's own track table is used; false for the flat single-track layout. */
    public boolean hasTracks(){return tracks!=null;}
    private static native long open(int fd) throws IOException;
    private static native void readNative(long handle,long offset,byte[] out) throws IOException;
    private static native void closeNative(long handle);
    private static native long lengthNative(long handle);
    private static native int[] tracksNative(long handle);
    @Override public synchronized long length()throws IOException{if(handle==0)throw new IOException("Disc is closed");return lengthNative(handle);}
    @Override public synchronized byte[] read(long offset,int length) throws IOException {
        if(handle==0)throw new IOException("Disc is closed");
        if(length<0 || length>4*1024*1024)throw new IOException("Disc read too large");
        try(EngineLog.Span span=journal==null?null:journal.watch("cd.chd.read",EngineLog.DEBUG,5_000_000L)){byte[] b=new byte[length];readNative(handle,offset,b);return b;}
    }
    /** Raw 2352-byte sector. Gaps not stored in the file read as silence; CHD keeps CD audio big-endian. */
    @Override public byte[] readSector(int lba)throws IOException{
        if(tracks==null)return read((long)lba*UNIT,SECTOR);
        int frame=tracks.chdFrame(lba);if(frame<0)return new byte[SECTOR];
        byte[] sector=read((long)frame*UNIT,SECTOR);
        if(tracks.audioTrack(lba))for(int i=0;i+1<SECTOR;i+=2){byte b=sector[i];sector[i]=sector[i+1];sector[i+1]=b;}
        return sector;
    }
    @Override public int sectorCount(){if(tracks!=null)return tracks.sectorCount();try{return (int)(length()/UNIT);}catch(IOException e){return 0;}}
    @Override public int lastTrack(){return tracks==null?1:tracks.lastTrack();}
    @Override public int trackStart(int track){return tracks==null?SectorSource.super.trackStart(track):tracks.trackStart(track);}
    @Override public int trackNumber(int lba){return tracks==null?1:tracks.trackNumber(lba);}
    @Override public int trackIndex(int lba){return tracks==null?1:tracks.trackIndex(lba);}
    @Override public int trackRelative(int lba){return tracks==null?Math.max(0,lba):tracks.trackRelative(lba);}
    @Override public boolean audioTrack(int lba){return tracks!=null&&tracks.audioTrack(lba);}
    @Override public synchronized void close(){if(handle!=0){closeNative(handle);handle=0;}}
}
