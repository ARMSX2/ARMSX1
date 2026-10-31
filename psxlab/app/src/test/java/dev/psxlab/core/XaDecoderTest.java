package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
import java.util.*;

public class XaDecoderTest {
    static byte[] sector(int coding,int file,int channel){
        byte[] raw=new byte[2352];raw[15]=2;raw[16]=(byte)file;raw[17]=(byte)channel;raw[18]=0x64;raw[19]=(byte)coding;System.arraycopy(raw,16,raw,20,4);
        for(int g=0;g<18;g++){int base=24+g*128;Arrays.fill(raw,base,base+16,(byte)8);for(int i=0;i<28;i++)for(int col=0;col<4;col++)raw[base+16+i*4+col]=(byte)((coding&16)!=0?(col%2==0?0xfe:2):0x21);}
        return raw;
    }
    @Test public void fourBitStereoUsesIndependentLowHighNibbles(){XaDecoder x=new XaDecoder();assertEquals(2016,x.decodeSource(sector(1,1,0)));assertEquals(16,x.sourceSample(0,0));assertEquals(32,x.sourceSample(1,0));assertEquals(16,x.sourceSample(0,28));}
    @Test public void monoOrdersBothNibblesThenNextByteColumn(){XaDecoder x=new XaDecoder();assertEquals(4032,x.decodeSource(sector(0,1,0)));assertEquals(16,x.sourceSample(0,0));assertEquals(32,x.sourceSample(0,28));assertEquals(16,x.sourceSample(0,56));assertEquals(32,x.sourceSample(1,28));}
    @Test public void signedEightBitStereoUsesSeparateByteColumns(){XaDecoder x=new XaDecoder();assertEquals(1008,x.decodeSource(sector(17,1,0)));assertEquals(-2,x.sourceSample(0,0));assertEquals(2,x.sourceSample(1,0));assertEquals(-2,x.sourceSample(0,28));}
    @Test public void negativeFourBitNibblesAreSignExtended(){byte[] raw=sector(1,1,0);for(int g=0;g<18;g++)Arrays.fill(raw,24+g*128+16,24+g*128+128,(byte)0x8f);XaDecoder x=new XaDecoder();x.decodeSource(raw);assertEquals(-16,x.sourceSample(0,0));assertEquals(-128,x.sourceSample(1,0));}
    @Test public void reservedShiftActsAsNine(){byte[] raw=sector(1,1,0);for(int g=0;g<18;g++)Arrays.fill(raw,24+g*128,24+g*128+16,(byte)15);XaDecoder x=new XaDecoder();x.decodeSource(raw);assertEquals(8,x.sourceSample(0,0));assertEquals(16,x.sourceSample(1,0));}
    @Test public void sourceAndOutputDurationsMatchAllCodingModes(){
        for(int coding:new int[]{0,1,4,5,16,17,20,21}){XaDecoder x=new XaDecoder();int source=18*28*((coding&16)!=0?4:8)/((coding&1)!=0?2:1);int expected=source*((coding&4)!=0?2:1)*7/6;int[] count={0};assertEquals(expected,x.decode(sector(coding,1,0),(l,r)->count[0]++));assertEquals(expected,count[0]);}
    }
    @Test public void resamplerPreservesDocumentedDcGainAndStereoAfterPriming(){XaDecoder x=new XaDecoder();List<int[]> out=new ArrayList<>();x.decode(sector(1,1,0),(l,r)->out.add(new int[]{l,r}));for(int i=100;i<out.size();i++){assertEquals(14,out.get(i)[0],1);assertEquals(28,out.get(i)[1],2);}}
    @Test public void predictionHistoryCrossesSectorsAndResetClearsIt(){
        byte[] first=sector(0,1,0),next=sector(0,1,0);for(byte[] raw:new byte[][]{first,next})for(int g=0;g<18;g++){Arrays.fill(raw,24+g*128,24+g*128+16,(byte)0x18);Arrays.fill(raw,24+g*128+16,24+g*128+128,(byte)0);}first[24+17*128+16+27*4+3]=0x10;
        XaDecoder x=new XaDecoder();x.decodeSource(first);int last=x.sourceSample(0,4031);assertTrue(last>0);x.decodeSource(next);assertEquals((last*60+32)>>6,x.sourceSample(0,0));x.reset();x.decodeSource(next);assertEquals(0,x.sourceSample(0,0));
    }
    @Test public void saturationCannotWrapSign(){byte[] raw=sector(0,1,0);for(int g=0;g<18;g++){Arrays.fill(raw,24+g*128,24+g*128+16,(byte)0x20);Arrays.fill(raw,24+g*128+16,24+g*128+128,(byte)0x77);}XaDecoder x=new XaDecoder();x.decodeSource(raw);assertEquals(32767,x.sourceSample(0,10));}
    @Test public void malformedRawSectorIsRejected(){try{new XaDecoder().decodeSource(new byte[2048]);fail();}catch(IllegalArgumentException expected){}}
}
