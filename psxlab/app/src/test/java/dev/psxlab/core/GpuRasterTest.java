package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;

public class GpuRasterTest {
    private static void send(Gpu g,int... words){for(int w:words)g.write(w);}
    private static int xy(int x,int y){return (x&65535)|(y<<16);}
    private static void area(Gpu g){send(g,0xe3000000,0xe407ffff);}
    private static int raw(Gpu g,int x,int y){send(g,0xc0000000,xy(x,y),xy(1,1));return g.read()&65535;}
    private static void put(Gpu g,int x,int y,int... pixels){send(g,0xa0000000,xy(x,y),xy(pixels.length,1));for(int i=0;i<pixels.length;i+=2)g.write(pixels[i]|(i+1<pixels.length?pixels[i+1]<<16:0));}
    private static void quad(Gpu g,int op,int color,int x,int y,int w,int h){send(g,(op<<24)|color,xy(x,y),xy(x+w,y),xy(x,y+h),xy(x+w,y+h));}
    @Test public void command2aBlendsQuadWithoutDoubleDrawingDiagonal(){
        try(Gpu g=new Gpu(false)){area(g);put(g,1,1,0,0,0,0);put(g,1,2,0,0,0,0);quad(g,0x2a,128,1,1,4,2);for(int y=1;y<3;y++)for(int x=1;x<5;x++)assertEquals(8,raw(g,x,y));assertEquals(0,raw(g,5,2));assertEquals(0,raw(g,2,3));}
    }
    @Test public void triangleWindingDoesNotChangeCoverage(){
        int[][] frames=new int[2][];for(int i=0;i<2;i++)try(Gpu g=new Gpu(false)){area(g);send(g,0x200000ff,xy(2,2),i==0?xy(8,2):xy(2,8),i==0?xy(2,8):xy(8,2));frames[i]=g.snapshot();assertEquals(31,raw(g,2,2));assertEquals(0,raw(g,7,7));}assertArrayEquals(frames[0],frames[1]);
    }
    @Test public void offsetsSignExtendAndClipAtInclusiveDrawArea(){
        try(Gpu g=new Gpu(false)){send(g,0xe3000000|3|(3<<10),0xe4000000|5|(5<<10),0xe5000000|0x7fe|(0x7ff<<11));quad(g,0x28,0xff00,3,3,8,8);assertEquals(0,raw(g,2,3));assertEquals(0x3e0,raw(g,3,3));assertEquals(0x3e0,raw(g,5,5));assertEquals(0,raw(g,6,5));}
    }
    @Test public void oversizedAndDegenerateTrianglesAreDiscarded(){
        try(Gpu g=new Gpu(false)){area(g);send(g,0x200000ff,xy(-1,0),xy(1023,0),xy(0,2));send(g,0x200000ff,xy(0,-1),xy(2,0),xy(0,511));send(g,0x200000ff,xy(1,1),xy(2,2),xy(3,3));assertEquals(0,raw(g,1,1));}
    }
    @Test public void gouraudColorsInterpolateAcrossTriangle(){
        try(Gpu g=new Gpu(false)){area(g);send(g,0x300000ff,xy(0,0),0xff00,xy(8,0),0xff0000,xy(0,8));assertEquals(31,raw(g,0,0));int mid=raw(g,2,2);assertTrue((mid&31)>0);assertTrue(((mid>>>5)&31)>0);assertTrue(((mid>>>10)&31)>0);}
    }
    @Test public void allFourBlendModesClampChannels(){
        int[] expected={24,31,0,27};for(int mode=0;mode<4;mode++)try(Gpu g=new Gpu(false)){area(g);put(g,2,2,20);send(g,0xe1000000|(mode<<5));send(g,0x6a0000e0,xy(2,2));assertEquals(expected[mode],raw(g,2,2));}
    }
    @Test public void textureZeroIsTransparentBut8000IsBlackWithMask(){
        try(Gpu g=new Gpu(false)){area(g);put(g,640,0,0,0x8000,31);put(g,0,0,0x3e0,0x3e0,0x3e0);send(g,0xe100010a);send(g,0x65000000,xy(0,0),0,xy(3,1));assertEquals(0x3e0,raw(g,0,0));assertEquals(0x8000,raw(g,1,0));assertEquals(31,raw(g,2,0));}
    }
    @Test public void fourBitAndEightBitTexturesUseClut(){
        for(int depth=0;depth<2;depth++)try(Gpu g=new Gpu(false)){area(g);put(g,640,0,depth==0?0x21:0x0201);put(g,16,300,0,31,0x3e0);send(g,0xe100000a|(depth<<7));send(g,0x65000000,xy(0,0),(300*64+1)<<16,xy(2,1));assertEquals(31,raw(g,0,0));assertEquals(0x3e0,raw(g,1,0));}
    }
    @Test public void texturedPolygonUpdatesPageAndInterpolatesUv(){
        try(Gpu g=new Gpu(false)){area(g);put(g,640,0,31,0x3e0,0x7c00,0x7fff);send(g,0x25000000,xy(0,0),0,xy(4,0),4|(0x10a<<16),xy(0,4),4<<8);assertEquals(31,raw(g,0,0));assertEquals(0x3e0,raw(g,1,0));assertEquals(0x10a,g.status()&511);send(g,0x6d000000,xy(10,10),2);assertEquals(0x7c00,raw(g,10,10));}
    }
    @Test public void texturedGouraudQuadConsumesAllTwelveWords(){
        try(Gpu g=new Gpu(false)){area(g);put(g,640,0,31,31,31,31);send(g,0x3d808080,xy(0,0),0,0x808080,xy(4,0),(0x10a<<16)|4,0x808080,xy(0,4),4<<8,0x808080,xy(4,4),4|(4<<8));send(g,0x6800ff00,xy(20,20));assertEquals(31,raw(g,1,0));assertEquals(0x3e0,raw(g,20,20));}
    }
    @Test public void textureModulationUses80AsNeutralAndCanBrighten(){
        try(Gpu g=new Gpu(false)){area(g);put(g,640,0,8|(8<<5)|(8<<10));send(g,0xe100010a);for(int color:new int[]{0x808080,0x404040,0xffffff}){send(g,0x64000000|color,xy(0,0),0,xy(1,1));int expected=color==0x808080?8:color==0x404040?4:15;assertEquals(expected,raw(g,0,0)&31);}}
    }
    @Test public void texturedSemiTransparencyOnlyBlendsTexelsWithBit15(){
        try(Gpu g=new Gpu(false)){area(g);put(g,640,0,8,0x8008);put(g,0,0,24,24);send(g,0xe100010a);send(g,0x67000000,xy(0,0),0,xy(2,1));assertEquals(8,raw(g,0,0));assertEquals(0x8010,raw(g,1,0));}
    }
    @Test public void textureWindowAndRectangleFlipsChangeSampling(){
        try(Gpu g=new Gpu(false)){area(g);put(g,648,0,31,0x3e0,0x7c00);send(g,0xe100110a,0xe2000401);send(g,0x65000000,xy(0,0),10,xy(3,1));assertEquals(0x7c00,raw(g,0,0));assertEquals(0x3e0,raw(g,1,0));assertEquals(31,raw(g,2,0));}
    }
    @Test public void rectanglesCoverVariableOneEightAndSixteenSizes(){
        try(Gpu g=new Gpu(false)){area(g);send(g,0x600000ff,xy(0,0),xy(3,2));send(g,0x6800ff00,xy(10,0));send(g,0x700000ff,xy(20,0));send(g,0x780000ff,xy(40,0));assertEquals(31,raw(g,2,1));assertEquals(0,raw(g,3,1));assertEquals(0x3e0,raw(g,10,0));assertEquals(31,raw(g,27,7));assertEquals(0,raw(g,28,7));assertEquals(31,raw(g,55,15));assertEquals(0,raw(g,56,15));}
    }
    @Test public void maskBitsAffectDrawingUploadsAndCopiesButNotFill(){
        try(Gpu g=new Gpu(false)){area(g);put(g,0,0,0x8001);send(g,0xe6000002);send(g,0x680000ff,0);put(g,0,0,31);assertEquals(0x8001,raw(g,0,0));send(g,0xe6000001);put(g,2,2,31);assertEquals(0x801f,raw(g,2,2));send(g,0x80000000,xy(2,2),xy(3,3),xy(1,1));assertEquals(0x801f,raw(g,3,3));send(g,0xe6000003,0x0200ff00,0,xy(16,1));assertEquals(0x3e0,raw(g,0,0));}
    }
    @Test public void ditherAffectsGouraudPolygonsAndLinesButNotFlatRects(){
        try(Gpu g=new Gpu(false)){area(g);send(g,0xe1000200,0x30000008,xy(0,0),8,xy(8,0),8,xy(0,8));assertEquals(0,raw(g,0,0));assertEquals(1,raw(g,1,0));send(g,0x60000008,xy(0,10),xy(4,1));for(int x=0;x<4;x++)assertEquals(1,raw(g,x,10));}
    }
    @Test public void linesIncludeEndpointsAndPolylinesTerminateWithoutEatingNextCommand(){
        try(Gpu g=new Gpu(false)){area(g);send(g,0x480000ff,xy(0,0),xy(3,0),xy(3,3),0x50005000,0x6800ff00,xy(10,10));assertEquals(31,raw(g,0,0));assertEquals(31,raw(g,3,3));assertEquals(0x3e0,raw(g,10,10));}
    }
    @Test public void shadedPolylineAcceptsColorWordsAndBufferReset(){
        try(Gpu g=new Gpu(false)){area(g);send(g,0x580000ff,xy(0,0),0xff00,xy(4,0),0xff0000,xy(4,4),0x55555555);assertEquals(31,raw(g,0,0));assertEquals(0x7c00,raw(g,4,4));send(g,0x480000ff,xy(0,0),xy(1,0));g.control(0x01000000);send(g,0x6800ff00,xy(10,10));assertEquals(0x3e0,raw(g,10,10));}
    }
    @Test public void threadedEnvironmentAndTextureWritesStayOrdered(){
        int[][] frames=new int[2][];for(int mode=0;mode<2;mode++)try(Gpu g=new Gpu(mode==1)){area(g);for(int i=0;i<400;i++){send(g,0xe5000000|i);send(g,0x680000ff,0);send(g,0xe6000001);put(g,640,0,i&31);send(g,0xe6000000,0xe100010a,0x6d000000,xy(0,2),0);}frames[mode]=g.snapshot();}assertArrayEquals(frames[0],frames[1]);
    }
    @Test public void scanoutUsesSelectedBufferAndDisplayEnable(){
        try(Gpu g=new Gpu(false)){put(g,100,256,31);g.control(0x05000000|100|(256<<10));g.control(0x06000000|(8<<12));g.control(0x07000000|(1<<10));g.control(0x08000001);Gpu.Frame disabled=g.displayFrame();assertEquals(0xff000000,disabled.pixels()[0]);g.control(0x03000000);Gpu.Frame frame=g.displayFrame();assertEquals(1,frame.width());assertEquals(1,frame.height());assertEquals(0xffff0000,frame.pixels()[0]);}
    }
    @Test public void packed24BitScanoutUsesThreeBytesPerPixel(){
        try(Gpu g=new Gpu(false)){put(g,10,0,0x2211,0x4433,0x6655);g.control(0x0500000a);g.control(0x03000000);g.control(0x08000011);g.control(0x06000000|(16<<12));g.control(0x07000000|(1<<10));Gpu.Frame frame=g.displayFrame();assertEquals(2,frame.width());assertEquals(0xff112233,frame.pixels()[0]);assertEquals(0xff445566,frame.pixels()[1]);}
    }
    @Test public void gpuIrqRaisesIrq1UntilAcknowledged(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);b.write16(0x1f801074,2);g.write(0x1f000000);g.tickVideo(b,1);assertTrue(b.interruptPending());g.control(0x02000000);b.write16(0x1f801070,0xfffd);g.tickVideo(b,1);assertFalse(b.interruptPending());}
    }
    @Test public void fixedGradientsStayWithinOneColorStepOfBarycentricReference(){
        try(Gpu g=new Gpu(false)){
            area(g);send(g,0x300000ff,xy(-20,-10),0x00ff00,xy(240,16),0xff0000,xy(20,200));int[] pixels=g.snapshot();
            long area=260L*210-26L*40;
            for(int y=0;y<190;y+=7)for(int x=0;x<230;x+=7){
                long wa=(-220L)*(y-16)-184L*(x-240),wb=(-40L)*(y-200)+210L*(x-20),wc=260L*(y+10)-26L*(x+20);
                if(wa<=0 || wb<=0 || wc<=0)continue;
                int pixel=pixels[y*1024+x];
                assertTrue(Math.abs(((pixel>>>19)&31)-(int)(wa*255/area/8))<=1);
                assertTrue(Math.abs(((pixel>>>11)&31)-(int)(wb*255/area/8))<=1);
                assertTrue(Math.abs(((pixel>>>3)&31)-(int)(wc*255/area/8))<=1);
            }
        }
    }
}
