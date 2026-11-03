package dev.psxlab.core;

/** Disc layout of a CD CHD from its track metadata (CHT2/CHTR), so CD-audio tracks exist for the drive.
 * CHD stores each track's frames back to back, padded to a multiple of four frames; a pregap is either
 * stored in front of the track's data ("V" pregap type) or absent from the file and played as silence.
 * Disc LBA 0 is track 1's INDEX 01 (00:02:00). */
public final class ChdTrackMap {
    /** Per track: audio flag, stored frames, pregap frames, pregap stored (1/0), postgap frames. */
    public static final int FIELDS=5;
    static final int PADDING=4;
    private final int count,sectors;
    private final boolean[] audio;
    private final int[] start,index1,end,chdFirst,storedFirst,storedEnd;
    public ChdTrackMap(int[] tracks){
        if(tracks==null||tracks.length==0||tracks.length%FIELDS!=0)throw new IllegalArgumentException("CHD track table");
        count=tracks.length/FIELDS;audio=new boolean[count];start=new int[count];index1=new int[count];end=new int[count];chdFirst=new int[count];storedFirst=new int[count];storedEnd=new int[count];
        int lba=0,chd=0;
        for(int t=0;t<count;t++){
            int at=t*FIELDS,frames=tracks[at+1],pregap=tracks[at+2],postgap=tracks[at+4];boolean stored=tracks[at+3]!=0;
            if(frames<=0||pregap<0||postgap<0||(stored&&pregap>frames))throw new IllegalArgumentException("CHD track "+(t+1)+" layout");
            audio[t]=tracks[at]!=0;start[t]=lba;chdFirst[t]=chd;
            if(stored){storedFirst[t]=lba;index1[t]=lba+pregap;}else{index1[t]=lba+pregap;storedFirst[t]=index1[t];}
            storedEnd[t]=storedFirst[t]+frames;lba=storedEnd[t]+postgap;end[t]=lba;
            chd+=frames+(PADDING-frames%PADDING)%PADDING;
        }
        int origin=index1[0];
        for(int t=0;t<count;t++){start[t]-=origin;index1[t]-=origin;end[t]-=origin;storedFirst[t]-=origin;storedEnd[t]-=origin;}
        sectors=end[count-1];
    }
    public int lastTrack(){return count;}
    public int sectorCount(){return sectors;}
    /** INDEX 01 of a track; track 0 is the lead-out. */
    public int trackStart(int track){if(track==0)return sectors;if(track<1||track>count)throw new IllegalArgumentException("No such CHD track");return index1[track-1];}
    private int track(int lba){for(int t=count-1;t>0;t--)if(lba>=start[t])return t;return 0;}
    public int trackNumber(int lba){return track(lba)+1;}
    public int trackIndex(int lba){return lba<index1[track(lba)]?0:1;}
    /** Distance from INDEX 01; pregap positions count down towards it, as the drive reports them. */
    public int trackRelative(int lba){int t=track(lba);return Math.abs(lba-index1[t]);}
    public boolean audioTrack(int lba){return audio[track(lba)];}
    /** CHD frame holding this LBA, or -1 for pregap/postgap frames not stored in the file. */
    public int chdFrame(int lba){int t=track(lba);if(lba<storedFirst[t]||lba>=storedEnd[t])return -1;return chdFirst[t]+(lba-storedFirst[t]);}
    public String describe(){StringBuilder out=new StringBuilder();for(int t=0;t<count;t++){if(t>0)out.append(", ");out.append(t+1).append(audio[t]?" audio @":" data @").append(index1[t]);}return out.toString();}
}
