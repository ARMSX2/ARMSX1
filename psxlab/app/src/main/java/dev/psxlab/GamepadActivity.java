package dev.psxlab;
import android.app.Activity;
import android.os.*;
import android.view.*;
import android.hardware.input.InputManager;
import android.content.SharedPreferences;
import dev.psxlab.core.PadState;

/** Controller profiles, two input ports and remappable emulator actions. */
public abstract class GamepadActivity extends Activity implements InputManager.InputDeviceListener {
    protected final PadState pad=new PadState(),pad2=new PadState();
    protected final GamepadInput gamepad=new GamepadInput(pad),gamepad2=new GamepadInput(pad2);
    private InputManager inputs;private final Handler feedback=new Handler(Looper.getMainLooper());
    private final String[] descriptors={null,null};private final ControllerProfile[] profiles=new ControllerProfile[2];private final int[] axisHotkeys=new int[2];
    protected SharedPreferences inputPrefs;
    @Override public void onCreate(Bundle saved){super.onCreate(saved);inputPrefs=getSharedPreferences("Controllers",0);configurePads();inputs=(InputManager)getSystemService(INPUT_SERVICE);if(inputs!=null)inputs.registerInputDeviceListener(this,new Handler(Looper.getMainLooper()));}
    protected final void configurePads(){if(inputPrefs==null)return;PadState[] states={pad,pad2};for(int i=0;i<2;i++){states[i].controllerType=inputPrefs.getInt("type_"+i,i==0?0:3);states[i].startAnalog=inputPrefs.getBoolean("analog_"+i,false);states[i].forceAnalog=inputPrefs.getBoolean("forceAnalog_"+i,false);descriptors[i]=null;}gamepad.clear();gamepad2.clear();clearHotkeys();}
    private int port(InputDevice device){String second=inputPrefs.getString("device_1","");return device!=null && !second.isEmpty() && second.equals(device.getDescriptor())?1:0;}
    private ControllerProfile profile(InputDevice device,int port){String descriptor=device==null?"keyboard":device.getDescriptor();if(!descriptor.equals(descriptors[port])){profiles[port]=new ControllerProfile(inputPrefs,descriptor,port);descriptors[port]=descriptor;(port==0?gamepad:gamepad2).configure(profiles[port]);}return profiles[port];}
    @Override public boolean dispatchKeyEvent(KeyEvent event){
        if(event.getAction()==KeyEvent.ACTION_DOWN || event.getAction()==KeyEvent.ACTION_UP){int port=port(event.getDevice());ControllerProfile p=profile(event.getDevice(),port);int hotkeys=p.keyHotkeys(event.getKeyCode());if(hotkeys!=0&&!useMenuNavigation()){if(event.getRepeatCount()==0)for(int i=0;i<ControllerProfile.HOTKEYS.length;i++)if((hotkeys&(1<<i))!=0)onHotkey(i,event.getAction()==KeyEvent.ACTION_DOWN);return true;}
            String assigned=inputPrefs.getString("device_0","");if(port==0 && !assigned.isEmpty() && event.getDevice()!=null && !assigned.equals(event.getDevice().getDescriptor()))return useMenuNavigation()?dispatchMenuKeyEvent(event):super.dispatchKeyEvent(event);
            GamepadInput input=port==0?gamepad:gamepad2;if(input.key(event.getKeyCode(),event.getAction()==KeyEvent.ACTION_DOWN)){input.deviceId=event.getDeviceId();onPadChanged();return useMenuNavigation()?dispatchMenuKeyEvent(event):true;}}
        return useMenuNavigation()?dispatchMenuKeyEvent(event):super.dispatchKeyEvent(event);
    }
    @Override public boolean onGenericMotionEvent(MotionEvent event){if((event.getSource()&InputDevice.SOURCE_JOYSTICK)!=InputDevice.SOURCE_JOYSTICK && (event.getSource()&InputDevice.SOURCE_GAMEPAD)!=InputDevice.SOURCE_GAMEPAD)return super.onGenericMotionEvent(event);int port=port(event.getDevice());ControllerProfile p=profile(event.getDevice(),port);int current=p.axisHotkeys(event),changed=current^axisHotkeys[port];axisHotkeys[port]=current;for(int i=0;i<12;i++)if((changed&(1<<i))!=0)onHotkey(i,(current&(1<<i))!=0);String assigned=inputPrefs.getString("device_0","");if(port==0 && !assigned.isEmpty() && event.getDevice()!=null && !assigned.equals(event.getDevice().getDescriptor()))return super.onGenericMotionEvent(event);if((port==0?gamepad:gamepad2).motion(event)){onPadChanged();return useMenuNavigation()?super.onGenericMotionEvent(event):true;}return super.onGenericMotionEvent(event);}
    protected boolean useMenuNavigation(){return false;}
    protected boolean dispatchMenuKeyEvent(KeyEvent event){return super.dispatchKeyEvent(event);}
    protected void onHotkey(int action,boolean down){}
    private void clearHotkeys(){axisHotkeys[0]=axisHotkeys[1]=0;onHotkey(5,false);}
    private final Runnable rumble=new Runnable(){public void run(){if(hasWindowFocus()){applyRumble(pad,gamepad,0);applyRumble(pad2,gamepad2,1);}feedback.postDelayed(this,50);}};
    private void applyRumble(PadState state,GamepadInput input,int port){if(!inputPrefs.getBoolean("rumble_"+port,false))return;InputDevice device=InputDevice.getDevice(input.deviceId);if(device==null)return;Vibrator vibrator=device.getVibrator();if(!vibrator.hasVibrator())return;int amount=Math.round(Math.max(state.largeMotor,state.smallMotor!=0?100:0)*inputPrefs.getInt("rumbleStrength_"+port,100)/100f);if(amount>0)vibrator.vibrate(VibrationEffect.createOneShot(80,Math.min(255,amount)));else vibrator.cancel();}
    private void stopInput(){for(GamepadInput input:new GamepadInput[]{gamepad,gamepad2}){InputDevice d=InputDevice.getDevice(input.deviceId);if(d!=null && d.getVibrator().hasVibrator())d.getVibrator().cancel();input.clear();}clearHotkeys();onPadChanged();}
    @Override protected void onResume(){super.onResume();configurePads();feedback.removeCallbacks(rumble);feedback.post(rumble);}
    @Override protected void onPause(){feedback.removeCallbacks(rumble);stopInput();super.onPause();}
    @Override public void onWindowFocusChanged(boolean focused){super.onWindowFocusChanged(focused);if(!focused)stopInput();}
    @Override protected void onDestroy(){feedback.removeCallbacksAndMessages(null);if(inputs!=null)inputs.unregisterInputDeviceListener(this);super.onDestroy();}
    @Override public void onInputDeviceAdded(int id){onPadChanged();}
    @Override public void onInputDeviceChanged(int id){if(id==gamepad.deviceId||id==gamepad2.deviceId)stopInput();}
    @Override public void onInputDeviceRemoved(int id){if(id==gamepad.deviceId||id==gamepad2.deviceId)stopInput();}
    protected void onPadChanged(){}
}
