package dev.psxlab;

import android.app.Activity;
import android.view.*;

/** Shared controller support for the library and settings pages. */
public abstract class MenuActivity extends Activity {
    private final MenuNavigation navigation=new MenuNavigation(this);
    @Override public boolean dispatchKeyEvent(KeyEvent event){return navigation.key(event)||super.dispatchKeyEvent(event);}
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event){return navigation.motion(event)||super.dispatchGenericMotionEvent(event);}
    protected boolean navigateMenuDirection(KeyEvent event){return false;}
    @Override protected void onPause(){navigation.stop();super.onPause();}
    @Override public void onWindowFocusChanged(boolean focused){super.onWindowFocusChanged(focused);if(!focused)navigation.stop();}
}
