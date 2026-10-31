package dev.psxlab.core;

import org.junit.Test;
import java.io.*;
import java.util.*;
import static org.junit.Assert.*;
import static dev.psxlab.core.CdRomTest.*;

public class CueDiscTest {
    @Test public void libraryReferencesUseTheSameParserAndDoNotOpenTrackFiles()throws Exception{
        String cue="FILE \"Tracks/Disc One.bin\" BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\nFILE AUDIO.BIN BINARY\nTRACK 02 AUDIO\nINDEX 01 00:00:00\n";
        List<String> names=CueDisc.referencedFiles(cue);assertEquals(Arrays.asList("Tracks/Disc One.bin","AUDIO.BIN"),names);
        assertThrows(UnsupportedOperationException.class,()->names.add("other.bin"));
        assertThrows(IOException.class,()->CueDisc.referencedFiles("FILE missing.wav WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00"));
    }
    private static ByteSource bytes(byte[] data){return new ByteSource(){
        public long length(){return data.length;}
        public byte[] read(long offset,int length)throws IOException{if(offset<0||offset+length>data.length)throw new EOFException();return Arrays.copyOfRange(data,(int)offset,(int)offset+length);}
        public void close(){}
    };}
    private static CueDisc open(String cue,Map<String,byte[]> files)throws IOException{return new CueDisc(cue,name->{byte[] data=files.get(name);if(data==null)throw new FileNotFoundException("Missing track: "+name);return bytes(data);});}
    private static byte[] rawIso(int stride,int offset,int leading){
        byte[] cooked=BootTest.iso(),raw=new byte[(32+leading)*stride];
        for(int i=0;i<32;i++){int at=(i+leading)*stride;raw[at+15]=(byte)(offset==24?2:1);System.arraycopy(cooked,i*2048,raw,at+offset,2048);}return raw;
    }
    private static byte[] pcm(int sectors){
        byte[] data=new byte[sectors*2352];for(int i=0;i<data.length;i+=4){data[i]=0x34;data[i+1]=0x12;data[i+2]=(byte)0xcc;data[i+3]=(byte)0xed;}return data;
    }
    private static final String THREE="FILE \"track-00.bin\" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\nFILE \"track-01.bin\" BINARY\n TRACK 02 AUDIO\n INDEX 00 00:00:00\n INDEX 01 00:02:00\nFILE \"track-02.bin\" BINARY\n TRACK 03 AUDIO\n PREGAP 00:00:02\n INDEX 01 00:00:00\n";
    private static CueDisc three()throws IOException{return open(THREE,Map.of("track-00.bin",rawIso(2352,24,0),"track-01.bin",pcm(154),"track-02.bin",pcm(3)));}
    @Test public void stagedThreeBinCueBootsAndKeepsAudioAndPregap()throws Exception{
        try(CueDisc cue=three()){
            IsoDisc disc=new IsoDisc((SectorSource)cue);assertArrayEquals(BootTest.executable(),disc.bootExecutable());assertEquals(3,disc.lastTrack());
            assertEquals(0,disc.trackStart(1));assertEquals(182,disc.trackStart(2));assertEquals(188,disc.trackStart(3));assertEquals(191,disc.sectorCount());assertEquals(191,disc.trackStart(0));
            assertEquals(2,disc.trackNumber(32));assertEquals(0,disc.trackIndex(32));assertEquals(150,disc.trackRelative(32));assertTrue(disc.audioTrack(32));assertEquals(1,disc.trackIndex(182));assertEquals(0,disc.trackRelative(182));
            assertArrayEquals(Arrays.copyOf(pcm(1),2352),disc.readSector(182));assertArrayEquals(new byte[2352],disc.readSector(186));assertArrayEquals(pcm(1),disc.readSector(188));
            assertThrows(EOFException.class,()->disc.readSector(191));assertThrows(EOFException.class,()->disc.readSector(-1));
        }
    }
    @Test public void sharedBinUsesFileIndexesRatherThanTreatingThemAsLengths()throws Exception{
        byte[] raw=Arrays.copyOf(rawIso(2352,24,0),40*2352);System.arraycopy(pcm(8),0,raw,32*2352,8*2352);
        String cue="FILE \"game.bin\" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n TRACK 02 AUDIO\n INDEX 00 00:00:32\n INDEX 01 00:00:34\n INDEX 02 00:00:36\n";
        try(CueDisc disc=open(cue,Map.of("game.bin",raw))){assertEquals(34,disc.trackStart(2));assertEquals(40,disc.sectorCount());assertEquals(0,disc.trackIndex(32));assertEquals(1,disc.trackIndex(34));assertEquals(2,disc.trackIndex(36));assertArrayEquals(pcm(1),disc.readSector(34));assertArrayEquals(BootTest.executable(),new IsoDisc((SectorSource)disc).bootExecutable());}
    }
    @Test public void firstTrackStoredPregapIsSkippedWhenReadingLbaZero()throws Exception{
        String cue="FILE \"data.bin\" BINARY\n TRACK 01 MODE2/2352\n INDEX 00 00:00:00\n INDEX 01 00:02:00\n";
        try(CueDisc disc=open(cue,Map.of("data.bin",rawIso(2352,24,150)))){assertEquals(0,disc.trackStart(1));assertEquals(32,disc.sectorCount());assertArrayEquals(BootTest.executable(),new IsoDisc((SectorSource)disc).bootExecutable());}
    }
    @Test public void cookedMode1AndMode2And2336BootThroughRawSectorView()throws Exception{
        for(String mode:new String[]{"MODE1/2048","MODE2/2048","MODE2/2336"}){
            byte[] file=mode.endsWith("2336")?new byte[32*2336]:BootTest.iso();if(mode.endsWith("2336"))for(int i=0;i<32;i++)System.arraycopy(BootTest.iso(),i*2048,file,i*2336+8,2048);
            try(CueDisc cue=open("FILE \"data.bin\" BINARY\n TRACK 01 "+mode+"\n INDEX 01 00:00:00",Map.of("data.bin",file))){IsoDisc disc=new IsoDisc((SectorSource)cue);assertArrayEquals(BootTest.executable(),disc.bootExecutable());assertEquals(mode.startsWith("MODE1")?1:2,disc.readSector(0)[15]);}
        }
    }
    @Test public void mixedSectorSizesInOneFileUseByteOffsetsForEachTrack()throws Exception{
        byte[] iso=BootTest.iso(),file=Arrays.copyOf(iso,iso.length+2352*3);System.arraycopy(pcm(3),0,file,iso.length,2352*3);
        String cue="FILE \"mixed.bin\" BINARY\n TRACK 01 MODE1/2048\n INDEX 01 00:00:00\n TRACK 02 AUDIO\n INDEX 01 00:00:32";
        try(CueDisc disc=open(cue,Map.of("mixed.bin",file))){assertEquals(35,disc.sectorCount());assertArrayEquals(pcm(1),disc.readSector(32));assertArrayEquals(BootTest.executable(),new IsoDisc((SectorSource)disc).bootExecutable());}
    }
    @Test public void bomCrLfCaseAndNestedTrackNamesAreAccepted()throws Exception{
        String cue="\uFEFFrem original fixture\r\nfile \"tracks\\data file.bin\" binary\r\ntrack 01 mode1/2048\r\nindex 01 00:00:00\r\n";
        try(CueDisc disc=open(cue,Map.of("tracks/data file.bin",BootTest.iso()))){assertArrayEquals(BootTest.executable(),new IsoDisc((SectorSource)disc).bootExecutable());}
    }
    @Test public void malformedCuesFailWithSpecificErrorsAndCloseAlreadyOpenedFiles()throws Exception{
        String[] invalid={"", "FILE \"a\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00", "FILE a BINARY\nTRACK 02 AUDIO\nINDEX 01 00:00:00", "FILE a BINARY\nTRACK 01 AUDIO", "FILE a BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:75", "FILE ../a BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00", "FILE a BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\nINDEX 00 00:00:00"};
        for(String cue:invalid)assertThrows(IOException.class,()->open(cue,Map.of("a",pcm(1))));
        assertTrue(assertThrows(IOException.class,()->open("FILE missing.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00",Map.of())).getMessage().contains("Missing track"));
        int[] closed={0};String cue="FILE good.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\nFILE absent.bin BINARY\nTRACK 02 AUDIO\nINDEX 01 00:00:00";
        assertThrows(IOException.class,()->new CueDisc(cue,name->{if(name.startsWith("absent"))throw new FileNotFoundException(name);return new ByteSource(){public byte[] read(long o,int n){return new byte[n];}public long length(){return 2352;}public void close(){closed[0]++;}};}));assertEquals(1,closed[0]);
        assertThrows(IOException.class,()->open("FILE a BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:02",Map.of("a",pcm(1))));
        assertThrows(IOException.class,()->open("FILE a BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00",Map.of("a",new byte[2353])));
    }
    @Test public void unusedFilePrefixBeforeIndex00DoesNotShiftLaterTracks()throws Exception{
        String cue="FILE data.bin BINARY\nTRACK 01 MODE2/2352\nINDEX 01 00:00:00\nFILE music.bin BINARY\nTRACK 02 AUDIO\nINDEX 00 00:00:05\nINDEX 01 00:00:07";
        try(CueDisc disc=open(cue,Map.of("data.bin",rawIso(2352,24,0),"music.bin",pcm(10)))){assertEquals(34,disc.trackStart(2));assertEquals(37,disc.sectorCount());assertEquals(0,disc.trackIndex(32));assertArrayEquals(pcm(1),disc.readSector(34));}
    }
    @Test public void readWithCddaModeRoutesAudioWithoutPublishingADataSector()throws Exception{
        try(CueDisc cue=three();Gpu gpu=new Gpu(false)){
            Bus b=new Bus(gpu,new PadState(),false);b.cdrom.insert(cue);command(b,0x0e,1);ack(b);command(b,2,0,4,0x32);ack(b);command(b,6);ack(b);
            for(int i=0;i<10000&&b.cdrom.cddaSectors==0;i++)b.cdrom.tick(1000);
            assertEquals(1,b.cdrom.cddaSectors);assertEquals(588,b.cdrom.audioBufferedFrames());assertEquals(0xedcc1234,b.cdrom.audioFrame());assertEquals(0,b.cdrom.availableBytes());
        }
    }
    @Test public void closeIsIdempotentAndPreventsFurtherReads()throws Exception{
        CueDisc cue=three();cue.close();cue.close();assertThrows(IOException.class,()->cue.readSector(0));
    }
    @Test public void controllerUsesCueTocAndPlaysSignedStereoWithoutDma()throws Exception{
        try(CueDisc cue=three();Gpu gpu=new Gpu(false)){
            Bus b=new Bus(gpu,new PadState(),false);b.cdrom.insert(new IsoDisc((SectorSource)cue));
            command(b,0x13);assertArrayEquals(new int[]{2,1,3},response(b));ack(b);
            command(b,0x14,2);assertArrayEquals(new int[]{2,0,4},response(b));ack(b);
            command(b,0x14,0);assertArrayEquals(new int[]{2,0,4},response(b));ack(b);
            command(b,3,2);assertArrayEquals(new int[]{0x82},response(b));ack(b);b.cdrom.tick(SECTOR);assertEquals(588,b.cdrom.audioBufferedFrames());assertEquals(0xedcc1234,b.cdrom.audioFrame());assertEquals(0,b.cdrom.availableBytes());assertEquals(0,b.cdrom.dmaBytes);
            command(b,0x11);int[] at=response(b);assertEquals(2,at[0]);assertEquals(1,at[1]);assertEquals(0,at[4]);assertEquals(1,b.cdrom.cddaSectors);
        }
    }
    @Test public void cddaHonorsMutePauseAndAutoPauseAtTheNextTracksPregap()throws Exception{
        try(CueDisc cue=three();Gpu gpu=new Gpu(false)){
            Bus b=new Bus(gpu,new PadState(),false);b.cdrom.insert(cue);command(b,0x0e,2);ack(b);command(b,3,2);ack(b);b.cdrom.tick(SECTOR);assertEquals(0xedcc1234,b.cdrom.audioFrame());
            command(b,0x0b);ack(b);assertEquals(0,b.cdrom.audioFrame());
            b.cdrom.tick(SECTOR*4);b.cdrom.tick(1);assertEquals(4,irq(b));ack(b);assertEquals(4,b.cdrom.cddaSectors);
            command(b,9);ack(b);awaitIrq(b,2);ack(b);assertEquals(0,b.cdrom.audioBufferedFrames());
        }
    }
    @Test public void doubleSpeedCddaTakesAlternateFramesWithoutDoublingTheAudioQueue()throws Exception{
        try(CueDisc cue=three();Gpu gpu=new Gpu(false)){
            Bus b=new Bus(gpu,new PadState(),false);b.cdrom.insert(cue);command(b,0x0e,128);ack(b);command(b,3,2);ack(b);b.cdrom.tick(SECTOR/2);assertEquals(294,b.cdrom.audioBufferedFrames());assertEquals(0xedcc1234,b.cdrom.audioFrame());
        }
    }
}
