package dev.psxlab;
import android.os.*;
import android.app.*;
import android.view.*;
import android.widget.*;
import java.util.*;

/** Complete bindings for the PS1 controls and the emulator's supported hotkeys. */
abstract class ControllerOptionsActivity extends GamepadActivity {
    private final MenuNavigation menuNavigation=new MenuNavigation(this);
    @Override protected boolean useMenuNavigation(){return role<0;}
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event){if(event.isFromSource(InputDevice.SOURCE_JOYSTICK))return onGenericMotionEvent(event);return super.dispatchGenericMotionEvent(event);}
    @Override protected boolean dispatchMenuKeyEvent(KeyEvent event){return menuNavigation.key(event)||super.dispatchMenuKeyEvent(event);}
    @Override protected void onPause(){menuNavigation.stop();super.onPause();}
    @Override public void onWindowFocusChanged(boolean focused){super.onWindowFocusChanged(focused);if(!focused)menuNavigation.stop();}
    private TextView values;private String descriptor="*";private int port,role=-1;private TextView captureHint;private Button cancelCapture;private ScrollView optionsScroll;
    @Override public void onCreate(Bundle state){super.onCreate(state);if(getIntent().getBooleanExtra("probeInput",false)){inputPrefs=getSharedPreferences("ControllerProbe",0);if(getIntent().getBooleanExtra("clearProbeInput",false))inputPrefs.edit().clear().commit();configurePads();}if(state!=null){descriptor=state.getString("descriptor","*");port=state.getInt("port",0);}}
    @Override protected void onSaveInstanceState(Bundle out){super.onSaveInstanceState(out);out.putString("descriptor",descriptor);out.putInt("port",port);}
    protected abstract void rebuildControllerOptions();
    protected final boolean recordingControllerBinding(){return role>=0;}
    protected final void detachControllerOptions(){role=-1;values=null;captureHint=null;cancelCapture=null;optionsScroll=null;}
    protected final void renderControllerOptions(LinearLayout p,ScrollView hostScroll){int scroll=optionsScroll==null?0:optionsScroll.getScrollY();optionsScroll=hostScroll;optionsScroll.setFocusable(false);
        Ui.choice(this,p,"Controller port",new String[]{"Port 1","Port 2"},port,which->{port=which;rebuildControllerOptions();});
        ArrayList<String> ids=new ArrayList<>(Arrays.asList("*"));ArrayList<String> names=new ArrayList<>(Arrays.asList("All controllers — default profile"));
        for(int id:InputDevice.getDeviceIds()){InputDevice d=InputDevice.getDevice(id);if(d!=null && (d.supportsSource(InputDevice.SOURCE_GAMEPAD)||d.supportsSource(InputDevice.SOURCE_JOYSTICK)||d.supportsSource(InputDevice.SOURCE_KEYBOARD))){ids.add(d.getDescriptor());names.add(d.getName());}}
        if(!ids.contains(descriptor)){ids.add(descriptor);names.add("Saved controller profile");}
        Ui.choice(this,p,"Mapping profile",names.toArray(new String[0]),ids.indexOf(descriptor),which->{descriptor=ids.get(which);rebuildControllerOptions();});
        ArrayList<String> assignmentIds=new ArrayList<>(Arrays.asList(""));ArrayList<String> assignmentNames=new ArrayList<>(Arrays.asList(port==0?"Automatic — unassigned controllers":"No device assigned"));for(int i=1;i<ids.size();i++){assignmentIds.add(ids.get(i));assignmentNames.add(names.get(i));}
        int assigned=Math.max(0,assignmentIds.indexOf(inputPrefs.getString("device_"+port,"")));Ui.choice(this,p,"Input device for this port",assignmentNames.toArray(new String[0]),assigned,which->{inputPrefs.edit().putString("device_"+port,assignmentIds.get(which)).apply();configurePads();});
        Ui.choice(this,p,"Emulated controller",new String[]{"DualShock","Digital controller","Analog joystick","Disconnected"},inputPrefs.getInt("type_"+port,port==0?0:3),which->{inputPrefs.edit().putInt("type_"+port,which).apply();configurePads();});
        Ui.toggle(this,p,"Start in analog mode","DualShock starts with both analog sticks enabled. Applies to the next game.",inputPrefs.getBoolean("analog_"+port,false),on->inputPrefs.edit().putBoolean("analog_"+port,on).apply());
        Ui.toggle(this,p,"Force analog mode","Keep DualShock analog mode enabled even if a game requests digital mode.",inputPrefs.getBoolean("forceAnalog_"+port,false),on->inputPrefs.edit().putBoolean("forceAnalog_"+port,on).apply());
        slider(p,"Stick dead zone","deadzone_"+port,0,50,8,"%");slider(p,"Stick sensitivity","sensitivity_"+port,50,200,100,"%");slider(p,"Trigger / axis button threshold","trigger_"+port,10,90,50,"%");
        for(int i=0;i<4;i++){final int axis=i;Ui.toggle(this,p,"Invert "+ControllerProfile.AXES[i],"Reverse this stick axis.",inputPrefs.getBoolean("invert_"+port+"_"+i,false),on->inputPrefs.edit().putBoolean("invert_"+port+"_"+axis,on).apply());}
        Ui.toggle(this,p,"Controller rumble","Send DualShock vibration to the assigned controller when supported.",inputPrefs.getBoolean("rumble_"+port,false),on->inputPrefs.edit().putBoolean("rumble_"+port,on).apply());slider(p,"Rumble strength","rumbleStrength_"+port,0,100,100,"%");
        Button auto=Ui.button(this,"Use automatic Android mapping",true);p.addView(auto);auto.setOnClickListener(v->{android.content.SharedPreferences.Editor edit=inputPrefs.edit();String prefix="binding_"+descriptor+"_";for(String key:inputPrefs.getAll().keySet())if(key.startsWith(prefix))edit.remove(key);edit.putBoolean("automatic_"+descriptor,true).apply();configurePads();rebuildControllerOptions();});Ui.gap(p,16);
        p.addView(Ui.heading(this,"Buttons",18));Ui.gap(p,8);for(int i=0;i<16;i++)binding(p,ControllerProfile.BUTTONS[i],i);
        Ui.gap(p,8);p.addView(Ui.heading(this,"Analog axes",18));Ui.gap(p,8);for(int i=0;i<4;i++)binding(p,ControllerProfile.AXES[i],200+i);
        Ui.gap(p,8);p.addView(Ui.heading(this,"Analog directions from buttons",18));Ui.gap(p,8);for(int i=0;i<8;i++)binding(p,ControllerProfile.AXES[i/2]+(i%2==0?" negative":" positive"),210+i);
        Ui.gap(p,8);p.addView(Ui.heading(this,"Emulator hotkeys",18));Ui.gap(p,8);for(int i=0;i<ControllerProfile.HOTKEYS.length;i++)binding(p,ControllerProfile.HOTKEYS[i],100+i);
        Ui.gap(p,12);p.addView(Ui.heading(this,"Live input test",18));values=Ui.text(this,"",14,Ui.TEXT);p.addView(values);onPadChanged();optionsScroll.post(()->optionsScroll.scrollTo(0,scroll));
    }
    private void slider(LinearLayout p,String title,String key,int minimum,int maximum,int fallback,String unit){
        int[] presets=key.startsWith("deadzone")?new int[]{0,5,8,10,15,20,25,30,40,50}:key.startsWith("sensitivity")?new int[]{50,75,100,125,150,175,200}:key.startsWith("trigger")?new int[]{10,25,40,50,60,75,90}:new int[]{0,25,50,75,100};
        ArrayList<Integer> choices=new ArrayList<>();for(int n:presets)if(n>=minimum&&n<=maximum)choices.add(n);int value=Math.max(minimum,Math.min(maximum,inputPrefs.getInt(key,fallback)));if(!choices.contains(value)){choices.add(value);Collections.sort(choices);}String[] labels=new String[choices.size()];for(int i=0;i<labels.length;i++)labels[i]=choices.get(i)+unit;Ui.choice(this,p,title,labels,choices.indexOf(value),which->{inputPrefs.edit().putInt(key,choices.get(which)).apply();configurePads();});
    }
    private void binding(LinearLayout p,String title,int target){
        String value=inputPrefs.getString(ControllerProfile.pref(descriptor,target),null);ArrayList<String> choices=new ArrayList<>(Arrays.asList("Automatic","Unbound",target>=200&&target<204?"Record stick axis…":"Record button / axis…"));int selected=value==null?0:value.equals("none")?1:3;if(selected==3)choices.add(ControllerProfile.label(value));
        TextView hint=Ui.text(this,"",12,Ui.ACCENT);Button cancel=Ui.button(this,"Cancel recording",false);hint.setVisibility(View.GONE);cancel.setVisibility(View.GONE);cancel.setOnClickListener(v->{role=-1;rebuildControllerOptions();});
        Ui.choice(this,p,title,choices.toArray(new String[0]),selected,which->{if(which==0){inputPrefs.edit().remove(ControllerProfile.pref(descriptor,target)).putBoolean("automatic_"+descriptor,false).apply();role=-1;configurePads();rebuildControllerOptions();}else if(which==1){role=target;saveBinding("none");}else if(which==2){if(captureHint!=null)captureHint.setVisibility(View.GONE);if(cancelCapture!=null)cancelCapture.setVisibility(View.GONE);role=target;captureHint=hint;cancelCapture=cancel;hint.setText(target>=200&&target<204?"Move the stick axis to record it.":"Press a controller/keyboard button or move a stick/trigger to record it.");hint.setVisibility(View.VISIBLE);cancel.setVisibility(View.VISIBLE);}});p.addView(hint);p.addView(cancel);Ui.gap(p,8);
    }
    private void saveBinding(String value){if(role<0)return;inputPrefs.edit().putString(ControllerProfile.pref(descriptor,role),value).putBoolean("automatic_"+descriptor,false).apply();role=-1;configurePads();rebuildControllerOptions();}
    @Override public boolean dispatchKeyEvent(KeyEvent e){if(role>=0 && e.getAction()==KeyEvent.ACTION_DOWN && e.getRepeatCount()==0){if(e.getKeyCode()==KeyEvent.KEYCODE_BACK){role=-1;rebuildControllerOptions();return true;}if(role<200||role>=204)saveBinding("k:"+e.getKeyCode());return true;}return super.dispatchKeyEvent(e);}
    @Override public boolean onGenericMotionEvent(MotionEvent e){if(role>=0 && e.getAction()==MotionEvent.ACTION_MOVE){InputDevice device=e.getDevice();if(device!=null)for(InputDevice.MotionRange range:device.getMotionRanges()){int axis=range.getAxis();if(axis==MotionEvent.AXIS_GENERIC_1||axis==MotionEvent.AXIS_PRESSURE||axis==MotionEvent.AXIS_SIZE)continue;float value=ControllerProfile.axisValue(e,axis);if(Math.abs(value)>.65f){if(role>=200&&role<204 && (range.getMin()>=0 || axis==MotionEvent.AXIS_HAT_X||axis==MotionEvent.AXIS_HAT_Y))continue;saveBinding("a:"+axis+":"+(role>=200&&role<204?1:value<0?-1:1));break;}}return true;}return super.onGenericMotionEvent(e)||menuNavigation.motion(e);}
    @Override protected void onPadChanged(){if(values!=null)values.setText("Port 1\n"+pad.describe()+"\n\nPort 2\n"+pad2.describe());}
}
