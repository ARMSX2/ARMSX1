package dev.psxlab.core;
import java.util.concurrent.atomic.*;
import java.util.*;

/** Immutable input snapshots cross from Android's UI thread to the emulation thread. */
public final class PadState {
    public record State(int pressed,int rightX,int rightY,int leftX,int leftY) {
        public int buttonWord(boolean analog){
            int buttons=pressed;
            if(!analog){buttons&=~6;if(leftX<77)buttons|=128;if(leftX>178)buttons|=32;if(leftY<77)buttons|=16;if(leftY>178)buttons|=64;}
            return ~buttons&65535;
        }
    }
    private final AtomicReference<State> state=new AtomicReference<>(new State(0,128,128,128,128));
    private final AtomicInteger toggles=new AtomicInteger();
    public volatile boolean analogMode;
    /** 0 DualShock, 1 digital pad, 2 analog joystick, 3 disconnected. */
    public volatile int controllerType;
    public volatile boolean startAnalog,forceAnalog;
    public volatile int smallMotor,largeMotor;
    public State get(){return state.get();}
    public void update(int pressed,int rightX,int rightY,int leftX,int leftY){state.set(new State(pressed&65535,clamp(rightX),clamp(rightY),clamp(leftX),clamp(leftY)));}
    private int clamp(int n){return Math.max(0,Math.min(255,n));}
    public void clear(){update(0,128,128,128,128);smallMotor=largeMotor=0;}
    public void toggleAnalog(){toggles.incrementAndGet();}
    public int toggleGeneration(){return toggles.get();}
    public String describe(){
        State s=get();String[] names={"Select","L3","R3","Start","Up","Right","Down","Left","L2","R2","L1","R1","Triangle","Circle","Cross","Square"};
        ArrayList<String> pressed=new ArrayList<>();for(int i=0;i<16;i++)if((s.pressed&(1<<i))!=0)pressed.add(names[i]);
        return "Buttons: "+(pressed.isEmpty()?"none":String.join(" + ",pressed))+String.format(Locale.ROOT,"\nLeft stick %d, %d · Right stick %d, %d",s.leftX,s.leftY,s.rightX,s.rightY);
    }
}
