package dev.psxlab;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;
import android.view.*;

/** Host menu navigation only; game input and binding capture keep their own paths. */
final class MenuNavigation {
    private final Activity activity;
    private final Handler handler=new Handler(Looper.getMainLooper());
    private int direction;
    MenuNavigation(Activity activity){this.activity=activity;}

    boolean key(KeyEvent event){
        int code=event.getKeyCode();
        int focusDirection=code==KeyEvent.KEYCODE_DPAD_UP?View.FOCUS_UP:code==KeyEvent.KEYCODE_DPAD_DOWN?View.FOCUS_DOWN:code==KeyEvent.KEYCODE_DPAD_LEFT?View.FOCUS_LEFT:code==KeyEvent.KEYCODE_DPAD_RIGHT?View.FOCUS_RIGHT:0;
        if(focusDirection!=0){
            if(activity instanceof MenuActivity&&((MenuActivity)activity).navigateMenuDirection(event))return true;
            View current=activity.getCurrentFocus();
            for(View parent=current;parent!=null;parent=parent.getParent() instanceof View?(View)parent.getParent():null)if(parent instanceof android.widget.AbsListView)return false;
            if(current instanceof android.widget.EditText&&(focusDirection==View.FOCUS_LEFT||focusDirection==View.FOCUS_RIGHT))return false;
            if(event.getAction()==KeyEvent.ACTION_DOWN){View next=FocusFinder.getInstance().findNextFocus((ViewGroup)activity.getWindow().getDecorView(),current,focusDirection);if(next!=null){next.requestFocusFromTouch();next.requestRectangleOnScreen(new android.graphics.Rect(0,0,next.getWidth(),next.getHeight()),true);}}
            return true;
        }
        if(code==KeyEvent.KEYCODE_BUTTON_A){
            if(event.getAction()==KeyEvent.ACTION_UP){View focused=activity.getCurrentFocus();if(focused instanceof android.widget.AbsListView){android.widget.AbsListView list=(android.widget.AbsListView)focused;int at=list.getSelectedItemPosition();if(at<0&&list.getCount()>0){at=0;list.setSelection(at);}if(at>=0)list.performItemClick(list.getSelectedView(),at,list.getItemIdAtPosition(at));}else if(focused!=null)focused.performClick();}
            return true;
        }
        if(code==KeyEvent.KEYCODE_BUTTON_B){if(event.getAction()==KeyEvent.ACTION_UP)activity.onBackPressed();return true;}
        return false;
    }

    boolean motion(MotionEvent event){
        if(!event.isFromSource(InputDevice.SOURCE_JOYSTICK)||event.getActionMasked()!=MotionEvent.ACTION_MOVE)return false;
        float x=event.getAxisValue(MotionEvent.AXIS_X),y=event.getAxisValue(MotionEvent.AXIS_Y);
        float hx=event.getAxisValue(MotionEvent.AXIS_HAT_X),hy=event.getAxisValue(MotionEvent.AXIS_HAT_Y);
        if(Math.abs(hx)>.5f||Math.abs(hy)>.5f){x=hx;y=hy;}
        float threshold=direction==0?.55f:.35f;
        int next=Math.max(Math.abs(x),Math.abs(y))<threshold?0:Math.abs(y)>=Math.abs(x)?(y<0?KeyEvent.KEYCODE_DPAD_UP:KeyEvent.KEYCODE_DPAD_DOWN):(x<0?KeyEvent.KEYCODE_DPAD_LEFT:KeyEvent.KEYCODE_DPAD_RIGHT);
        if(next!=direction){stop();direction=next;if(next!=0){step();handler.postDelayed(repeat,350);}}
        return true;
    }

    private void step(){
        long now=android.os.SystemClock.uptimeMillis();
        KeyEvent down=new KeyEvent(now,now,KeyEvent.ACTION_DOWN,direction,0),up=new KeyEvent(now,now,KeyEvent.ACTION_UP,direction,0);
        if(!key(down))activity.getWindow().superDispatchKeyEvent(down);
        if(!key(up))activity.getWindow().superDispatchKeyEvent(up);
    }
    private final Runnable repeat=new Runnable(){public void run(){if(direction==0||!activity.hasWindowFocus()){stop();return;}step();handler.postDelayed(this,120);}};
    void stop(){direction=0;handler.removeCallbacks(repeat);}
}
