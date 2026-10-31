package dev.psxlab.core;

/** Original functional GTE implementation. Command pipeline timing is not yet modeled. */
final class ReferenceGte {
    boolean widescreen;
    private final int[] data=new int[32],control=new int[32];
    private int shift;
    private boolean limit;
    ReferenceGte(){reset();}
    public void reset(){java.util.Arrays.fill(data,0);java.util.Arrays.fill(control,0);data[31]=32;}
    private static int clamp(long v,int min,int max){return (int)Math.max(min,Math.min(max,v));}
    public int readData(int reg){
        return switch(reg){
            case 1,3,5,8,9,10,11 -> (short)data[reg];
            case 7,16,17,18,19 -> data[reg]&65535;
            case 15 -> data[14];
            case 28,29 -> clamp((short)data[9]>>7,0,31)|(clamp((short)data[10]>>7,0,31)<<5)|(clamp((short)data[11]>>7,0,31)<<10);
            default -> data[reg];
        };
    }
    public void writeData(int reg,int value){
        switch(reg){
            case 1,3,5,8,9,10,11 -> data[reg]=(short)value;
            case 7,16,17,18,19 -> data[reg]=value&65535;
            case 15 -> {data[12]=data[13];data[13]=data[14];data[14]=value;}
            case 28 -> {data[9]=(value&31)<<7;data[10]=((value>>>5)&31)<<7;data[11]=((value>>>10)&31)<<7;}
            case 29,31 -> {}
            case 30 -> {data[30]=value;data[31]=Integer.numberOfLeadingZeros(value<0?~value:value);}
            default -> data[reg]=value;
        }
    }
    public int readControl(int reg){return control[reg];}
    public void writeControl(int reg,int value){
        switch(reg){
            case 4,12,20,26,27,29,30 -> control[reg]=(short)value;
            case 31 -> {control[31]=value&0x7ffff000;summary();}
            default -> control[reg]=value;
        }
    }
    private void flag(int bit){control[31]|=1<<bit;}
    private void summary(){control[31]&=0x7fffffff;if((control[31]&0x7f87e000)!=0)control[31]|=0x80000000;}
    private long wrap(int i,long value){
        if(value>0x7ffffffffffL)flag(31-i);if(value< -0x80000000000L)flag(28-i);
        return (value<<20)>>20;
    }
    private int saturate(long value,int min,int max,int bit){if(value<min || value>max)flag(bit);return clamp(value,min,max);}
    private void result(int i,long value){
        int m=(int)(wrap(i,value)>>shift);data[24+i]=m;data[8+i]=saturate(m,limit?0:-32768,32767,25-i);
    }
    private void mac0(long value){if(value>Integer.MAX_VALUE)flag(16);if(value<Integer.MIN_VALUE)flag(15);data[24]=(int)value;}
    private int[] vector(int index){return index==3?new int[]{(short)data[9],(short)data[10],(short)data[11]}:new int[]{(short)data[index*2],(short)(data[index*2]>>>16),(short)data[index*2+1]};}
    private int[] matrix(int index){
        if(index==3){int red=(data[6]&255)<<4;return new int[]{-red,red,(short)data[8],(short)control[1],(short)control[1],(short)control[1],(short)control[2],(short)control[2],(short)control[2]};}
        int[] out=new int[9];for(int i=0;i<9;i++)out[i]=(short)(control[index*8+i/2]>>>((i&1)*16));return out;
    }
    private int translation(int index,int row){return index==3?0:control[(index==0?5:index==1?13:21)+row];}
    private long[] multiply(int matrixIndex,int[] v,int translate,boolean farBug){
        int[] m=matrix(matrixIndex);long[] values=new long[3];
        for(int row=0;row<3;row++){
            int i=row+1;long sum=wrap(i,((long)translation(translate,row)<<12)+(long)m[row*3]*v[0]);
            if(farBug){saturate((int)(sum>>shift),-32768,32767,25-i);sum=0;}
            sum=wrap(i,sum+(long)m[row*3+1]*v[1]);sum=wrap(i,sum+(long)m[row*3+2]*v[2]);values[row]=sum;result(i,sum);
        }
        return values;
    }
    private void pushColor(){
        int rgb=data[6]&0xff000000;
        for(int i=1;i<=3;i++)rgb|=saturate(data[24+i]>>4,0,255,22-i)<<((i-1)*8);
        data[20]=data[21];data[21]=data[22];data[22]=rgb;
    }
    private void interpolate(long[] base){
        int factor=(short)data[8];int[] difference=new int[3];
        for(int i=1;i<=3;i++)difference[i-1]=saturate((int)(wrap(i,((long)control[20+i]<<12)-base[i-1])>>shift),-32768,32767,25-i);
        for(int i=1;i<=3;i++)result(i,base[i-1]+(long)difference[i-1]*factor);
    }
    private long[] modulated(){long[] out=new long[3];for(int i=0;i<3;i++)out[i]=(long)((data[6]>>>(i*8))&255)*(short)data[9+i]<<4;return out;}
    private void color(int v,boolean modulate,boolean depth){
        multiply(1,vector(v),3,false);multiply(2,vector(3),1,false);
        if(modulate){long[] base=modulated();if(depth)interpolate(base);else for(int i=1;i<=3;i++)result(i,base[i-1]);}
        pushColor();
    }
    private void perspective(int vector,boolean finalVertex){
        int previousIr3Flag=control[31]&(1<<22);
        long[] transformed=multiply(0,vector(vector),0,false);
        // IR3 flag always tests transformed Z shifted by 12, independently of sf/lm.
        control[31]=(control[31]&~(1<<22))|previousIr3Flag;saturate(transformed[2]>>12,-32768,32767,22);
        data[16]=data[17];data[17]=data[18];data[18]=data[19];data[19]=saturate(transformed[2]>>12,0,65535,18);
        int ratio=divide(control[26]&65535,data[19]);
        long projected=(long)ratio*(short)data[9];if(widescreen)projected=projected*3/4;
        long x=projected+control[24],y=(long)ratio*(short)data[10]+control[25];
        mac0(x);int sx=saturate(x>>16,-1024,1023,14);mac0(y);int sy=saturate(y>>16,-1024,1023,13);
        data[12]=data[13];data[13]=data[14];data[14]=(sx&65535)|(sy<<16);
        if(finalVertex){long depth=(long)ratio*(short)control[27]+control[28];mac0(depth);data[8]=saturate(depth>>12,0,4096,12);}
    }
    private int divide(int numerator,int denominator){
        if(numerator>=denominator*2){flag(17);return 0x1ffff;}
        int normalize=Integer.numberOfLeadingZeros(denominator)-16;
        long n=(long)numerator<<normalize,d=(long)denominator<<normalize;
        int index=(int)((d-0x7fc0)>>7),table=Math.max(0,((0x40000/(index+0x100)+1)/2)-0x101);
        long u=table+0x101;d=(0x2000080L-d*u)>>8;d=(0x80+d*u)>>8;
        return (int)Math.min(0x1ffff,(n*d+0x8000)>>16);
    }
    public void execute(int instruction){
        control[31]=0;shift=(instruction&0x80000)!=0?12:0;limit=(instruction&0x400)!=0;
        switch(instruction&63){
            case 1 -> perspective(0,true);
            case 0x30 -> {perspective(0,false);perspective(1,false);perspective(2,true);}
            case 6 -> {
                long x0=(short)data[12],y0=(short)(data[12]>>>16),x1=(short)data[13],y1=(short)(data[13]>>>16),x2=(short)data[14],y2=(short)(data[14]>>>16);
                mac0(x0*y1+x1*y2+x2*y0-x0*y2-x1*y0-x2*y1);
            }
            case 0x2d,0x2e -> {
                boolean four=(instruction&63)==0x2e;long sum=(data[17]&65535)+(data[18]&65535)+(data[19]&65535)+(four?(data[16]&65535):0);
                long value=sum*(short)control[four?30:29];mac0(value);data[7]=saturate(value>>12,0,65535,18);
            }
            case 0x12 -> multiply((instruction>>>17)&3,vector((instruction>>>15)&3),(instruction>>>13)&3,((instruction>>>13)&3)==2);
            case 0x28 -> {int[] v=vector(3);for(int i=1;i<=3;i++)result(i,(long)v[i-1]*v[i-1]);}
            case 0x0c -> {
                int a=(short)control[0],b=(short)control[2],c=(short)control[4];int[] v=vector(3);
                result(1,(long)v[2]*b-(long)v[1]*c);result(2,(long)v[0]*c-(long)v[2]*a);result(3,(long)v[1]*a-(long)v[0]*b);
            }
            case 0x3d,0x3e -> {
                int[] v=vector(3);int factor=(short)data[8];
                for(int i=1;i<=3;i++)result(i,(long)v[i-1]*factor+((instruction&63)==0x3e?(long)data[24+i]<<shift:0));pushColor();
            }
            case 0x10,0x11,0x29,0x2a -> {
                int op=instruction&63;
                for(int k=0;k<(op==0x2a?3:1);k++){
                    long[] base=new long[3];int rgb=op==0x2a?data[20]:data[6];
                    for(int i=0;i<3;i++)base[i]=op==0x11?(long)(short)data[9+i]<<12:op==0x29?(long)((rgb>>>(i*8))&255)*(short)data[9+i]<<4:(long)((rgb>>>(i*8))&255)<<16;
                    interpolate(base);pushColor();
                }
            }
            case 0x1e -> color(0,false,false);
            case 0x20 -> {for(int i=0;i<3;i++)color(i,false,false);}
            case 0x1b -> color(0,true,false);
            case 0x3f -> {for(int i=0;i<3;i++)color(i,true,false);}
            case 0x13 -> color(0,true,true);
            case 0x16 -> {for(int i=0;i<3;i++)color(i,true,true);}
            case 0x1c,0x14 -> {
                multiply(2,vector(3),1,false);long[] base=modulated();
                if((instruction&63)==0x14)interpolate(base);else for(int i=1;i<=3;i++)result(i,base[i-1]);pushColor();
            }
            default -> throw new UnsupportedOperationException(String.format("Undocumented GTE command %02X is not implemented",instruction&63));
        }
        summary();
    }
}
