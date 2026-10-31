package com.armsx2;

import android.app.Activity;
import android.os.Bundle;
import android.view.*;
import android.widget.*;

/** Plain visible Surface for explicit performance tests, without library background effects. */
public final class EngineAuditActivity extends Activity implements SurfaceHolder.Callback {
    private volatile Surface surface;
    private volatile int bufferWidth,bufferHeight;
    private FrameLayout root;
    private SurfaceView view;
    private TextView fps;
    public Surface gameSurface(){return surface;}
    public int bufferWidth(){return bufferWidth;}
    public int bufferHeight(){return bufferHeight;}
    @Override public void onCreate(Bundle state){
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON|WindowManager.LayoutParams.FLAG_FULLSCREEN);
        root=new FrameLayout(this);addSurface();
        fps=new TextView(this);fps.setText("FPS --");fps.setTextColor(0xffffffff);fps.setTextSize(18);fps.setPadding(14,8,14,8);fps.setBackgroundColor(0x99000000);
        FrameLayout.LayoutParams corner=new FrameLayout.LayoutParams(-2,-2,Gravity.TOP|Gravity.RIGHT);root.addView(fps,corner);setContentView(root);
    }
    public void showFps(float value){runOnUiThread(()->fps.setText(value>0?String.format(java.util.Locale.ROOT,"%.1f FPS",value):"FPS --"));}
    public void showLabel(String text){runOnUiThread(()->fps.setText(text));}
    private void addSurface(){view=new SurfaceView(this);view.getHolder().addCallback(this);root.addView(view,0,new FrameLayout.LayoutParams(-1,-1));}
    public void resizeSurface(int width,int height){runOnUiThread(()->{bufferWidth=bufferHeight=0;view.getHolder().setFixedSize(width,height);});}
    public void recreateSurface(){runOnUiThread(()->{SurfaceView old=view;view=null;root.removeView(old);surface=null;bufferWidth=bufferHeight=0;addSurface();});}
    @Override public void surfaceCreated(SurfaceHolder holder){if(view!=null&&holder==view.getHolder())surface=holder.getSurface();}
    @Override public void surfaceChanged(SurfaceHolder holder,int format,int width,int height){if(view!=null&&holder==view.getHolder()){surface=holder.getSurface();bufferWidth=width;bufferHeight=height;}}
    @Override public void surfaceDestroyed(SurfaceHolder holder){if(view!=null&&holder==view.getHolder()){surface=null;bufferWidth=bufferHeight=0;}}
}
