package dev.psxlab;
import android.view.*;
import dev.psxlab.core.PadState;

/** Android gamepad events to a port-one controller snapshot. */
public final class GamepadInput {
    private final PadState state;
    private int keys,hats,triggers,touch;
    private float lx,ly,rx,ry;
    private ControllerProfile profile;
    private int axisButtons,axisDirections;
    private final java.util.Set<Integer> held=new java.util.HashSet<>();
    void configure(ControllerProfile value){clear();profile=value;}
    public int deviceId=-1;
    public GamepadInput(PadState state){this.state=state;}
    private static int defaultBit(int code){return switch(code){
            case KeyEvent.KEYCODE_BUTTON_SELECT -> 0;
            case KeyEvent.KEYCODE_BUTTON_THUMBL -> 1;
            case KeyEvent.KEYCODE_BUTTON_THUMBR -> 2;
            case KeyEvent.KEYCODE_BUTTON_START -> 3;
            case KeyEvent.KEYCODE_DPAD_UP -> 4;
            case KeyEvent.KEYCODE_DPAD_RIGHT -> 5;
            case KeyEvent.KEYCODE_DPAD_DOWN -> 6;
            case KeyEvent.KEYCODE_DPAD_LEFT -> 7;
            case KeyEvent.KEYCODE_BUTTON_L2 -> 8;
            case KeyEvent.KEYCODE_BUTTON_R2 -> 9;
            case KeyEvent.KEYCODE_BUTTON_L1 -> 10;
            case KeyEvent.KEYCODE_BUTTON_R1 -> 11;
            case KeyEvent.KEYCODE_BUTTON_Y -> 12;
            case KeyEvent.KEYCODE_BUTTON_B -> 13;
            case KeyEvent.KEYCODE_BUTTON_A,KeyEvent.KEYCODE_DPAD_CENTER -> 14;
            case KeyEvent.KEYCODE_BUTTON_X -> 15;
            default -> -1;
        };
    }
    public boolean key(int code,boolean down){int bit=defaultBit(code);
        int mask=profile==null?(bit<0?0:1<<bit):profile.keyMasks.getOrDefault(code,0);if(mask==0 && (profile==null || !profile.axisKey(code)))return false;
        if(down)held.add(code);else held.remove(code);keys=0;for(int key:held){if(profile!=null)keys|=profile.keyMasks.getOrDefault(key,0);else{int b=defaultBit(key);if(b>=0)keys|=1<<b;}}publish();return true;
    }
    public void axes(float leftX,float leftY,float rightX,float rightY,float hatX,float hatY,float leftTrigger,float rightTrigger){
        lx=adjust(leftX,0);ly=adjust(leftY,1);rx=adjust(rightX,2);ry=adjust(rightY,3);hats=0;triggers=0;
        if(hatX< -0.5f)hats|=128;if(hatX>0.5f)hats|=32;if(hatY< -0.5f)hats|=16;if(hatY>0.5f)hats|=64;
        float threshold=profile==null?.5f:profile.triggerThreshold;if(leftTrigger>threshold)triggers|=256;if(rightTrigger>threshold)triggers|=512;if(profile!=null){hats&=profile.automaticAxisMask();triggers&=profile.automaticAxisMask();}publish();
    }
    private float adjust(float v,int axis){v=finite(v);if(profile==null)return v;if(Math.abs(v)<=profile.deadzone)return 0;v*=profile.sensitivity;return finite(profile.invert[axis]?-v:v);}
    private float finite(float value){return Float.isFinite(value)?Math.max(-1,Math.min(1,value)):0;}
    private int axisByte(float value){return Math.round((value+1)*127.5f);}
    private void publish(){float x=lx,y=ly,r=rx,s=ry;if(profile!=null){x=profile.digitalAxis(held,axisDirections,0,x);y=profile.digitalAxis(held,axisDirections,1,y);r=profile.digitalAxis(held,axisDirections,2,r);s=profile.digitalAxis(held,axisDirections,3,s);}state.update(keys|hats|triggers|touch|axisButtons,axisByte(r),axisByte(s),axisByte(x),axisByte(y));}
    public void touch(int bit,boolean down){if(down)touch|=1<<bit;else touch&=~(1<<bit);publish();}
    public void clear(){held.clear();keys=hats=triggers=touch=axisButtons=axisDirections=0;lx=ly=rx=ry=0;deviceId=-1;state.clear();}
    public boolean motion(MotionEvent event){
        if(event.getAction()!=MotionEvent.ACTION_MOVE || (event.getSource()&InputDevice.SOURCE_JOYSTICK)!=InputDevice.SOURCE_JOYSTICK && (event.getSource()&InputDevice.SOURCE_GAMEPAD)!=InputDevice.SOURCE_GAMEPAD)return false;
        InputDevice device=event.getDevice();deviceId=event.getDeviceId();
        int rightX=device!=null && device.getMotionRange(MotionEvent.AXIS_RX)!=null?MotionEvent.AXIS_RX:MotionEvent.AXIS_Z;
        int rightY=device!=null && device.getMotionRange(MotionEvent.AXIS_RY)!=null?MotionEvent.AXIS_RY:MotionEvent.AXIS_RZ;
        int[] axes={MotionEvent.AXIS_X,MotionEvent.AXIS_Y,rightX,rightY};if(profile!=null){for(int i=0;i<4;i++)if(profile.axes[i]>=0)axes[i]=profile.axes[i];axisButtons=profile.buttonAxes(event);axisDirections=profile.directionAxes(event);}
        axes(centered(event,device,axes[0]),centered(event,device,axes[1]),centered(event,device,axes[2]),centered(event,device,axes[3]),event.getAxisValue(MotionEvent.AXIS_HAT_X),event.getAxisValue(MotionEvent.AXIS_HAT_Y),Math.max(trigger(event,device,MotionEvent.AXIS_LTRIGGER),trigger(event,device,MotionEvent.AXIS_BRAKE)),Math.max(trigger(event,device,MotionEvent.AXIS_RTRIGGER),trigger(event,device,MotionEvent.AXIS_GAS)));
        return true;
    }
    private float centered(MotionEvent event,InputDevice device,int axis){
        if(axis==-2 || profile!=null && profile.reservedAxis(axis))return 0;
        InputDevice.MotionRange range=device==null?null:device.getMotionRange(axis,event.getSource());float value=event.getAxisValue(axis),flat=range==null?0.08f:Math.max(0.08f,range.getFlat());return Math.abs(value)<=flat?0:value;
    }
    private float trigger(MotionEvent event,InputDevice device,int axis){
        InputDevice.MotionRange range=device==null?null:device.getMotionRange(axis,event.getSource());if(range==null)return 0;
        float value=event.getAxisValue(axis);return range.getMin()<0?(value-range.getMin())/(range.getMax()-range.getMin()):value;
    }
}
