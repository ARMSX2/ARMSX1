package dev.psxlab.core;
/** Original native GTE arithmetic; CPU register transfers use the Java core register array. */
final class NativeGte {
    private final int[] state=new int[64];
    boolean widescreen;
    int[] rawState(){return state;}
    void reset(){for(int i=0;i<64;i++)state[i]=0;state[31]=32;}
    private static int clamp(int v){return Math.max(0,Math.min(31,v));}
    int readData(int reg){return switch(reg){
        case 1,3,5,8,9,10,11 -> (short)state[reg];
        case 7,16,17,18,19 -> state[reg]&65535;
        case 15 -> state[14];
        case 28,29 -> clamp((short)state[9]>>7)|(clamp((short)state[10]>>7)<<5)|(clamp((short)state[11]>>7)<<10);
        default -> state[reg];
    };}
    void writeData(int reg,int value){switch(reg){
        case 1,3,5,8,9,10,11 -> state[reg]=(short)value;
        case 7,16,17,18,19 -> state[reg]=value&65535;
        case 15 -> {state[12]=state[13];state[13]=state[14];state[14]=value;}
        case 28 -> {state[9]=(value&31)<<7;state[10]=((value>>>5)&31)<<7;state[11]=((value>>>10)&31)<<7;}
        case 29,31 -> {}
        case 30 -> {state[30]=value;state[31]=Integer.numberOfLeadingZeros(value<0?~value:value);}
        default -> state[reg]=value;
    }}
    int readControl(int reg){return state[32+reg];}
    void writeControl(int reg,int value){switch(reg){
        case 4,12,20,26,27,29,30 -> state[32+reg]=(short)value;
        case 31 -> {value&=0x7ffff000;if((value&0x7f87e000)!=0)value|=0x80000000;state[63]=value;}
        default -> state[32+reg]=value;
    }}
    void execute(int instruction){executeNative(state,instruction,widescreen);}
    private static native void executeNative(int[] state,int instruction,boolean widescreen);
}
