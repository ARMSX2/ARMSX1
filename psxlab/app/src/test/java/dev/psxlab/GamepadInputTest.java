package dev.psxlab;
import dev.psxlab.core.PadState;
import android.view.KeyEvent;
import org.junit.Test;
import static org.junit.Assert.*;
public class GamepadInputTest {
    @Test public void faceButtonsMatchAndroidMappingAndRelease(){
        PadState state=new PadState();GamepadInput input=new GamepadInput(state);
        input.key(KeyEvent.KEYCODE_BUTTON_A,true);assertEquals(1<<14,state.get().pressed());input.key(KeyEvent.KEYCODE_BUTTON_B,true);input.key(KeyEvent.KEYCODE_BUTTON_X,true);input.key(KeyEvent.KEYCODE_BUTTON_Y,true);assertEquals(0xf000,state.get().pressed());input.key(KeyEvent.KEYCODE_BUTTON_A,false);assertEquals(0xb000,state.get().pressed());
    }
    @Test public void startSelectShouldersTriggersAndClicksAreMapped(){
        PadState state=new PadState();GamepadInput input=new GamepadInput(state);
        int[] keys={KeyEvent.KEYCODE_BUTTON_SELECT,KeyEvent.KEYCODE_BUTTON_THUMBL,KeyEvent.KEYCODE_BUTTON_THUMBR,KeyEvent.KEYCODE_BUTTON_START,KeyEvent.KEYCODE_BUTTON_L2,KeyEvent.KEYCODE_BUTTON_R2,KeyEvent.KEYCODE_BUTTON_L1,KeyEvent.KEYCODE_BUTTON_R1};for(int key:keys)input.key(key,true);assertEquals(0x0f0f,state.get().pressed());
    }
    @Test public void axesUseCorrectOrderAndCenteredRange(){
        PadState state=new PadState();GamepadInput input=new GamepadInput(state);input.axes(-1,1,0,0,-1,-1,1,0);assertEquals(0,state.get().leftX());assertEquals(255,state.get().leftY());assertEquals(128,state.get().rightX());assertEquals(0x190,state.get().pressed());
    }
    @Test public void duplicateKeyAndAxisReportsDoNotReleaseHeldButton(){
        PadState state=new PadState();GamepadInput input=new GamepadInput(state);input.key(KeyEvent.KEYCODE_DPAD_UP,true);input.key(KeyEvent.KEYCODE_BUTTON_L2,true);input.axes(0,0,0,0,0,-1,1,0);input.key(KeyEvent.KEYCODE_DPAD_UP,false);input.key(KeyEvent.KEYCODE_BUTTON_L2,false);assertEquals(0x110,state.get().pressed());input.axes(0,0,0,0,0,0,0,0);assertEquals(0,state.get().pressed());
    }
    @Test public void invalidAxesAndLifecycleClearLeaveNeutralPad(){
        PadState state=new PadState();GamepadInput input=new GamepadInput(state);input.axes(Float.NaN,Float.POSITIVE_INFINITY,2,-2,0,0,0,0);assertEquals(128,state.get().leftX());assertEquals(255,state.get().rightX());input.key(KeyEvent.KEYCODE_BUTTON_A,true);input.clear();assertEquals(0,state.get().pressed());assertEquals(128,state.get().rightY());
    }
    @Test public void androidBackAndVolumeKeysRemainAvailableToSystem(){
        GamepadInput input=new GamepadInput(new PadState());assertFalse(input.key(KeyEvent.KEYCODE_BACK,true));assertFalse(input.key(KeyEvent.KEYCODE_VOLUME_UP,true));
    }
}
