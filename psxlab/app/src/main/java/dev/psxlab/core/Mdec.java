package dev.psxlab.core;

import java.util.ArrayDeque;
import java.util.Arrays;

/** Original functional MDEC: streaming RLE, programmable IDCT and pixel packing.
 * Register/format reference: psx-spx.consoledev.net/macroblockdecodermdec/.
 * Processing is synchronous; FIFO timing and hardware-exact rounding are not modeled.
 */
public final class Mdec {
    private static final int[] ZIGZAG={
        0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,
        12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
        35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
        58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};
    private record Output(int[] words,int block,int depth){}
    private final Bus bus;
    private final int[] luma=new int[64],chroma=new int[64],scale=new int[64];
    private final int[] coefficients=new int[64],temporary=new int[64];
    private final int[][] blocks=new int[6][64];
    private final ArrayDeque<Output> output=new ArrayDeque<>();
    private int command,kind,remaining,parameterIndex,format,control,block,coefficient=-1,qScale;
    private int outputIndex,outputWords,idleCount,lastRead;
    public long decodedBlocks,resets,quantUploads,scaleUploads;

    Mdec(Bus bus){this.bus=bus;reset();}
    private void reset(){
        command=kind=remaining=parameterIndex=format=control=block=outputIndex=outputWords=idleCount=0;
        coefficient=-1;lastRead=0;output.clear();resets++;
        // Quantization and scale RAM survive a command abort.
    }
    public void control(int value){
        if(value<0)reset();
        control=value&0x60000000;
        bus.dma.resumeMdec();
    }
    public int status(){
        int current=output.isEmpty()?(block<2?block+4:block-2):output.peek().block;
        if((format>>>25&3)<2)current=4;
        // Synchronous IDCT queues future macroblocks ahead of the hardware output
        // stage. Accepting the last input word does not make that backlog ready.
        // The final macroblock may remain in the output FIFO after busy clears.
        int finalWords=switch(format>>>25&3){case 0->8;case 1->16;case 2->192;default->128;};
        boolean busy=remaining>0||outputWords>finalWords;
        return (outputWords==0?0x80000000:0)|(busy?0x20000000:0)
            |(inputRequest()?0x10000000:0)|(outputRequest()?0x08000000:0)
            |format|(current<<16)|(remaining>0?(remaining-1)&65535:idleCount);
    }
    public boolean inputRequest(){return (control&0x40000000)!=0;}
    public boolean outputRequest(){return (control&0x20000000)!=0 && outputWords>0;}
    public int availableWords(){return outputWords;}
    public void write(int value){
        if(remaining==0){
            command=value;kind=value>>>29;format=(value>>>2)&0x07800000;parameterIndex=0;
            coefficient=-1;block=0;
            remaining=switch(kind){case 1->value&65535;case 2->(value&1)!=0?32:16;case 3->32;default->0;};
            idleCount=remaining>0?65535:value&65535;
        }else{
            if(kind==2){
                for(int i=0;i<4;i++){int at=parameterIndex*4+i;(at<64?luma:chroma)[at&63]=(value>>>(i*8))&255;}
            }else if(kind==3){scale[parameterIndex*2]=(short)value;scale[parameterIndex*2+1]=(short)(value>>>16);}
            else if(kind==1){halfword(value&65535);halfword(value>>>16);}
            parameterIndex++;remaining--;
            if(remaining==0){idleCount=65535;if(kind==2)quantUploads++;if(kind==3)scaleUploads++;}
        }
        bus.dma.resumeMdec();
    }
    private void halfword(int value){
        if(coefficient<0){
            if(value==0xfe00)return;
            Arrays.fill(coefficients,0);coefficient=0;qScale=value>>>10;
        }else{
            coefficient+=(value>>>10)+1;
            if(coefficient>=64){
                transform(blocks[block]);decodedBlocks++;
                int depth=format>>>25&3;
                if(depth<2){packMono(blocks[block],depth);block=0;}
                else{if(block>=2)packColor(block-2,depth);block=(block+1)%6;}
                coefficient=-1;return;
            }
        }
        int signed=(value<<22)>>22;
        int[] quant=(format>>>25&3)>=2 && block<2?chroma:luma;
        int v=qScale==0?signed*2:coefficient==0?signed*quant[0]:(signed*quant[coefficient]*qScale+4)>>3;
        coefficients[qScale==0?coefficient:ZIGZAG[coefficient]]=clamp(v,-1024,1023);
    }
    private void transform(int[] destination){
        int[] source=coefficients,target=temporary;
        for(int pass=0;pass<2;pass++){
            for(int y=0;y<8;y++)for(int x=0;x<8;x++){
                long sum=0;for(int z=0;z<8;z++)sum+=(long)source[y+z*8]*(scale[x+z*8]>>3);
                target[x+y*8]=(int)((sum+4095)>>13);
            }
            source=target;target=destination;
        }
    }
    private int sample(int value){
        value=(value<<23)>>23;return component(value);
    }
    private int component(int value){
        value=clamp(value,-128,127);
        return (value&255)^((format&0x1000000)==0?128:0);
    }
    private void packMono(int[] pixels,int depth){
        int perWord=depth==0?8:4,bits=depth==0?4:8;
        int[] words=new int[64/perWord];
        for(int i=0;i<64;i++){int v=sample(pixels[i]);if(bits==4)v>>>=4;words[i/perWord]|=v<<((i%perWord)*bits);}
        enqueue(words,4,depth);
    }
    private void packColor(int quadrant,int depth){
        byte[] pixels=new byte[depth==3?128:192];int at=0;
        int xOffset=(quadrant&1)*8,yOffset=(quadrant>>>1)*8;
        for(int y=0;y<8;y++)for(int x=0;x<8;x++){
            int uv=(y+yOffset)/2*8+(x+xOffset)/2;
            int cr=blocks[0][uv],cb=blocks[1][uv],l=blocks[quadrant+2][y*8+x];
            int r=component(l+((1436*cr+512)>>10));
            int g=component(l+((-352*cb-731*cr+512)>>10));
            int b=component(l+((1815*cb+512)>>10));
            if(depth==3){int rgb=(r>>>3)|((g>>>3)<<5)|((b>>>3)<<10)|((format&0x800000)!=0?0x8000:0);pixels[at++]=(byte)rgb;pixels[at++]=(byte)(rgb>>>8);}
            else{pixels[at++]=(byte)r;pixels[at++]=(byte)g;pixels[at++]=(byte)b;}
        }
        int[] words=new int[pixels.length/4];
        for(int i=0;i<pixels.length;i++)words[i/4]|=(pixels[i]&255)<<((i&3)*8);
        enqueue(words,quadrant,depth);
    }
    private void enqueue(int[] words,int quadrant,int depth){output.add(new Output(words,quadrant,depth));outputWords+=words.length;}
    public int read(){
        if(outputWords==0)return lastRead;
        Output first=output.peek();lastRead=first.words[outputIndex++];outputWords--;
        if(outputIndex==first.words.length){output.remove();outputIndex=0;}
        return lastRead;
    }
    /** DMA1 interleaves the four 8x8 output blocks into a 16x16 macroblock. */
    public int dmaAddressOffset(){
        Output first=output.peek();if(first==null || first.depth<2)return 0;
        int rowWords=first.depth==3?4:6;
        int linear=first.block*rowWords*8+outputIndex;
        int raster=((first.block/2)*8+outputIndex/rowWords)*rowWords*2+(first.block&1)*rowWords+outputIndex%rowWords;
        return (raster-linear)*4;
    }
    private static int clamp(int value,int low,int high){return Math.max(low,Math.min(high,value));}
    public String diagnostic(){return "MDEC blocks="+decodedBlocks+" resets="+resets+" tables="+quantUploads+"/"+scaleUploads+" pending="+remaining+" output="+outputWords+String.format(java.util.Locale.ROOT," status=%08X",status());}
}
