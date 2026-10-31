package dev.psxlab.core;

import java.io.IOException;
import java.util.*;

/** Scheduled CD-ROM controller with data, XA and CUE track/CD-DA access.
 * Drive/HC05 timing remains an approximation. */
public final class CdRom {
    public static final int COMMAND_TICKS=20000,COMPLETE_TICKS=10000;
    private static final int SINGLE_SECTOR_TICKS=33868800/75;
    private final Bus bus;
    private SectorSource disc;
    private int bank,enable,interrupt,parameterCount,mode,file,channel;
    private final int[] parameters=new int[16];
    private final ArrayDeque<Integer> response=new ArrayDeque<>();
    private final int[] responseHistory=new int[16];
    private int responseReadPosition;
    private final ArrayDeque<Event> events=new ArrayDeque<>();
    private int pendingCommand=-1,commandTicks,location,readLocation,sectorTicks;
    private int[] commandParameters;
    private boolean motor,reading,playing,request,muted;
    private int driveTicks,seekTicks,seekTarget;
    private boolean readAfterSeek,explicitSeek,seekReady;
    private int playStep=1;
    private int scexTotal,scexMatches;
    private boolean locationPending;
    private byte[] lastSector,lastDataSector,data=new byte[0];
    private int dataPosition;
    private boolean sectorPending;
    public long commands,commandOverwrites,sectorsRead,dmaBytes,xaSectors,xaFrames,xaDroppedFrames,cddaSectors,cddaFrames;
    private final XaDecoder xa=new XaDecoder();
    private final short[] audio=new short[65536];
    private int audioHead,audioSize,xaFile=-1,xaChannel=-1;
    private boolean xaMuted;
    private final int[] pendingVolume={128,0,128,0},volume={128,0,128,0};
    private final XaDecoder.Sink xaSink=this::queueAudio;
    private void resetAudio(){if(traceSink!=null)traceSink.accept("CD_AUDIO_RESET queued="+audioBufferedFrames()+" decoded="+xaFrames);audioHead=audioSize=0;xaFile=xaChannel=-1;xa.reset();}
    private boolean enqueueAudio(int left,int right){
        if(audioSize==audio.length){xaDroppedFrames++;return false;}
        int tail=(audioHead+audioSize)%audio.length;audio[tail]=(short)left;audio[(tail+1)%audio.length]=(short)right;audioSize+=2;return true;
    }
    private void queueAudio(int left,int right){if(enqueueAudio(left,right))xaFrames++;}
    private void queueCdda(byte[] pcm)throws IOException{
        if(pcm.length!=2352)throw new IOException("Invalid CD-DA sector length");
        // Binary CD audio is signed little-endian stereo. At 2x the controller takes alternate frames.
        int step=(mode&128)!=0?8:4;
        for(int at=0;at<pcm.length;at+=step){int left=(short)((pcm[at]&255)|(pcm[at+1]<<8)),right=(short)((pcm[at+2]&255)|(pcm[at+3]<<8));if(enqueueAudio(left,right))cddaFrames++;}
        cddaSectors++;
    }
    /** One frame at the SPU's 44.1 kHz clock, with the CD controller's applied matrix. */
    int audioFrame(){
        if(audioSize==0)return 0;
        int left=audio[audioHead],right=audio[(audioHead+1)%audio.length];audioHead=(audioHead+2)%audio.length;audioSize-=2;
        if(audioSize==0 && traceSink!=null)traceSink.accept("CD_AUDIO_DRAINED lba="+(readLocation-1)+" reading="+reading+" decoded="+xaFrames);
        if(muted || xaMuted)return 0;
        int l=Spu.clamp((left*volume[0]+right*volume[3])>>7),r=Spu.clamp((left*volume[1]+right*volume[2])>>7);
        return (l&65535)|(r<<16);
    }
    int audioBufferedFrames(){return audioSize/2;}
    public String lastMediaError="";
    private final ArrayDeque<Integer> recentCommands=new ArrayDeque<>();
    private final ArrayDeque<Integer> recentWrites=new ArrayDeque<>();
    private long irqReads,statusReads;
    private int lastReadBank;
    private java.util.function.Consumer<String> traceSink;
    /** Optional bounded capture is owned by the diagnostic caller, never enabled by normal play. */
    public void setTraceSink(java.util.function.Consumer<String> sink){traceSink=sink;}
    private static final class Event {
        int ticks;final int irq;final int[] bytes;
        Event(int ticks,int irq,int[] bytes){this.ticks=ticks;this.irq=irq;this.bytes=bytes;}
    }
    CdRom(Bus bus){this.bus=bus;}
    public void insert(SectorSource disc){this.disc=Objects.requireNonNull(disc);stopRead();driveTicks=0;motor=true;lastSector=lastDataSector=null;resetAudio();location=readLocation=0;data=new byte[0];dataPosition=0;initializeScex();}
    /** Host tray-open event. The host retains the old source until replacement is installed. */
    public void eject(){
        stopRead();disc=null;motor=false;driveTicks=seekTicks=0;readAfterSeek=explicitSeek=seekReady=false;
        pendingCommand=-1;commandTicks=0;parameterCount=0;events.clear();response.clear();interrupt=0;
        lastSector=lastDataSector=null;data=new byte[0];dataPosition=0;resetAudio();initializeScex();
        event(0,5,0x10,8);
    }
    private void initializeScex(){scexTotal=scexMatches=disc!=null && disc.region()!=' '?1:0;}
    /** The fast-load path replaces the BIOS GUI, including its CD host IRQ setup. */
    public void prepareBootHandoff(){if(disc!=null){write(0,1);write(2,31);write(3,0x5f);write(0,0);write(3,0);}}
    public static int bcd(int n){return (n/10<<4)|(n%10);}
    private int binary(int n){if((n&15)>9 || (n>>>4)>9)return -1;return (n>>>4)*10+(n&15);}
    private int status(){return (disc==null?0x10:0)|(motor?2:0)|(seekTicks>0?0x40:0)|(reading?0x20:0)|(playing?0x80:0);}
    private void irqLine(){if((interrupt&enable&31)!=0)bus.raiseInterrupt(2);}
    private void event(int ticks,int irq,int... bytes){if(events.size()>=16)throw new IllegalStateException("CD-ROM response queue overflow");events.add(new Event(ticks,irq,bytes));}
    private void error(int code){event(0,5,status()|1,code);}
    private boolean media(){if(disc==null){error(0x80);return false;}return true;}
    int statusRegister(){return bank|(parameterCount==0?8:0)|(parameterCount<16?16:0)|(!response.isEmpty()?32:0)|(dataPosition<data.length?64:0)|(pendingCommand>=0?128:0);}
    int interruptRegister(){return 0xe0|((bank&1)==0?enable:interrupt);}
    public int read(int port){
        if((port&3)==3){irqReads++;lastReadBank=bank;}if((port&3)==0)statusReads++;
        return switch(port&3){
            case 0 -> statusRegister();
            case 1 -> {int value=responseHistory[responseReadPosition++&15];if(!response.isEmpty())response.removeFirst();yield value;}
            case 2 -> readDataByte();
            default -> interruptRegister();
        };
    }
    public void write(int port,int value){
        value&=255;port&=3;
        if(port==0 && bank==(value&3))return;
        recentWrites.add((bank<<16)|(port<<8)|value);if(recentWrites.size()>24)recentWrites.removeFirst();
        if(port==0){bank=value&3;return;}
        if(bank==0){
            if(port==1){
                // COMMAND is a single latch, not a queue. An early write replaces the
                // unhandled command; pending IRQ/results still require acknowledgement.
                // Keep the replacement's parameter snapshot and restart its receive delay.
                if(pendingCommand>=0)commandOverwrites++;
                pendingCommand=value;commandTicks=COMMAND_TICKS;commandParameters=Arrays.copyOf(parameters,parameterCount);parameterCount=0;
                commands++;recentCommands.add(value);if(recentCommands.size()>12)recentCommands.removeFirst();
            }else if(port==2){if(parameterCount<16)parameters[parameterCount++]=value;}
            else{
                if((value&0x20)!=0)throw new UnsupportedOperationException("CD-ROM sound-map writes are not implemented");
                boolean previous=request;request=(value&0x80)!=0;
                if(request && !previous && lastDataSector!=null)loadData();else if(!request){data=new byte[0];dataPosition=0;}
            }
        }else if(bank==1){
            if(port==2){enable=value&31;irqLine();}
            else if(port==3){
                // IRQ acknowledgement and RESULT reads are independent. Some drivers
                // acknowledge first, then drain RESULT; keep its bytes and read cursor.
                interrupt&=~(value&31);if((value&0x40)!=0)parameterCount=0;irqLine();
            }
            else if(value!=0)throw new UnsupportedOperationException("CD-ROM sound-map upload is not implemented");
        }else if(bank==2 || bank==3){
            if(bank==2){if(port==1)audioRegisters[0]=value;else pendingVolume[port-2]=value;}
            else if(port==1)pendingVolume[2]=value;
            else if(port==2)pendingVolume[3]=value;
            else{xaMuted=(value&1)!=0;if((value&32)!=0)System.arraycopy(pendingVolume,0,volume,0,4);}
        }
    }
    private final int[] audioRegisters=new int[6];
    private void loadData(){
        int offset=(mode&32)!=0?12:(lastDataSector[15]&255)==1?16:24;
        int bytes=(mode&32)!=0?2340:2048;
        data=Arrays.copyOfRange(lastDataSector,offset,offset+bytes);dataPosition=0;bus.dma.resumeCdrom();
    }
    private int readDataByte(){if(dataPosition>=data.length)return data.length==0?0:data[data.length-(data.length==2048?8:4)]&255;return data[dataPosition++]&255;}
    private void replaceResponse(int[] bytes){response.clear();Arrays.fill(responseHistory,0);responseReadPosition=0;for(int i=0;i<bytes.length;i++){response.add(bytes[i]&255);responseHistory[i]=bytes[i]&255;}}
    public int availableBytes(){return data.length-dataPosition;}
    public int readDmaWord(){
        if(availableBytes()<4)throw new IllegalStateException("CD-ROM DMA requested unavailable data");
        dmaBytes+=4;return readDataByte()|readDataByte()<<8|readDataByte()<<16|readDataByte()<<24;
    }
    public void tick(int cycles){
        boolean wasSeeking=seekTicks>0,wasReading=reading;driveTicks=Math.max(0,driveTicks-cycles);
        irqLine();
        if(pendingCommand>=0){commandTicks=Math.max(0,commandTicks-cycles);if(commandTicks==0 && interrupt==0){int command=pendingCommand;pendingCommand=-1;process(command,commandParameters);}}
        if(!events.isEmpty()){
            Event head=events.peekFirst();head.ticks=Math.max(0,head.ticks-cycles);
            if(head.ticks==0 && interrupt==0){events.removeFirst();replaceResponse(head.bytes);interrupt=head.irq;if(traceSink!=null)traceSink.accept("INT"+interrupt+" "+Arrays.toString(head.bytes));irqLine();}
        }
        int readCycles=reading && !wasReading?0:cycles;
        if(seekTicks>0){
            if(!wasSeeking)readCycles=0;
            else{int before=seekTicks;seekTicks=Math.max(0,seekTicks-cycles);readCycles=Math.max(0,cycles-before);
                if(seekTicks==0){readLocation=seekTarget;reading=readAfterSeek;readAfterSeek=false;seekReady=!reading;sectorTicks=CdDriveTiming.sector((mode&128)!=0);if(traceSink!=null)traceSink.accept("SEEK_COMPLETE lba="+readLocation+" reading="+reading);if(explicitSeek){explicitSeek=false;event(0,2,status());}}
            }
        }
        if(reading && disc!=null && readCycles>0){
            sectorTicks-=readCycles;
            if(sectorTicks<=0){
                sectorTicks+=SINGLE_SECTOR_TICKS/((mode&128)!=0?2:1);
                try{
                    lastSector=disc.readSector(readLocation++);if(lastSector.length!=2352)throw new IOException("Invalid raw sector length");
                    sectorsRead++;
                    if(disc.audioTrack(readLocation-1)){if((mode&1)!=0)queueCdda(lastSector);}
                    else{
                        int submode=lastSector[18]&255;
                        boolean realtimeAudio=(lastSector[15]&255)==2 && (submode&0x44)==0x44;
                        boolean filtered=(mode&8)!=0,matches=(lastSector[16]&255)==file && (lastSector[17]&255)==channel;
                        if(realtimeAudio && (mode&64)!=0){
                            if((!filtered || matches) && (lastSector[17]&255)<32){
                                int f=lastSector[16]&255,c=lastSector[17]&255;
                                if(xaFile<0){xaFile=f;xaChannel=c;}
                                if(f==xaFile && c==xaChannel){int before=audioBufferedFrames(),decoded=xa.decode(lastSector,xaSink);xaSectors++;if(traceSink!=null)traceSink.accept("XA_ENQUEUE lba="+(readLocation-1)+" file="+f+" channel="+c+" coding="+(lastSector[19]&255)+" frames="+decoded+" queued="+before+"->"+audioBufferedFrames()+" dropped="+xaDroppedFrames);if((submode&128)!=0){xaFile=xaChannel=-1;}}
                            }
                        }else if(!(realtimeAudio && filtered)){
                            lastDataSector=lastSector;sectorPending=true;if(request)loadData();
                        }
                    }
                }catch(IOException e){reading=false;lastMediaError=e.getMessage();error(0x80);}
            }
            if(sectorPending && interrupt==0 && events.isEmpty()){
                sectorPending=false;replaceResponse(new int[]{status()});interrupt=1;irqLine();
            }
        }
        if(playing)tickDataTrackPlay(cycles);
    }
    int clocksToEvent(){
        int n=Integer.MAX_VALUE;
        if((interrupt&enable&31)!=0 && !bus.interruptRaised(2))return 1;
        if(pendingCommand>=0 && (commandTicks>0 || interrupt==0))n=Math.min(n,Math.max(1,commandTicks));
        if(!events.isEmpty() && (events.peekFirst().ticks>0 || interrupt==0))n=Math.min(n,Math.max(1,events.peekFirst().ticks));
        if(reading || playing)n=Math.min(n,Math.max(1,sectorTicks));
        if(seekTicks>0)n=Math.min(n,seekTicks);
        if(driveTicks>0)n=Math.min(n,driveTicks);
        if(sectorPending && interrupt==0 && events.isEmpty())return 1;
        return n;
    }
    /** Keep legacy command-to-child-event ordering at the final clock quantum. */
    void tickScheduled(int cycles){
        if(cycles<=32){tick(cycles);return;}
        int prefix=((cycles-1)/32)*32;
        if(prefix<clocksToEvent()){tick(prefix);tick(cycles-prefix);}
        else{while(cycles>32){tick(32);cycles-=32;}if(cycles>0)tick(cycles);}
    }
    /** PLAY advances across the mounted track map; only audio tracks feed PCM. */
    private void tickDataTrackPlay(int cycles){
        sectorTicks-=cycles;
        int period=SINGLE_SECTOR_TICKS/((mode&128)!=0?2:1);
        while(playing && sectorTicks<=0){
            sectorTicks+=period;
            int current=readLocation;
            if(current>=0&&current<disc.sectorCount()&&disc.audioTrack(current)){
                try{
                    queueCdda(disc.readSector(current));
                }catch(IOException e){playing=false;lastMediaError=e.getMessage();error(0x80);break;}
            }
            readLocation+=playStep;
            if(readLocation<0){readLocation=0;playStep=1;}
            if(readLocation>=disc.sectorCount()){
                readLocation=disc.sectorCount();playing=false;motor=false;event(0,4,status());break;
            }
            if((mode&2)!=0&&disc.trackNumber(readLocation)!=disc.trackNumber(current)){
                playing=false;event(0,4,status());break;
            }
            int absolute=readLocation+149,frame=bcd(absolute%75);
            if((mode&4)!=0 && (frame&15)==0 && interrupt==0 && events.isEmpty()){
                boolean relative=(frame&16)!=0;int position=relative?disc.trackRelative(current):absolute;
                replaceResponse(new int[]{status(),bcd(disc.trackNumber(current)),bcd(disc.trackIndex(current)),bcd(position/4500),bcd(position/75%60)|(relative?128:0),bcd(position%75),0,0});
                interrupt=1;irqLine();
            }
        }
    }
    private boolean count(int[] p,int n){if(p.length!=n){error(0x20);return false;}return true;}
    private void stopRead(){reading=playing=readAfterSeek=explicitSeek=seekReady=false;seekTicks=0;sectorPending=false;}
    private void seek(int target,boolean read){
        if(!motor)driveTicks=Math.max(driveTicks,CdDriveTiming.HZ);motor=true;
        seekTarget=target;seekTicks=driveTicks+CdDriveTiming.seek(readLocation,target,(mode&128)!=0);readAfterSeek=read;explicitSeek=!read;
        if(traceSink!=null)traceSink.accept("SEEK_BEGIN from="+readLocation+" to="+target+" ticks="+seekTicks+" after="+(read?"read":"interrupt"));
    }
    private void process(int command,int[] p){
        if(traceSink!=null)traceSink.accept(String.format(Locale.ROOT,"CMD %02X %s at LBA %d",command,Arrays.toString(p),readLocation));
        switch(command){
            case 1 -> {if(count(p,0))event(0,3,status());}
            case 2 -> {
                if(!count(p,3))break;
                int m=binary(p[0]),s=binary(p[1]),f=binary(p[2]);
                if(m<0 || s<0 || s>=60 || f<0 || f>=75 || (m*60+s)*75+f<150){error(0x10);break;}
                location=(m*60+s)*75+f-150;locationPending=true;if(location!=readLocation)seekReady=false;event(0,3,status());
            }
            case 3 -> {
                if(p.length>1){error(0x20);break;}
                if(!media())break;
                int track=p.length==0?0:binary(p[0]);if(track<0||track>disc.lastTrack()){error(0x10);break;}
                if(disc.sectorCount()<=0){error(0x80);break;}
                stopRead();resetAudio();
                if(track!=0){readLocation=disc.trackStart(track);locationPending=false;}
                else if(locationPending){readLocation=location;locationPending=false;}
                motor=playing=true;playStep=1;sectorTicks=SINGLE_SECTOR_TICKS/((mode&128)!=0?2:1);event(0,3,status());
            }
            case 4,5 -> {
                if(!count(p,0))break;
                if(!playing){error(0x80);break;}
                playStep=command==4?Math.min(Math.max(1,playStep)+4,255):Math.max(Math.min(-1,playStep)-4,-255);
                event(0,3,status());
            }
            case 6,0x1b -> {
                if(!count(p,0) || !media())break;
                if(reading && !locationPending){event(0,3,status());break;}
                int target=locationPending?location:readLocation;boolean positioned=seekReady && target==readLocation;
                stopRead();resetAudio();locationPending=false;
                if(positioned && driveTicks==0){reading=true;sectorTicks=CdDriveTiming.sector((mode&128)!=0);}
                else if(positioned){seekTarget=target;seekTicks=driveTicks;readAfterSeek=true;}
                else seek(target,true);
                event(0,3,status());
            }
            case 7,8,9,0x0a -> {
                if(!count(p,0))break;
                int old=status(),completion=command==9?CdDriveTiming.pause(reading||playing,(mode&128)!=0,readLocation,sectorTicks):command==0x0a?Math.max(CdDriveTiming.INIT,(mode&128)!=0?CdDriveTiming.HZ*7/10:0):command==8?(motor?((mode&128)!=0?25000000:13000000):7000):400000;
                stopRead();resetAudio();events.removeIf(e->e.irq==2);driveTicks=Math.max(driveTicks,completion);
                if(command==8)motor=false;else if(command==7 || command==0x0a)motor=disc!=null;
                if(command==0x0a){mode=file=channel=0;xaMuted=false;System.arraycopy(new int[]{128,0,128,0},0,pendingVolume,0,4);System.arraycopy(pendingVolume,0,volume,0,4);location=readLocation=0;locationPending=false;data=new byte[0];dataPosition=0;request=false;muted=false;}
                event(0,3,old);event(completion,2,status());
            }
            case 0x0b,0x0c -> {if(count(p,0)){muted=command==0x0b;event(0,3,status());}}
            case 0x0d -> {if(count(p,2)){if(file!=p[0] || channel!=p[1])resetAudio();file=p[0];channel=p[1];event(0,3,status());}}
            case 0x0e -> {if(count(p,1)){boolean changed=((mode^p[0])&128)!=0;mode=p[0];if(changed){int delay=CdDriveTiming.HZ*((mode&128)!=0?6:7)/10;driveTicks=delay;if(seekTicks>0)seekTicks+=delay;else if(reading||playing)sectorTicks+=delay;if(traceSink!=null)traceSink.accept("SPEED_CHANGE ticks="+delay+" double="+((mode&128)!=0));}event(0,3,status());}}
            case 0x0f -> {if(count(p,0))event(0,3,status(),mode,0,file,channel);}
            case 0x10 -> {if(count(p,0)){if(lastSector==null)error(0x80);else{int[] header=new int[8];for(int i=0;i<8;i++)header[i]=lastSector[12+i]&255;event(0,3,header);}}}
            case 0x11 -> {
                if(!count(p,0) || !media())break;int at=Math.max(0,readLocation-1),absolute=at+150;
                int relative=disc.trackRelative(at);
                event(0,3,bcd(disc.trackNumber(at)),bcd(disc.trackIndex(at)),bcd(relative/4500),bcd(relative/75%60),bcd(relative%75),bcd(absolute/4500),bcd(absolute/75%60),bcd(absolute%75));
            }
            case 0x12 -> {if(count(p,1) && media()){if(p[0]!=1)error(0x10);else{event(0,3,status());event(COMPLETE_TICKS,2,status());}}}
            case 0x13 -> {if(count(p,0) && media())event(0,3,status(),1,bcd(disc.lastTrack()));}
            case 0x14 -> {
                if(!count(p,1) || !media())break;int track=binary(p[0]);if(track<0 || track>disc.lastTrack()){error(0x10);break;}
                if(track==0 && disc.sectorCount()<=0)throw new UnsupportedOperationException("Disc lead-out length is unavailable");
                int frames=disc.trackStart(track)+150;event(0,3,status(),bcd(frames/4500),bcd(frames/75%60));
            }
            case 0x15,0x16 -> {
                if(!count(p,0) || !media())break;stopRead();resetAudio();locationPending=false;events.removeIf(e->e.irq==2);
                seek(location,false);event(0,3,status());
            }
            case 0x19 -> {
                if(p.length!=1){error(0x20);break;}
                switch(p[0]){
                    // Raw SCEx scanning at a data-track position finds no lead-in
                    // strings. This test does not change the mounted disc's identity.
                    case 0x04->{motor=true;scexTotal=scexMatches=0;event(0,3,status());}
                    case 0x05->event(0,3,scexTotal,scexMatches);
                    case 0x20->event(0,3,0x95,0x07,0x24,0xc1);
                    case 0x21->event(0,3,0);
                    case 0x22->event(0,3,'f','o','r',' ','U','/','C');
                    default->error(0x10);
                }
            }
            case 0x1a -> {
                if(!count(p,0))break;events.removeIf(e->e.irq==2);event(0,3,status());
                if(disc==null)event(CdDriveTiming.ID,5,8,0x40,0,0,0,0,0,0);
                else {char region=disc.region();event(CdDriveTiming.ID,2,status(),0,0x20,0,region==' '?' ':'S',region==' '?' ':'C',region==' '?' ':'E',region);}
            }
            case 0x1c -> {if(count(p,0)){stopRead();resetAudio();initializeScex();mode=0;event(0,3,status());}}
            case 0x1e -> {if(count(p,0) && media()){events.removeIf(e->e.irq==2);driveTicks=Math.max(driveTicks,CdDriveTiming.TOC);event(0,3,status());event(CdDriveTiming.TOC,2,status());}}
            default -> error(0x40);
        }
    }
    private String writesDiagnostic(){ArrayList<String> writes=new ArrayList<>();for(int value:recentWrites)writes.add(String.format(Locale.ROOT,"%d:%d=%02X",value>>>16,(value>>>8)&255,value&255));return writes.toString();}
    public String diagnostic(){return String.format(Locale.ROOT,"CD: %d commands, %d sectors, %d DMA bytes; IRQ=%d enable=%02X busy=%s mode=%02X LBA=%d commands=%s; writes=%s IRQreads=%d bank=%d statusReads=%d%s",commands,sectorsRead,dmaBytes,interrupt,enable,pendingCommand>=0,mode,readLocation,recentCommands,writesDiagnostic(),irqReads,lastReadBank,statusReads,lastMediaError.isEmpty()?"":" media error="+lastMediaError)+"; command overwrites="+commandOverwrites+"; XA "+xaSectors+" sectors / "+xaFrames+" frames / "+xaDroppedFrames+" dropped, "+audioBufferedFrames()+" buffered; CDDA "+cddaSectors+" sectors / "+cddaFrames+" frames";}
}
