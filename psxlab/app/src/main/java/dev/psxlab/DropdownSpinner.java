package dev.psxlab;

import android.content.Context;
import android.graphics.Rect;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.os.Handler;
import android.os.Looper;
import android.view.*;
import android.widget.*;

/** A screen-sized dropdown with controller navigation and scrolling for long lists. */
final class DropdownSpinner extends Spinner {
    private PopupWindow popup;
    private ListView options;
    private final Handler handler=new Handler(Looper.getMainLooper());
    private int stickDirection;
    DropdownSpinner(Context context){super(context,Spinner.MODE_DROPDOWN);}

    @Override public boolean performClick(){
        if(!isEnabled()||getAdapter()==null)return false;
        if(popup!=null&&popup.isShowing())return true;
        SpinnerAdapter values=getAdapter();
        BaseAdapter rows=new BaseAdapter(){
            public int getCount(){return values.getCount();}
            public Object getItem(int at){return values.getItem(at);}
            public long getItemId(int at){return values.getItemId(at);}
            public View getView(int at,View old,ViewGroup parent){return values.getDropDownView(at,old,parent);}
        };
        ListView list=new ListView(getContext());options=list;int inset=Ui.dp(getContext(),1);list.setPadding(inset,inset,inset,inset);list.setAdapter(rows);list.setDivider(null);list.setSelector(new ColorDrawable(Color.TRANSPARENT));list.setChoiceMode(AbsListView.CHOICE_MODE_SINGLE);list.setFocusableInTouchMode(true);
        popup=new PopupWindow(list,1,1,true);popup.setAnimationStyle(0);popup.setEnterTransition(null);popup.setExitTransition(null);popup.setInputMethodMode(PopupWindow.INPUT_METHOD_NOT_NEEDED);popup.setOutsideTouchable(true);
        popup.setBackgroundDrawable(Ui.border(Ui.PANEL,Ui.ACCENT,8,getContext()));
        Rect screen=new Rect();getWindowVisibleDisplayFrame(screen);int margin=Ui.dp(getContext(),8);screen.inset(margin,margin);
        int width=Math.max(1,Math.min(Math.min(getWidth(),Ui.dp(getContext(),400)),screen.width())),height=Ui.dp(getContext(),4);
        int[] rowHeights=new int[rows.getCount()];
        FrameLayout measureParent=new FrameLayout(getContext());
        for(int i=0;i<rowHeights.length;i++){View row=rows.getView(i,null,measureParent);row.measure(MeasureSpec.makeMeasureSpec(width,MeasureSpec.EXACTLY),MeasureSpec.makeMeasureSpec(0,MeasureSpec.UNSPECIFIED));rowHeights[i]=row.getMeasuredHeight();height+=rowHeights[i];}
        int bounded=Ui.dp(getContext(),4);for(int rowHeight:rowHeights){if(bounded+rowHeight>screen.height())break;bounded+=rowHeight;}
        height=Math.min(height,Math.min(Math.max(1,screen.height()),Math.max(Ui.dp(getContext(),48),bounded)));
        int[] location=new int[2];getLocationOnScreen(location);int gap=Ui.dp(getContext(),4),below=location[1]+getHeight()+gap,above=location[1]-height-gap;
        int top=below+height<=screen.bottom?below:above>=screen.top?above:Math.max(screen.top,Math.min(below,screen.bottom-height));
        int left=Math.max(screen.left,Math.min(location[0],screen.right-width));
        popup.setWidth(width);popup.setHeight(height);
        list.setOnItemClickListener((parent,view,position,id)->choose(position));popup.setOnDismissListener(()->{stopStick();popup=null;options=null;});
        list.setOnKeyListener((view,key,event)->{
            if(key==KeyEvent.KEYCODE_BUTTON_A){if(event.getAction()==KeyEvent.ACTION_UP)choose(list.getSelectedItemPosition());return true;}
            if(key==KeyEvent.KEYCODE_BUTTON_B){if(event.getAction()==KeyEvent.ACTION_UP&&popup!=null)popup.dismiss();return true;}return false;
        });
        list.setOnGenericMotionListener((view,event)->{
            if(!event.isFromSource(InputDevice.SOURCE_JOYSTICK)||event.getActionMasked()!=MotionEvent.ACTION_MOVE)return false;
            float y=event.getAxisValue(MotionEvent.AXIS_HAT_Y);if(Math.abs(y)<.5f)y=event.getAxisValue(MotionEvent.AXIS_Y);
            int next=Math.abs(y)<(stickDirection==0?.55f:.35f)?0:y<0?-1:1;
            if(next!=stickDirection){stopStick();stickDirection=next;if(next!=0){stepStick();handler.postDelayed(repeatStick,350);}}return true;
        });
        popup.showAsDropDown(this,left-location[0],top-location[1]-getHeight(),Gravity.LEFT);list.requestFocus();list.setSelection(getSelectedItemPosition());list.setItemChecked(getSelectedItemPosition(),true);
        return true;
    }
    private void choose(int at){if(at>=0&&at<getCount())setSelection(at);if(popup!=null)popup.dismiss();}
    private void stepStick(){if(options==null)return;int at=options.getSelectedItemPosition();options.setSelection(Math.max(0,Math.min(getCount()-1,(at<0?0:at)+stickDirection)));}
    private final Runnable repeatStick=new Runnable(){public void run(){if(popup==null||!popup.isShowing()||stickDirection==0){stopStick();return;}stepStick();handler.postDelayed(this,120);}};
    private void stopStick(){stickDirection=0;handler.removeCallbacks(repeatStick);}
    @Override public boolean onTouchEvent(MotionEvent event){if(!isEnabled())return false;switch(event.getActionMasked()){case MotionEvent.ACTION_DOWN:setPressed(true);return true;case MotionEvent.ACTION_UP:boolean click=isPressed()&&event.getX()>=0&&event.getX()<getWidth()&&event.getY()>=0&&event.getY()<getHeight();setPressed(false);if(click)performClick();return true;case MotionEvent.ACTION_CANCEL:setPressed(false);return true;default:return true;}}
    @Override protected void onDetachedFromWindow(){if(popup!=null)popup.dismiss();stopStick();super.onDetachedFromWindow();}
}
