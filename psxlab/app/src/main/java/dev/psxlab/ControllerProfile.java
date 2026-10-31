package dev.psxlab;
import android.content.SharedPreferences;
import android.view.*;
import java.util.*;

/** Persistent bindings are host input policy, separate from the emulated pad protocol. */
final class ControllerProfile {
    static final String[] BUTTONS={"Select","L3 / left stick click","R3 / right stick click","Start","D-pad up","D-pad right","D-pad down","D-pad left","L2","R2","L1","R1","Triangle","Circle","Cross","Square"};
    static final String[] HOTKEYS={"Pause / open menu","Save state","Load state","Next state slot","Previous state slot","Hold fast forward (muted)","Reset game","Screenshot","Cycle on-screen display","Toggle touch controls","Toggle analog mode","Exit game"};
    static final String[] AXES={"Left stick X","Left stick Y","Right stick X","Right stick Y"};
    private static final int[] DEFAULT_KEYS={109,106,107,108,19,22,20,21,104,105,102,103,100,97,96,99};
    private static final int[] DEFAULT_HOTKEYS={110,KeyEvent.KEYCODE_F5,KeyEvent.KEYCODE_F8,-1,-1,KeyEvent.KEYCODE_TAB,-1,KeyEvent.KEYCODE_F12,KeyEvent.KEYCODE_F10,-1,-1,-1};
    final String[] buttons=new String[16],hotkeys=new String[12];final int[] axes={-1,-1,-1,-1};final boolean[] invert=new boolean[4];
    final String[] directions=new String[8];
    final Map<Integer,Integer> keyMasks=new HashMap<>();final float deadzone,sensitivity,triggerThreshold;int customMask;
    ControllerProfile(SharedPreferences prefs,String descriptor,int port){
        for(int i=0;i<16;i++){String custom=stored(prefs,descriptor,i);buttons[i]=custom==null?"k:"+DEFAULT_KEYS[i]:custom;if(custom!=null)customMask|=1<<i;}
        Set<String> used=new HashSet<>();for(int i=0;i<16;i++)if((customMask&(1<<i))!=0)used.add(buttons[i]);
        for(int i=0;i<16;i++)if(buttons[i].startsWith("k:") && ((customMask&(1<<i))!=0 || !used.contains(buttons[i])))keyMasks.merge(Integer.parseInt(buttons[i].substring(2)),1<<i,(a,b)->a|b);
        if((customMask&(1<<14))==0)keyMasks.putIfAbsent(KeyEvent.KEYCODE_DPAD_CENTER,1<<14);
        for(int i=0;i<12;i++){String custom=stored(prefs,descriptor,100+i);hotkeys[i]=custom==null?(DEFAULT_HOTKEYS[i]<0?"none":"k:"+DEFAULT_HOTKEYS[i]):custom;}
        for(int i=0;i<4;i++){String custom=stored(prefs,descriptor,200+i);if("none".equals(custom))axes[i]=-2;else if(custom!=null && custom.startsWith("a:"))axes[i]=Integer.parseInt(custom.split(":")[1]);invert[i]=prefs.getBoolean("invert_"+port+"_"+i,false);}
        for(int i=0;i<8;i++){String value=stored(prefs,descriptor,210+i);directions[i]=value==null?"none":value;}
        deadzone=prefs.getInt("deadzone_"+port,8)/100f;sensitivity=prefs.getInt("sensitivity_"+port,100)/100f;triggerThreshold=prefs.getInt("trigger_"+port,50)/100f;
    }
    static String pref(String descriptor,int role){return "binding_"+descriptor+"_"+role;}
    private static String stored(SharedPreferences p,String descriptor,int role){if(p.getBoolean("automatic_"+descriptor,false))return null;String key=pref(descriptor,role);return p.contains(key)?p.getString(key,null):p.getString(pref("*",role),null);}
    static String label(String binding){if(binding==null)return "Automatic";if(binding.equals("none"))return "Unbound";String[] p=binding.split(":");return p[0].equals("k")?KeyEvent.keyCodeToString(Integer.parseInt(p[1])).replace("KEYCODE_",""):MotionEvent.axisToString(Integer.parseInt(p[1])).replace("AXIS_","")+(p.length>2 && p[2].equals("-1")?" −":" +");}
    static float axisValue(MotionEvent e,int axis){float value=e.getAxisValue(axis);InputDevice d=e.getDevice();InputDevice.MotionRange range=d==null?null:d.getMotionRange(axis,e.getSource());if(range!=null && (axis==MotionEvent.AXIS_LTRIGGER||axis==MotionEvent.AXIS_RTRIGGER||axis==MotionEvent.AXIS_BRAKE||axis==MotionEvent.AXIS_GAS) && range.getMin()<0)value=(value-range.getMin())/(range.getMax()-range.getMin());return value;}
    private boolean active(String bind,MotionEvent event){if(!bind.startsWith("a:"))return false;String[] a=bind.split(":");return axisValue(event,Integer.parseInt(a[1]))*Integer.parseInt(a[2])>triggerThreshold;}
    int buttonAxes(MotionEvent event){int mask=0;for(int i=0;i<16;i++)if(buttons[i].startsWith("a:") && !reservedAxis(Integer.parseInt(buttons[i].split(":")[1])) && active(buttons[i],event))mask|=1<<i;return mask;}
    int keyHotkeys(int code){int mask=0;for(int i=0;i<12;i++)if(hotkeys[i].equals("k:"+code))mask|=1<<i;return mask;}
    int axisHotkeys(MotionEvent event){int mask=0;for(int i=0;i<12;i++)if(active(hotkeys[i],event))mask|=1<<i;return mask;}
    boolean axisKey(int code){for(String b:directions)if(b.equals("k:"+code))return true;return false;}
    int directionAxes(MotionEvent event){int mask=0;for(int i=0;i<8;i++)if(directions[i].startsWith("a:") && !reservedAxis(Integer.parseInt(directions[i].split(":")[1])) && active(directions[i],event))mask|=1<<i;return mask;}
    float digitalAxis(Set<Integer> held,int motion,int axis,float natural){boolean negative=(motion&(1<<(axis*2)))!=0,positive=(motion&(1<<(axis*2+1)))!=0;for(int key:held){negative|=directions[axis*2].equals("k:"+key);positive|=directions[axis*2+1].equals("k:"+key);}return negative||positive?(negative==positive?0:negative?-1:1):natural;}
    boolean reservedAxis(int axis){for(String b:hotkeys)if(b.startsWith("a:") && Integer.parseInt(b.split(":")[1])==axis)return true;return false;}
    int automaticAxisMask(){int blocked=customMask;ArrayList<String> bindings=new ArrayList<>(Arrays.asList(buttons));bindings.addAll(Arrays.asList(hotkeys));for(String b:bindings)if(b.startsWith("a:")){int axis=Integer.parseInt(b.split(":")[1]);if(axis==MotionEvent.AXIS_HAT_X)blocked|=0xa0;if(axis==MotionEvent.AXIS_HAT_Y)blocked|=0x50;if(axis==MotionEvent.AXIS_LTRIGGER||axis==MotionEvent.AXIS_BRAKE)blocked|=0x100;if(axis==MotionEvent.AXIS_RTRIGGER||axis==MotionEvent.AXIS_GAS)blocked|=0x200;}return ~blocked;}
}
