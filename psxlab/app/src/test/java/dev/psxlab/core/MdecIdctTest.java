package dev.psxlab.core;

import java.util.Arrays;
import java.util.Random;
import org.junit.Test;
import static org.junit.Assert.*;

public class MdecIdctTest {
    private static void original(int[] coefficients,int[] scale,int[] destination){
        int[] temporary=new int[64],source=coefficients,target=temporary;
        for(int pass=0;pass<2;pass++){
            for(int y=0;y<8;y++)for(int x=0;x<8;x++){
                long sum=0;for(int z=0;z<8;z++)sum+=(long)source[y+z*8]*(scale[x+z*8]>>3);
                target[x+y*8]=(int)((sum+4095)>>13);
            }
            source=target;target=destination;
        }
    }
    @Test public void dcShortcutMatchesBothOriginalPassesForStandardAndCustomTables(){
        Random random=new Random(0x4d444543);int[] source=new int[64],scale=new int[64],temporary=new int[64],actual=new int[64],expected=new int[64];
        for(int fixture=0;fixture<4096;fixture++){
            source[0]=fixture%17==0?0:fixture%31==0?random.nextInt():random.nextInt(2048)-1024;
            for(int i=0;i<64;i++)scale[i]=(short)random.nextInt();
            if((fixture&1)==0){int value=scale[0]>>3;for(int i=0;i<8;i++)scale[i]=(value<<3)|i;}
            original(source,scale,expected);assertTrue(Mdec.transformDc(source,scale,temporary,actual));assertArrayEquals("DC fixture "+fixture,expected,actual);
        }
    }
    @Test public void acBlocksKeepTheirDestinationForTheFullTransform(){
        int[] source=new int[64],scale=new int[64],destination=new int[64];Arrays.fill(destination,12345);source[63]=1;
        assertFalse(Mdec.transformDc(source,scale,new int[64],destination));for(int value:destination)assertEquals(12345,value);
    }
    @Test public void diagnosticExecutionSettingsAreExcludedFromGuestStates()throws Exception{
        try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);StateCodec.Node state=StateCodec.capture(bus.mdec);
            for(String field:new String[]{"nativeIdct","dcOnlyBlocks","nativeIdctBlocks"})assertFalse(state.fields.containsKey(field));
        }
    }
}
