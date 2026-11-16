package dev.psxlab.core;

import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;
import org.junit.Test;
import static org.junit.Assert.*;

/** Bit-exact decoding checks and a movie-sized benchmark of the old and accelerated IDCT. */
public class MdecPerformanceTest {
    private static volatile int checksum;
    private static void original(int[] coefficients,int[] scale,int[] temporary,int[] destination){
        int[] source=coefficients,target=temporary;
        for(int pass=0;pass<2;pass++){
            for(int y=0;y<8;y++)for(int x=0;x<8;x++){
                long sum=0;for(int z=0;z<8;z++)sum+=(long)source[y+z*8]*(scale[x+z*8]>>3);
                target[x+y*8]=(int)((sum+4095)>>13);
            }
            source=target;target=destination;
        }
    }
    private static int[] standardScale(){
        int[] scale=new int[64];for(int z=0;z<8;z++)for(int x=0;x<8;x++)scale[x+z*8]=(short)Math.round(Math.cos((2*x+1)*z*Math.PI/16)*(z==0?1/Math.sqrt(2):1)*32768);return scale;
    }
    @Test public void nativeNeonTransformMatchesTheOriginalForDenseSparseAndEditedBlocks(){
        assertTrue(NativeMdec.AVAILABLE);Random random=new Random(0x49444354);int[] source=new int[64],scale=new int[64],actual=new int[64],expected=new int[64],temporary=new int[64];
        for(int fixture=0;fixture<6000;fixture++){
            for(int i=0;i<64;i++){source[i]=fixture%3==0&&i>0?0:fixture%3==1&&random.nextInt(8)!=0?0:random.nextInt(2048)-1024;scale[i]=(short)random.nextInt();}
            if(fixture%5==0)scale=standardScale();
            if(fixture%37==0)source[random.nextInt(64)]=random.nextInt();
            if(fixture%53==0)scale[random.nextInt(64)]=random.nextInt();
            original(source,scale,temporary,expected);NativeMdec.transform(source,scale,actual);assertArrayEquals("Native IDCT fixture "+fixture,expected,actual);
        }
    }
    private static void tables(Bus bus,int[] scale,Random random){
        bus.mdec.write(0x40000001);for(int i=0;i<32;i++)bus.mdec.write(random.nextInt());bus.mdec.write(0x60000000);
        for(int i=0;i<64;i+=2)bus.mdec.write((scale[i]&65535)|(scale[i+1]<<16));
    }
    @Test public void wholeDecoderOutputAndStatusStayIdenticalForEveryPixelFormat()throws Exception{
        assertTrue(NativeMdec.AVAILABLE);Random random=new Random(0x535452);
        for(int depth=0;depth<4;depth++)for(int flags=0;flags<4;flags++)try(Gpu ga=new Gpu(false);Gpu gb=new Gpu(false)){
            Bus reference=new Bus(ga,new PadState(),false),accelerated=new Bus(gb,new PadState(),true);long seed=random.nextLong();int[] scale=standardScale();
            if((flags&1)!=0)for(int i=0;i<64;i++)scale[i]=(short)random.nextInt();
            tables(reference,scale,new Random(seed));tables(accelerated,scale,new Random(seed));
            ArrayList<Integer> halfwords=new ArrayList<>();int blocks=(depth<2?1:6)*8;
            for(int block=0;block<blocks;block++){
                int qScale=block%4==0?0:1+random.nextInt(63);halfwords.add((qScale<<10)|random.nextInt(1024));int coefficient=0;
                if(block%3!=0)for(int ac=0;ac<12;ac++){int run=random.nextInt(7);coefficient+=run+1;if(coefficient>=64)break;halfwords.add((run<<10)|random.nextInt(1024));}
                halfwords.add(0xfe00);
            }
            if((halfwords.size()&1)!=0)halfwords.add(0xfe00);
            int command=0x20000000|(depth<<27)|((flags&1)<<26)|((flags>>>1)<<25)|(halfwords.size()/2);
            reference.mdec.write(command);accelerated.mdec.write(command);
            for(int at=0;at<halfwords.size();at+=2){int value=halfwords.get(at)|(halfwords.get(at+1)<<16);reference.mdec.write(value);accelerated.mdec.write(value);assertEquals(reference.mdec.status(),accelerated.mdec.status());}
            assertEquals(blocks,accelerated.mdec.decodedBlocks);assertTrue(accelerated.mdec.dcOnlyBlocks>0);assertTrue(accelerated.mdec.nativeIdctBlocks>0);
            while(reference.mdec.availableWords()>0){assertEquals(reference.mdec.dmaAddressOffset(),accelerated.mdec.dmaAddressOffset());assertEquals(reference.mdec.read(),accelerated.mdec.read());assertEquals(reference.mdec.status(),accelerated.mdec.status());}
            assertEquals(0,accelerated.mdec.availableWords());
            StateCodec.apply(StateCodec.capture(reference.mdec),accelerated.mdec);
            assertEquals(reference.mdec.status(),accelerated.mdec.status());
        }
    }
    @Test public void measureOneMovieFrameUsingTheOriginalAndProductionTransformPaths()throws Exception{
        assertTrue(NativeMdec.AVAILABLE);int[][] blocks=new int[1800][64];int[] scale=standardScale(),temporary=new int[64],output=new int[64];Random random=new Random(0x4d4f544f);
        for(int block=0;block<blocks.length;block++)for(int i=0;i<64;i++)blocks[block][i]=block%3==0&&i>0?0:block%3==1&&random.nextInt(8)!=0?0:random.nextInt(2048)-1024;
        long[] before=new long[5],after=new long[5];int oldChecksum=0,newChecksum=0;
        for(int trial=-2;trial<5;trial++){
            int sum=0;long started=android.os.Debug.threadCpuTimeNanos();
            for(int[] block:blocks){original(block,scale,temporary,output);sum+=output[0];}
            if(trial>=0)before[trial]=android.os.Debug.threadCpuTimeNanos()-started;oldChecksum=sum;checksum=sum;
            sum=0;started=android.os.Debug.threadCpuTimeNanos();
            for(int[] block:blocks){if(!Mdec.transformDc(block,scale,temporary,output))NativeMdec.transform(block,scale,output);sum+=output[0];}
            if(trial>=0)after[trial]=android.os.Debug.threadCpuTimeNanos()-started;newChecksum=sum;checksum=sum;
        }
        assertEquals(oldChecksum,newChecksum);Arrays.sort(before);Arrays.sort(after);
        String json=String.format(Locale.ROOT,"{\"blocks\":1800,\"original_median_cpu_ns\":%d,\"optimized_median_cpu_ns\":%d,\"speedup\":%.3f,\"checksum\":%d}",before[2],after[2],before[2]/(double)after[2],checksum);
        File folder=new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getExternalFilesDir(null),"mdec-validation");assertTrue(folder.isDirectory()||folder.mkdirs());Files.write(new File(folder,"benchmark.json").toPath(),json.getBytes(StandardCharsets.UTF_8));android.util.Log.i("MdecPerformanceTest",json);
    }
}
