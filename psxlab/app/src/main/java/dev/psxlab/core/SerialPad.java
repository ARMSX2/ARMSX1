package dev.psxlab.core;

/** DualShock-compatible byte protocol, initially digital. No multitap or physical rumble output. */
final class SerialPad {
    record Reply(int value,boolean acknowledge){}
    private final PadState state;
    private boolean selected,finished,config,analog,locked;
    private int command,index,lastToggle;
    private int[] response;
    private final int[] rumbleMap={255,255,255,255,255,255};
    private int smallMotor,largeMotor;
    private int type;
    SerialPad(PadState state){this.state=state;type=state.controllerType;analog=type==2 || type==0 && (state.startAnalog||state.forceAnalog);state.analogMode=analog;}
    void deselect(){selected=finished=false;response=null;index=0;}
    void restored(){lastToggle=state.toggleGeneration();state.analogMode=analog;state.smallMotor=state.largeMotor=0;}
    Reply exchange(int value){
        value&=255;
        if(type==3)return new Reply(255,false);
        if(finished)return new Reply(255,false);
        if(!selected){
            if(value!=1){finished=true;return new Reply(255,false);}
            int toggle=state.toggleGeneration();if(((toggle-lastToggle)&1)!=0 && !locked && !state.forceAnalog && type==0)analog=!analog;lastToggle=toggle;state.analogMode=analog;
            selected=true;return new Reply(255,true);
        }
        if(response==null){
            command=value;PadState.State snapshot=state.get();
            if(type!=0 && command!=0x42){finished=true;return new Reply(255,false);}
            if(command!=0x42 && command!=0x43 && !config){finished=true;return new Reply(255,false);}
            int id=config?0xf3:type==2?0x53:analog?0x73:0x41;
            if(command==0x42 || command==0x43 && !config){
                int buttons=snapshot.buttonWord(analog||config);response=config || analog?new int[]{id,0x5a,buttons&255,buttons>>>8,snapshot.rightX(),snapshot.rightY(),snapshot.leftX(),snapshot.leftY()}:new int[]{id,0x5a,buttons&255,buttons>>>8};
            }else{
                response=new int[]{0xf3,0x5a,0,0,0,0,0,0};
                switch(command){
                    case 0x43,0x44,0x46,0x4c,0x48 -> {}
                    case 0x45 -> {response[2]=1;response[3]=2;response[4]=analog?1:0;response[5]=2;response[6]=1;}
                    case 0x47 -> {response[4]=2;response[6]=1;}
                    case 0x4d -> System.arraycopy(rumbleMap,0,response,2,6);
                    default -> {finished=true;return new Reply(255,false);}
                }
            }
            index=1;return new Reply(response[0],true);
        }
        // index 1 is the dummy byte/5Ah sync; index 2..7 are command parameters.
        if(index>=2){
            int parameter=index-2;
            if(command==0x43 && parameter==0)config=value==1;
            if(command==0x44){if(parameter==0 && value<=1 && !state.forceAnalog){analog=value==1;state.analogMode=analog;}if(parameter==1)locked=(value&3)==3;}
            if(command==0x46 && parameter==0){int[] tail=value==0?new int[]{1,2,0,10}:value==1?new int[]{1,1,1,20}:new int[4];System.arraycopy(tail,0,response,4,4);}
            if(command==0x4c && parameter==0)response[5]=value==0?4:value==1?7:0;
            if(command==0x48 && parameter==0)response[6]=value<=1?1:0;
            if(command==0x4d)rumbleMap[parameter]=value;
            if(command==0x42){if(rumbleMap[parameter]==0)smallMotor=value&1;if(rumbleMap[parameter]==1)largeMotor=value;state.smallMotor=smallMotor;state.largeMotor=largeMotor;}
        }
        int result=response[index++];boolean ack=index<response.length;if(!ack)finished=true;
        return new Reply(result,ack);
    }
}
