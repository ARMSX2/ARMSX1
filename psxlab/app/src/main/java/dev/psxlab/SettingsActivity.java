package dev.psxlab;
import android.app.*;
import android.os.Bundle;
import android.content.*;
import android.view.*;
import android.widget.*;

public final class SettingsActivity extends ControllerOptionsActivity {
    private static final String[] TABS={"Display","Video","Controllers","Customisation","BIOS","About"};
    private static final int CONTROLLERS_TAB=2;
    private final Button[] tabs=new Button[TABS.length];
    private SharedPreferences prefs;
    private LinearLayout content;
    private ScrollView scroll;
    private int selectedTab;

    @Override public void onCreate(Bundle state){
        super.onCreate(state);Ui.chrome(this);prefs=getSharedPreferences("MainActivity",0);
        LinearLayout root=new LinearLayout(this);root.setOrientation(1);root.setBackgroundColor(Ui.BG);root.setPadding(Ui.dp(this,24),Ui.dp(this,12),Ui.dp(this,24),Ui.dp(this,16));
        LinearLayout header=new LinearLayout(this);header.setGravity(Gravity.CENTER_VERTICAL);
        Button back=Ui.button(this,"‹",false);back.setContentDescription("Back");back.setOnClickListener(v->finish());header.addView(back,new LinearLayout.LayoutParams(Ui.dp(this,48),Ui.dp(this,42)));
        TextView title=Ui.heading(this,"Settings",24);title.setPadding(Ui.dp(this,16),0,0,0);header.addView(title);root.addView(header);Ui.gap(root,16);
        HorizontalScrollView tabScroll=new HorizontalScrollView(this);tabScroll.setFocusable(false);tabScroll.setHorizontalScrollBarEnabled(false);LinearLayout bar=new LinearLayout(this);bar.setGravity(Gravity.CENTER_VERTICAL);
        for(int i=0;i<TABS.length;i++){final int at=i;Button tab=Ui.outlineButton(this,TABS[i],false);tab.setContentDescription(TABS[i]+" settings tab");tab.setAccessibilityDelegate(new View.AccessibilityDelegate(){@Override public void onInitializeAccessibilityNodeInfo(View host,android.view.accessibility.AccessibilityNodeInfo info){super.onInitializeAccessibilityNodeInfo(host,info);info.setCollectionItemInfo(android.view.accessibility.AccessibilityNodeInfo.CollectionItemInfo.obtain(0,1,at,1,false,selectedTab==at));}});tabs[i]=tab;LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-2,Ui.dp(this,44));if(i>0)lp.leftMargin=Ui.dp(this,8);bar.addView(tab,lp);tab.setOnClickListener(v->selectTab(at));}
        for(Button tab:tabs)tab.setId(View.generateViewId());
        tabScroll.addView(bar);root.addView(tabScroll,new LinearLayout.LayoutParams(-1,-2));Ui.gap(root,16);
        scroll=new ScrollView(this);scroll.setFocusable(false);scroll.setFillViewport(true);content=new LinearLayout(this);content.setOrientation(1);content.setPadding(0,Ui.dp(this,4),0,0);scroll.addView(content);root.addView(scroll,new LinearLayout.LayoutParams(-1,0,1));setContentView(root);
        selectTab(state==null?0:Math.max(0,Math.min(TABS.length-1,state.getInt("settingsTab",0))));
        tabs[selectedTab].post(()->{tabs[selectedTab].requestFocusFromTouch();tabs[selectedTab].requestRectangleOnScreen(new android.graphics.Rect(0,0,tabs[selectedTab].getWidth(),tabs[selectedTab].getHeight()),true);});
    }

    @Override protected void onSaveInstanceState(Bundle state){state.putInt("settingsTab",selectedTab);super.onSaveInstanceState(state);}
    @Override public boolean dispatchKeyEvent(KeyEvent event){if(recordingControllerBinding())return super.dispatchKeyEvent(event);int code=event.getKeyCode();if(code==KeyEvent.KEYCODE_BUTTON_L1||code==KeyEvent.KEYCODE_BUTTON_R1){if(event.getAction()==KeyEvent.ACTION_DOWN&&event.getRepeatCount()==0){selectTab((selectedTab+(code==KeyEvent.KEYCODE_BUTTON_R1?1:TABS.length-1))%TABS.length);tabs[selectedTab].requestFocusFromTouch();tabs[selectedTab].requestRectangleOnScreen(new android.graphics.Rect(0,0,tabs[selectedTab].getWidth(),tabs[selectedTab].getHeight()),true);}return true;}return super.dispatchKeyEvent(event);}

    private void selectTab(int at){
        if(selectedTab==CONTROLLERS_TAB&&at!=CONTROLLERS_TAB)detachControllerOptions();
        selectedTab=at;for(int i=0;i<tabs.length;i++)Ui.selectOutline(tabs[i],i==at);content.removeAllViews();scroll.scrollTo(0,0);
        switch(at){
            case 0:
                Spinner output=Ui.choice(this,content,"Display resolution",DisplayConfig.RESOLUTIONS,DisplayConfig.mode(prefs),which->prefs.edit().putInt("displayResolution",DisplayConfig.preferenceMode(which)).apply());
                Ui.choice(this,content,"Internal resolution",DisplayConfig.INTERNAL_RESOLUTIONS,DisplayConfig.internalMode(prefs),which->{android.content.SharedPreferences.Editor edit=prefs.edit().putInt("internalScale",DisplayConfig.scaleForInternalMode(which));if(which>0)edit.putInt("displayResolution",DisplayConfig.preferenceMode(DisplayConfig.FULL_HD_OUTPUT));edit.apply();if(which>0)output.setSelection(DisplayConfig.FULL_HD_OUTPUT);});
                help("Native keeps original game detail. 720p HD and 1080p Full HD both select 1080p screen output. Internal resolution labels are approximate and vary by game. HD uses GPU acceleration. Applies to the next game.");
                Ui.toggle(this,content,"Smooth display scaling","Smooth the game picture when scaling to the selected output resolution.",prefs.getBoolean("filter",true),on->prefs.edit().putBoolean("filter",on).apply());break;
            case 1:
                Ui.choice(this,content,"Renderer",DisplayConfig.RENDERERS,DisplayConfig.renderer(prefs),which->prefs.edit().putInt("renderer",which).apply());
                Ui.toggle(this,content,"Separate GPU thread","Run rendering alongside CPU emulation. Applies to the next game.",prefs.getBoolean("threaded",true),on->prefs.edit().putBoolean("threaded",on).apply());
                Ui.choice(this,content,"On-screen display",Osd.MODES,Osd.mode(prefs),which->prefs.edit().putInt("osdMode",which).putBoolean("stats",which!=0).apply());
                help("Off is the default. Minimal shows speed and video frame rate; Full adds audio, timing and renderer details. Change this in the game menu or with the OSD hotkey.");break;
            case CONTROLLERS_TAB:renderControllerOptions(content,scroll);break;
            case 3:
                Ui.toggle(this,content,"Download box art","Find covers by title, region and game ID, then cache them on this device. Downloads stop while a game is running.",prefs.getBoolean("coverDownloads",true),on->prefs.edit().putBoolean("coverDownloads",on).apply());break;
            case 4:
                Ui.toggle(this,content,"Fast boot","Skip the BIOS startup animation when launching games. Off by default. Applies to the next game.",prefs.getBoolean("fastBoot",false),on->prefs.edit().putBoolean("fastBoot",on).apply());
                link("PlayStation BIOS  ›",BiosActivity.class);break;
            case 5:
                content.addView(Ui.heading(this,"PSX Lab  ·  "+Ui.version(this),18));Ui.gap(content,8);
                help("CPU   ARM64 recompiler     •     Native GTE & sound");
                help("An original PlayStation emulator in development. Compatibility is incomplete. BIOS and games are supplied by you. CHD, ISO, BIN, IMG and PS-X EXE files and XA CD audio are supported; two persistent memory cards and five save-state slots per game are supported.");break;
        }
        wireTabFocus();
    }
    private void wireTabFocus(){for(int i=0;i<content.getChildCount();i++){View option=content.getChildAt(i);if(option instanceof Spinner||option instanceof Button){option.setId(View.generateViewId());option.setNextFocusUpId(tabs[selectedTab].getId());for(Button tab:tabs)tab.setNextFocusDownId(option.getId());break;}}}
    @Override protected void rebuildControllerOptions(){if(content==null||selectedTab!=CONTROLLERS_TAB)return;int focused=content.indexOfChild(getCurrentFocus());content.removeAllViews();renderControllerOptions(content,scroll);wireTabFocus();if(focused>=0&&focused<content.getChildCount()){View option=content.getChildAt(focused);if(option.isFocusable())option.post(()->{option.requestFocusFromTouch();option.requestRectangleOnScreen(new android.graphics.Rect(0,0,option.getWidth(),option.getHeight()),true);});}}
    private void help(String text){content.addView(Ui.text(this,text,12,Ui.MUTED));Ui.gap(content,12);}
    private void link(String label,Class<?> page){Button b=Ui.outlineButton(this,label,false);b.setOnClickListener(v->startActivity(new Intent(this,page)));content.addView(b,new LinearLayout.LayoutParams(-1,Ui.dp(this,48)));Ui.gap(content,12);}
}
