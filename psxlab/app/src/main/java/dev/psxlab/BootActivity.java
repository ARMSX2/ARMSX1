package dev.psxlab;



import android.app.*;

import android.os.*;

import android.net.Uri;

import android.graphics.*;

import android.view.*;

import android.widget.*;

import java.io.*;

import java.util.*;

import java.util.concurrent.*;

import dev.psxlab.core.*;



public final class BootActivity extends GamepadActivity {

    private final ExecutorService executor=Executors.newSingleThreadExecutor(r->new Thread(r,"PSX-CPU"));

    private volatile int generation;

    private TextView status;

    private TextView inputStatus;

    private Display display;

    private boolean threaded;

    private volatile AudioOutput audio;

    private long lastStatusUpdate;
    private volatile long lastPresentedSource=-1;

    private record PresentedFrame(int token,Gpu.Frame frame,String message){}

    private VsyncPresenter<PresentedFrame> presenter;

    private final Object pauseLock=new Object();
    private volatile boolean paused;
    private boolean started,leaving,showStats,showTouch;
    private volatile int osdMode;
    private LinearLayout topBar;
    private FrameLayout touchLayer;
    private AlertDialog gameMenu;
    private android.content.BroadcastReceiver audioDumpReceiver;
    private final Handler uiHandler=new Handler(Looper.getMainLooper());
    private final Runnable hideBar=()->{if(topBar!=null && (gameMenu==null || !gameMenu.isShowing())){topBar.setVisibility(View.GONE);positionOsd();}};

    @Override public void onCreate(Bundle saved){
        super.onCreate(saved);immersive();
        android.content.SharedPreferences prefs=getSharedPreferences("MainActivity",0);
        threaded=prefs.getBoolean("threaded",true);osdMode=Osd.mode(prefs);showStats=osdMode!=0;showTouch=prefs.getBoolean("touch",false);
        if(prefs.getBoolean("audioDiagnosis",false)){
            audioDumpReceiver=new android.content.BroadcastReceiver(){@Override public void onReceive(android.content.Context c,android.content.Intent i){saveAudioDiagnosis();}};
            registerReceiver(audioDumpReceiver,new android.content.IntentFilter("dev.psxlab.SAVE_AUDIO_DIAG"),android.Manifest.permission.DUMP,null,Build.VERSION.SDK_INT>=33?android.content.Context.RECEIVER_EXPORTED:0);
        }
        FrameLayout root=new FrameLayout(this);root.setBackgroundColor(Color.BLACK);
        display=new Display();display.setOnClickListener(v->{topBar.setVisibility(View.VISIBLE);positionOsd();uiHandler.removeCallbacks(hideBar);uiHandler.postDelayed(hideBar,3000);});root.addView(display,new FrameLayout.LayoutParams(-1,-1));
        touchLayer=new FrameLayout(this);addTouchControls();touchLayer.setVisibility(showTouch?View.VISIBLE:View.GONE);root.addView(touchLayer,new FrameLayout.LayoutParams(-1,-1));
        topBar=new LinearLayout(this);topBar.setGravity(Gravity.CENTER_VERTICAL);topBar.setPadding(Ui.dp(this,16),Ui.dp(this,8),Ui.dp(this,16),Ui.dp(this,8));topBar.setBackgroundColor(0xc90d111a);
        TextView title=Ui.heading(this,getIntent().getStringExtra("name")==null?"PlayStation":getIntent().getStringExtra("name").replaceFirst("(?i)\\.(chd|iso|bin|img|exe|cue)$",""),15);title.setSingleLine(true);title.setEllipsize(android.text.TextUtils.TruncateAt.END);topBar.addView(title,new LinearLayout.LayoutParams(0,-2,1));
        Button menu=Ui.button(this,"Ⅱ  Menu",false);menu.setOnClickListener(v->openMenu());topBar.addView(menu,new LinearLayout.LayoutParams(Ui.dp(this,104),Ui.dp(this,40)));root.addView(topBar,new FrameLayout.LayoutParams(-1,Ui.dp(this,56),Gravity.TOP));
        status=Ui.text(this,"Starting game…",11,Ui.TEXT);status.setTypeface(Typeface.MONOSPACE);status.setPadding(Ui.dp(this,12),Ui.dp(this,8),Ui.dp(this,12),Ui.dp(this,8));status.setBackgroundColor(0xb8000000);status.setVisibility(showStats?View.VISIBLE:View.GONE);FrameLayout.LayoutParams stats=new FrameLayout.LayoutParams(-2,-2,Gravity.TOP|Gravity.RIGHT);stats.topMargin=Ui.dp(this,64);stats.rightMargin=Ui.dp(this,8);root.addView(status,stats);
        inputStatus=new TextView(this);inputStatus.setVisibility(View.GONE);
        setContentView(root);presenter=new VsyncPresenter<>(this::drawFrame);display.setKeepScreenOn(true);uiHandler.postDelayed(hideBar,3500);
    }
    private void immersive(){getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN|View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY|View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN|View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION|View.SYSTEM_UI_FLAG_LAYOUT_STABLE);}
    @Override public void onWindowFocusChanged(boolean focused){super.onWindowFocusChanged(focused);if(focused)immersive();}
    @Override protected void onStart(){super.onStart();presenter.start();if(!started){started=true;int token=++generation;executor.execute(()->boot(token));}else if(gameMenu==null || !gameMenu.isShowing())setPaused(false);}
    @Override protected void onStop(){setPaused(true);presenter.stop();super.onStop();}
    @Override protected void onDestroy(){leaving=true;generation++;setPaused(false);uiHandler.removeCallbacksAndMessages(null);if(audioDumpReceiver!=null)unregisterReceiver(audioDumpReceiver);AudioOutput current=audio;if(current!=null)current.close();executor.shutdownNow();super.onDestroy();}
    private void setPaused(boolean value){paused=value;AudioOutput current=audio;if(current!=null)current.setPaused(value);synchronized(pauseLock){pauseLock.notifyAll();}}
    @Override public void onBackPressed(){openMenu();}
    private void positionOsd(){if(status==null || !(status.getLayoutParams() instanceof FrameLayout.LayoutParams))return;FrameLayout.LayoutParams at=(FrameLayout.LayoutParams)status.getLayoutParams();at.gravity=Gravity.TOP|Gravity.RIGHT;at.topMargin=Ui.dp(this,topBar!=null && topBar.getVisibility()==View.VISIBLE?64:8);at.rightMargin=Ui.dp(this,8);status.setLayoutParams(at);}
    private void setOsd(int mode){osdMode=mode;showStats=mode!=0;getSharedPreferences("MainActivity",0).edit().putInt("osdMode",mode).putBoolean("stats",showStats).apply();if(status!=null){status.setVisibility(showStats?View.VISIBLE:View.GONE);status.setTextSize(mode==1?12:11);positionOsd();}}
    private void openMenu(){
        if(gameMenu!=null && gameMenu.isShowing())return;setPaused(true);gamepad.clear();
        LinearLayout panel=new LinearLayout(this);panel.setOrientation(1);panel.setPadding(Ui.dp(this,24),Ui.dp(this,20),Ui.dp(this,24),Ui.dp(this,16));panel.addView(Ui.heading(this,"Game paused",22));Ui.gap(panel,16);
        ArrayList<String> labels=new ArrayList<>(Arrays.asList("Resume game","Restart game","Save state","Load state",showTouch?"Hide touch controls":"Show touch controls","Exit game"));
        for(String action:labels){Button b=Ui.button(this,action,action.equals("Resume game"));panel.addView(b,new LinearLayout.LayoutParams(-1,Ui.dp(this,40)));Ui.gap(panel,5);b.setOnClickListener(v->{
            if(action.equals("Resume game"))gameMenu.dismiss();
            else if(action.equals("Restart game")){new AlertDialog.Builder(this).setTitle("Restart this game?").setMessage("Current unsaved progress will be lost.").setPositiveButton("Restart",(d,w)->{leaving=true;gameMenu.dismiss();recreate();}).setNegativeButton("Cancel",null).show();}
            else if(action.equals("Save state"))chooseStateSlot(true);
            else if(action.equals("Load state"))chooseStateSlot(false);
            else if(action.contains("touch controls")){showTouch=!showTouch;touchLayer.setVisibility(showTouch?View.VISIBLE:View.GONE);getSharedPreferences("MainActivity",0).edit().putBoolean("touch",showTouch).apply();gameMenu.dismiss();}

            else{leaving=true;gameMenu.dismiss();finish();}
        });}
        Ui.gap(panel,8);Ui.choice(this,panel,"On-screen display",Osd.MODES,osdMode,this::setOsd);
        ScrollView menuScroll=new ScrollView(this);menuScroll.addView(panel);gameMenu=new AlertDialog.Builder(this).setView(menuScroll).create();gameMenu.setOnDismissListener(d->{if(!leaving){setPaused(false);immersive();uiHandler.postDelayed(hideBar,2000);}});gameMenu.show();if(gameMenu.getWindow()!=null){gameMenu.getWindow().setBackgroundDrawable(Ui.box(Ui.PANEL,16,this));gameMenu.getWindow().setLayout(Ui.dp(this,360),-2);}
    }
    private volatile int requestedSaveSlot=-1;
    private volatile boolean fastForward,screenshotRequested,resumeAfterSave,resumeAfterScreenshot;
    private int quickSlot(){return getSharedPreferences("MainActivity",0).getInt("stateSlot",1);}
    @Override protected void onHotkey(int action,boolean down){
        if(action==5){fastForward=down;return;}if(!down || !started)return;
        switch(action){
            case 0:openMenu();break;
            case 1:resumeAfterSave=!paused;setPaused(true);requestedSaveSlot=quickSlot();synchronized(pauseLock){pauseLock.notifyAll();}break;
            case 2:loadSlot(quickSlot());break;
            case 3:case 4:{int slot=(quickSlot()-1+(action==3?1:4))%5+1;getSharedPreferences("MainActivity",0).edit().putInt("stateSlot",slot).apply();Toast.makeText(this,"State slot "+slot,Toast.LENGTH_SHORT).show();break;}
            case 6:openMenu();new AlertDialog.Builder(this).setTitle("Restart this game?").setMessage("Current unsaved progress will be lost.").setPositiveButton("Restart",(d,w)->{leaving=true;gameMenu.dismiss();recreate();}).setNegativeButton("Cancel",null).show();break;
            case 7:resumeAfterScreenshot=!paused;setPaused(true);screenshotRequested=true;synchronized(pauseLock){pauseLock.notifyAll();}break;
            case 8:setOsd((osdMode+1)%3);break;
            case 9:showTouch=!showTouch;touchLayer.setVisibility(showTouch?View.VISIBLE:View.GONE);getSharedPreferences("MainActivity",0).edit().putBoolean("touch",showTouch).apply();break;
            case 10:pad.toggleAnalog();break;
            case 11:leaving=true;finish();break;
        }
    }
    private boolean checkingState;
    private void loadSlot(int slot){if(checkingState)return;File file=StateSlots.file(this,stateGame(),slot);if(!file.isFile()){Toast.makeText(this,"State slot "+slot+" is empty",Toast.LENGTH_SHORT).show();return;}boolean resume=!paused;setPaused(true);checkingState=true;Toast.makeText(this,"Checking state slot "+slot+"…",Toast.LENGTH_SHORT).show();
        new Thread(()->{android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND);try{if(file.length()>24L*1024*1024)throw new IOException("Save state exceeds size limit");byte[] bytes=java.nio.file.Files.readAllBytes(file.toPath());File biosFile=new File(getFilesDir(),"bios.bin");byte[] bios=biosFile.isFile()?java.nio.file.Files.readAllBytes(biosFile.toPath()):new byte[512*1024];SaveState.validate(bytes,stateGame(),bios);runOnUiThread(()->{checkingState=false;if(isDestroyed()||leaving)return;getIntent().putExtra("loadStateSlot",slot);leaving=true;if(gameMenu!=null)gameMenu.dismiss();recreate();});}catch(Exception e){runOnUiThread(()->{checkingState=false;if(isDestroyed()||leaving)return;new AlertDialog.Builder(this).setTitle("Could not load state").setMessage(e.getMessage()).setPositiveButton("OK",(d,w)->resumeAfterJob(resume)).show();});}},"PSX-StateCheck").start();
    }
    private void resumeAfterJob(boolean resume){if(resume)runOnUiThread(()->{if(!leaving && (gameMenu==null || !gameMenu.isShowing()))setPaused(false);});}
    private void screenshot(Gpu gpu)throws IOException{Gpu.Frame frame=gpu.displayFrame();Bitmap image=Bitmap.createBitmap(frame.pixels(),frame.width(),frame.height(),Bitmap.Config.ARGB_8888);String name="PSXLab-"+new java.text.SimpleDateFormat("yyyyMMdd-HHmmss-SSS",Locale.ROOT).format(new Date())+".png";
        if(Build.VERSION.SDK_INT>=29){android.content.ContentValues values=new android.content.ContentValues();values.put(android.provider.MediaStore.Images.Media.DISPLAY_NAME,name);values.put(android.provider.MediaStore.Images.Media.MIME_TYPE,"image/png");values.put(android.provider.MediaStore.Images.Media.RELATIVE_PATH,"Pictures/PSX Lab");values.put(android.provider.MediaStore.Images.Media.IS_PENDING,1);Uri uri=getContentResolver().insert(android.provider.MediaStore.Images.Media.EXTERNAL_CONTENT_URI,values);if(uri==null)throw new IOException("Cannot create screenshot");try(OutputStream out=getContentResolver().openOutputStream(uri)){if(out==null || !image.compress(Bitmap.CompressFormat.PNG,100,out))throw new IOException("Cannot write screenshot");}catch(IOException e){getContentResolver().delete(uri,null,null);throw e;}values.clear();values.put(android.provider.MediaStore.Images.Media.IS_PENDING,0);getContentResolver().update(uri,values,null,null);}
        else{File directory=getExternalFilesDir(Environment.DIRECTORY_PICTURES);if(directory==null)throw new IOException("Screenshot storage unavailable");directory.mkdirs();try(OutputStream out=new FileOutputStream(new File(directory,name))){if(!image.compress(Bitmap.CompressFormat.PNG,100,out))throw new IOException("Cannot write screenshot");}}
        image.recycle();
    }
    private String stateGame(){String uri=getIntent().getStringExtra("uri");return uri==null?"bios":uri;}
    private void chooseStateSlot(boolean save){String[] labels=new String[5];for(int i=0;i<5;i++){File file=StateSlots.file(this,stateGame(),i+1);labels[i]="Slot "+(i+1)+(file.isFile()?" · "+new java.text.SimpleDateFormat("d MMM, HH:mm",Locale.getDefault()).format(new Date(file.lastModified())):" · Empty");}
        new AlertDialog.Builder(this).setTitle(save?"Save state — choose slot":"Load state — choose slot").setItems(labels,(d,which)->{int slot=which+1;File file=StateSlots.file(this,stateGame(),slot);getSharedPreferences("MainActivity",0).edit().putInt("stateSlot",slot).apply();if(save){resumeAfterSave=false;requestedSaveSlot=slot;synchronized(pauseLock){pauseLock.notifyAll();}Toast.makeText(this,"Saving slot "+slot+"…",Toast.LENGTH_SHORT).show();}else if(file.isFile()){loadSlot(slot);}else Toast.makeText(this,"That slot is empty",Toast.LENGTH_SHORT).show();}).show();
    }
    private void saveAudioDiagnosis(){
        AudioOutput current=audio;if(current==null)return;String scene=getIntent().getStringExtra("name");
        new Thread(()->{android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND);try{File folder=current.saveDiagnosis(this,scene);android.util.Log.i("PSXCapture","Audio saved: "+folder.getAbsolutePath());runOnUiThread(()->android.widget.Toast.makeText(this,"Audio diagnosis saved",android.widget.Toast.LENGTH_SHORT).show());}catch(Exception e){android.util.Log.e("PSXCapture","Audio save failed",e);}},"PSX-AudioDump").start();
    }
    private void addTouchControls(){
        FrameLayout left=new FrameLayout(this),right=new FrameLayout(this);FrameLayout.LayoutParams l=new FrameLayout.LayoutParams(Ui.dp(this,144),Ui.dp(this,144),Gravity.BOTTOM|Gravity.LEFT);l.leftMargin=Ui.dp(this,20);l.bottomMargin=Ui.dp(this,20);touchLayer.addView(left,l);FrameLayout.LayoutParams r=new FrameLayout.LayoutParams(Ui.dp(this,144),Ui.dp(this,144),Gravity.BOTTOM|Gravity.RIGHT);r.rightMargin=Ui.dp(this,20);r.bottomMargin=Ui.dp(this,20);touchLayer.addView(right,r);
        touchButton(left,"↑",4,48,0);touchButton(left,"←",7,0,48);touchButton(left,"→",5,96,48);touchButton(left,"↓",6,48,96);
        touchButton(right,"△",12,48,0);touchButton(right,"□",15,0,48);touchButton(right,"○",13,96,48);touchButton(right,"×",14,48,96);
        LinearLayout center=new LinearLayout(this);FrameLayout.LayoutParams c=new FrameLayout.LayoutParams(-2,-2,Gravity.BOTTOM|Gravity.CENTER_HORIZONTAL);c.bottomMargin=Ui.dp(this,20);touchLayer.addView(center,c);for(int bit:new int[]{0,3}){Button b=Ui.button(this,bit==0?"Select":"Start",false);b.setAlpha(.65f);bindTouch(b,bit);center.addView(b,new LinearLayout.LayoutParams(Ui.dp(this,74),Ui.dp(this,40)));}
        for(int bit:new int[]{8,10,9,11}){Button b=Ui.button(this,bit==8?"L2":bit==10?"L1":bit==9?"R2":"R1",false);b.setAlpha(.6f);bindTouch(b,bit);FrameLayout.LayoutParams bpos=new FrameLayout.LayoutParams(Ui.dp(this,62),Ui.dp(this,40),Gravity.TOP|(bit==8 || bit==10?Gravity.LEFT:Gravity.RIGHT));bpos.topMargin=Ui.dp(this,72);if(bit==8 || bit==10)bpos.leftMargin=Ui.dp(this,bit==8?20:90);else bpos.rightMargin=Ui.dp(this,bit==9?20:90);touchLayer.addView(b,bpos);}
    }
    private void touchButton(FrameLayout group,String label,int bit,int x,int y){Button b=Ui.button(this,label,false);b.setAlpha(.6f);b.setTextSize(23);b.setPadding(0,0,0,0);FrameLayout.LayoutParams p=new FrameLayout.LayoutParams(Ui.dp(this,44),Ui.dp(this,44));p.leftMargin=Ui.dp(this,x);p.topMargin=Ui.dp(this,y);group.addView(b,p);bindTouch(b,bit);}
    private void bindTouch(Button b,int bit){b.setOnTouchListener((v,e)->{int action=e.getActionMasked();if(action==android.view.MotionEvent.ACTION_DOWN){gamepad.touch(bit,true);b.setPressed(true);return true;}if(action==android.view.MotionEvent.ACTION_UP || action==android.view.MotionEvent.ACTION_CANCEL){gamepad.touch(bit,false);b.setPressed(false);return true;}return true;});}

    private CardStorage cards;
    private Bus createBus(Gpu gpu)throws IOException{Bus bus=new Bus(gpu,pad);bus.sio0.setPad(1,pad2);cards=new CardStorage(new File(getFilesDir(),"memorycards"),bus);return bus;}
    private void boot(int token){
        try{

        if(getIntent().getBooleanExtra("bios",false)){

            try(Gpu gpu=new Gpu(threaded,DisplayConfig.renderer(getSharedPreferences("MainActivity",0)),DisplayConfig.internalScale(getSharedPreferences("MainActivity",0)))){

                Bus bus=createBus(gpu);byte[] bios=new byte[512*1024];

                try(DataInputStream in=new DataInputStream(openFileInput("bios.bin"))){in.readFully(bios);if(in.read()!=-1)throw new IOException("Invalid BIOS size");}

                bus.loadBios(bios);Cpu cpu=new Cpu(bus);cpu.reset();execute(token,cpu,bus,gpu,"BIOS reset vector BFC00000",true);

            }catch(Exception e){showError(token,e);}return;

        }

        Uri uri=Uri.parse(getIntent().getStringExtra("uri"));String name=getIntent().getStringExtra("name");

        String directory=getIntent().getStringExtra("parent");
        try(DiscMedia media=DiscMedia.open(this,uri,name,directory==null?null:Uri.parse(directory));Gpu gpu=new Gpu(threaded,DisplayConfig.renderer(getSharedPreferences("MainActivity",0)),DisplayConfig.internalScale(getSharedPreferences("MainActivity",0)))){
                Bus bus=createBus(gpu);byte[] executable=media.executable;String bootName=media.bootName;
                if(media.disc!=null)bus.cdrom.insert(media.disc);

                Cpu cpu;boolean initialized=new File(getFilesDir(),"bios.bin").isFile();

                if(initialized){

                    runOnUiThread(()->{if(token==generation)status.setText("Initializing imported BIOS kernel…");});

                    byte[] bios=new byte[512*1024];try(DataInputStream in=new DataInputStream(openFileInput("bios.bin"))){in.readFully(bios);if(in.read()!=-1)throw new IOException("Invalid BIOS size");}

                    if(getSharedPreferences("MainActivity",0).getBoolean("fastBoot",false)){
                        cpu=BiosBoot.initialize(bios,bus,()->token!=generation);bus.cdrom.prepareBootHandoff();ExeLoader.loadInto(executable,bus,cpu);bootName+=" · Fast boot";
                    }else{bus.loadBios(bios);cpu=new Cpu(bus);cpu.reset();bootName+=" · BIOS startup";}
                }else cpu=ExeLoader.load(executable,bus);

                execute(token,cpu,bus,gpu,bootName,initialized);

        }catch(Exception e){showError(token,e);}
        }finally{if(cards!=null){cards.close();cards=null;}}
    }

    private void showError(int token,Exception e){if(token==generation)runOnUiThread(()->{if(token==generation)new AlertDialog.Builder(this).setTitle("Unable to start game").setMessage(e.getMessage()).setPositiveButton("Exit game",(d,w)->finish()).show();});}

    @Override protected void onPadChanged(){if(inputStatus!=null)inputStatus.setText("Port 1: "+(pad.analogMode?"Analog":"Digital")+" · "+pad.describe());}

    private void execute(int token,Cpu cpu,Bus bus,Gpu gpu,String label,boolean bios)throws InterruptedException{
        int loadSlot=getIntent().getIntExtra("loadStateSlot",-1);getIntent().removeExtra("loadStateSlot");
        if(loadSlot>0)try{File file=StateSlots.file(this,stateGame(),loadSlot);if(file.length()>24L*1024*1024)throw new IOException("Save state exceeds size limit");SaveState.restore(java.nio.file.Files.readAllBytes(file.toPath()),cpu,bus,gpu,stateGame());}catch(IOException e){showError(token,e);return;}
        cpu.enableNative(true,true,true);

        try(AudioOutput output=new AudioOutput(!getSharedPreferences("MainActivity",0).getBoolean("timeStretch",false),getSharedPreferences("MainActivity",0).getBoolean("audioDiagnosis",false),this)){

            audio=output;output.setPaused(paused);bus.spu.setAudioSink(output);
            boolean audioDiagnosis=getSharedPreferences("MainActivity",0).getBoolean("audioDiagnosis",false);
            if(audioDiagnosis){
                bus.cdrom.setTraceSink(event->output.traceEvent(cpu.instructions,bus.spu.frames,gpu.vblanks,event));
                bus.spu.setTraceSink(event->output.traceEvent(cpu.instructions,bus.spu.frames,gpu.vblanks,event));
                gpu.setTraceSink(event->output.traceEvent(cpu.instructions,bus.spu.frames,gpu.vblanks,event));
                output.traceEvent(cpu.instructions,bus.spu.frames,gpu.vblanks,"SESSION "+label+" "+bus.cdrom.diagnostic());
            }

            long wallStart=System.nanoTime(),instructionStart=cpu.instructions,lastVblank=gpu.vblanks;


            long nextDiagnostic=0,checkedExceptions=cpu.exceptions,speedAt=wallStart,speedInstructions=instructionStart;
            double recentSpeed=1,recentFps=60;long speedVblanks=gpu.vblanks;
            long pacingWait=0;
            boolean liveTrace=getSharedPreferences("MainActivity",0).getBoolean("liveTrace",false);long traceAt=0;
            String diagnostic=label,minimalDiagnostic="Starting…";
            AudioAwareFramePacer pacer=new AudioAwareFramePacer(cpu.instructions,wallStart);boolean runningFastForward=false;

            while(token==generation && !Thread.currentThread().isInterrupted()){
                synchronized(pauseLock){while(paused && token==generation && requestedSaveSlot<0 && !screenshotRequested)pauseLock.wait();}
                if(token!=generation)break;
                if(requestedSaveSlot>0){int slot=requestedSaveSlot;requestedSaveSlot=-1;boolean resume=resumeAfterSave;resumeAfterSave=false;try{StateSlots.write(StateSlots.file(this,stateGame(),slot),SaveState.capture(cpu,bus,gpu,stateGame()));runOnUiThread(()->Toast.makeText(this,"State saved in slot "+slot,Toast.LENGTH_SHORT).show());}catch(IOException e){runOnUiThread(()->new AlertDialog.Builder(this).setTitle("Could not save state").setMessage(e.getMessage()).setPositiveButton("OK",null).show());}finally{resumeAfterJob(resume);}continue;}
                if(screenshotRequested){screenshotRequested=false;boolean resume=resumeAfterScreenshot;resumeAfterScreenshot=false;try{screenshot(gpu);runOnUiThread(()->Toast.makeText(this,"Screenshot saved",Toast.LENGTH_SHORT).show());}catch(IOException e){runOnUiThread(()->Toast.makeText(this,e.getMessage(),Toast.LENGTH_LONG).show());}finally{resumeAfterJob(resume);}continue;}
                if(fastForward!=runningFastForward){runningFastForward=fastForward;bus.spu.flushAudio();bus.spu.setAudioSink(runningFastForward?null:output);output.setPaused(runningFastForward||paused);pacer=new AudioAwareFramePacer(cpu.instructions,System.nanoTime());speedAt=System.nanoTime();speedInstructions=cpu.instructions;speedVblanks=gpu.vblanks;if(audioDiagnosis)output.traceEvent(cpu.instructions,bus.spu.frames,gpu.vblanks,runningFastForward?"FAST_FORWARD_ON_MUTED":"FAST_FORWARD_OFF");}
                if(token!=generation)break;

                cpu.runToFrame(100000);
                if(liveTrace && System.nanoTime()>=traceAt){
                    traceAt=System.nanoTime()+1000000000L;
                    StringBuilder trace=new StringBuilder(String.format(Locale.ROOT,"At %d guest clocks PC=%08X RA=%08X SP=%08X\n",cpu.instructions,cpu.pc,cpu.r[31],cpu.r[29]));
                    for(int i=0;i<32;i++)trace.append(String.format(Locale.ROOT,"r%d=%08X%s",i,cpu.r[i],i%4==3?"\n":" "));
                    if((cpu.pc&0x1fffffff)<0x200000)for(int o=-32;o<=32;o+=4)if(((cpu.pc+o)&0x1fffffff)<0x200000)trace.append(String.format(Locale.ROOT,"%08X: %08X\n",cpu.pc+o,bus.read32(cpu.pc+o)));
                    trace.append(bus.cdrom.diagnostic()).append('\n').append(bus.sio0.diagnostic()).append('\n').append(bus.mdec.diagnostic()).append('\n').append(bus.dma.diagnostic());
                    for(int ch=0;ch<7;ch++)trace.append(String.format(Locale.ROOT,"\nDMA%d %08X %08X %08X",ch,bus.dma.read(0x1f801080+ch*16),bus.dma.read(0x1f801084+ch*16),bus.dma.read(0x1f801088+ch*16)));
                    android.util.Log.i("PSXTrace",trace.toString());
                }



                if(cpu.exceptions!=checkedExceptions){
                    checkedExceptions=cpu.exceptions;
                    int vector=(cpu.cop0[12]&0x400000)!=0?0xbfc00180:0x80000080;
                    boolean empty=true;for(int offset=0;offset<16;offset+=4)if(bus.read32(vector+offset)!=0){empty=false;break;}
                    if(empty)throw new UnsupportedOperationException("CPU exception "+cpu.lastException+String.format(Locale.ROOT," at %08X has no installed kernel handler",cpu.cop0[14]));
                }
                if(gpu.vblanks!=lastVblank){

                    lastVblank=gpu.vblanks;

                    bus.flushClocks();bus.spu.flushAudio();



                    long wait=runningFastForward?0:pacer.waitNanos(cpu.instructions,System.nanoTime(),output.bufferedFrames());
                    if(wait>0){long beforeWait=System.nanoTime();TimeUnit.NANOSECONDS.sleep(wait);pacingWait+=System.nanoTime()-beforeWait;}

                    long diagnosticNow=System.nanoTime();
                    if(diagnosticNow>=nextDiagnostic){
                        nextDiagnostic=diagnosticNow+250000000;
                        if(diagnosticNow-speedAt>=1000000000L){recentSpeed=(cpu.instructions-speedInstructions)*1000000000d/(33868800d*(diagnosticNow-speedAt));recentFps=(gpu.vblanks-speedVblanks)*1000000000d/(diagnosticNow-speedAt);speedAt=diagnosticNow;speedInstructions=cpu.instructions;speedVblanks=gpu.vblanks;}
                        long audible=output.audibleSourceFrames();
                        String sync=output.synchronizationDiagnostic()+(audible>=0 && lastPresentedSource>=0?" · picture lead "+(lastPresentedSource-audible)*1000/44100+" ms":"");
                        if(audioDiagnosis)output.traceEvent(cpu.instructions,bus.spu.frames,gpu.vblanks,"CLOCK audible_source="+audible+" last_rendered_source="+lastPresentedSource+" "+output.diagnostic()+" "+bus.cdrom.diagnostic());
                        diagnostic=label+String.format(Locale.ROOT,"\nExecuting · %.0f%% speed · %,d guest clocks · PC %08X",recentSpeed*100,cpu.instructions,cpu.pc)+"\n"+output.diagnostic()+"\n"+sync+"\nRenderer: "+gpu.renderer()+" · GTE/SPU native"+(runningFastForward?"\nFast forward · audio muted":"");
                        minimalDiagnostic=String.format(Locale.ROOT,"%.0f%% speed · %.0f FPS",recentSpeed*100,recentFps)+(runningFastForward?" · FF":"");
                        android.util.Log.d("PSXAudio",output.diagnostic()+"; "+sync+"; "+bus.mdec.diagnostic()+"; "+bus.dma.diagnostic()+"; "+gpu.renderStatistics()+"; "+cpu.executionDiagnostic());
                    }
                    String frameMessage=osdMode==1?minimalDiagnostic:diagnostic;
                    long frameSource=output.sourceFrames;
                    long frameClock=cpu.instructions,frameSpu=bus.spu.frames,frameVblank=gpu.vblanks;
                    if(audioDiagnosis)output.traceEvent(frameClock,frameSpu,frameVblank,"FRAME_REQUEST source="+frameSource);
                    gpu.requestSurfaceFrame(display.surface,display.filter,frame->{lastPresentedSource=frameSource;if(audioDiagnosis)output.traceEvent(frameClock,frameSpu,frameVblank,"GPU_READY source="+frameSource);publish(token,frame,frameMessage);});

                }

            }
        }catch(RuntimeException e){

            android.util.Log.e("PSXCore",cpu.diagnostic()+"\n"+bus.cdrom.diagnostic()+"\n"+bus.sio0.diagnostic()+"\n"+bus.mdec.diagnostic()+"\n"+bus.dma.diagnostic(),e);

            publish(token,gpu.displayFrame(),label+"\nStopped after "+cpu.instructions+" guest clocks.\n"+e.getMessage()+(cpu.faultInstruction==0?String.format(Locale.ROOT,"\nLast CPU PC %08X",cpu.pc):String.format(Locale.ROOT,"\nCPU PC %08X · instruction %08X",cpu.faultPc,cpu.faultInstruction))+"\n"+bus.spu.diagnostic()+"\nDevelopment core: game compatibility is incomplete.");

        }

        finally{bus.cdrom.setTraceSink(null);bus.spu.setTraceSink(null);gpu.setTraceSink(null);cpu.closeNative();gpu.diagnosticsEnabled=false;bus.instructionCaching=true;bus.spu.setAudioSink(null);audio=null;}

    }

    private void publish(int token,Gpu.Frame frame,String message){

        if(token==generation)presenter.offer(new PresentedFrame(token,frame,message));

    }

    private void drawFrame(PresentedFrame update){

        if(update.token()!=generation)return;

        Gpu.Frame frame=update.frame();
        if(frame==null){display.nativePresentation=true;display.hasProducedFrame=true;}
        else if(!display.nativePresentation){
            int[] pixels=frame.pixels();
            for(int pixel:pixels)if((pixel&0xffffff)!=0){display.hasProducedFrame=true;break;}
            if(display.bitmap==null || display.bitmap.getWidth()!=frame.width() || display.bitmap.getHeight()!=frame.height())display.bitmap=Bitmap.createBitmap(frame.width(),frame.height(),Bitmap.Config.ARGB_8888);
            display.bitmap.setPixels(pixels,0,frame.width(),0,0,frame.width(),frame.height());display.drawSoftware();
        }

        if(update.message().contains("\nStopped") && !leaving){leaving=true;new AlertDialog.Builder(this).setTitle("Game stopped").setMessage(update.message()).setPositiveButton("Exit game",(d,w)->finish()).show();}
        long now=System.nanoTime();if(now-lastStatusUpdate>=250000000 || update.message().contains("\nStopped")){

            lastStatusUpdate=now;status.setText(update.message()+"\nVideo: "+presenter.presented+" presented · "+presenter.replaced()+" superseded");onPadChanged();

        }

    }

    private final class Display extends SurfaceView implements SurfaceHolder.Callback {

        Bitmap bitmap;boolean hasProducedFrame,nativePresentation;volatile Surface surface;final boolean filter=getSharedPreferences("MainActivity",0).getBoolean("filter",true);private final Paint paint=new Paint();Display(){super(BootActivity.this);getHolder().addCallback(this);setDefaultFocusHighlightEnabled(false);setFocusable(false);setFocusableInTouchMode(false);paint.setFilterBitmap(getSharedPreferences("MainActivity",0).getBoolean("filter",true));}

        @Override protected void onSizeChanged(int w,int h,int oldw,int oldh){super.onSizeChanged(w,h,oldw,oldh);int height=DisplayConfig.height(DisplayConfig.mode(getSharedPreferences("MainActivity",0)));if(height>0 && w>0 && h>0)getHolder().setFixedSize(Math.max(1,Math.round((float)w*height/h)),height);else getHolder().setSizeFromLayout();}
        @Override public void surfaceCreated(SurfaceHolder holder){surface=holder.getSurface();}
        @Override public void surfaceChanged(SurfaceHolder holder,int format,int w,int h){surface=holder.getSurface();}
        @Override public void surfaceDestroyed(SurfaceHolder holder){surface=null;nativePresentation=false;}
        void drawSoftware(){
            Surface target=surface;if(target==null || !target.isValid())return;
            Canvas canvas=null;try{canvas=getHolder().lockCanvas();if(canvas!=null)drawImage(canvas);}finally{if(canvas!=null)getHolder().unlockCanvasAndPost(canvas);}
        }
        private void drawImage(Canvas c){

            c.drawColor(Color.BLACK);

            if(bitmap!=null){

                float w=Math.min(c.getWidth(),c.getHeight()*4f/3f),h=w*3/4;

                c.drawBitmap(bitmap,new Rect(0,0,bitmap.getWidth(),bitmap.getHeight()),new RectF((c.getWidth()-w)/2,(c.getHeight()-h)/2,(c.getWidth()+w)/2,(c.getHeight()+h)/2),paint);

            }

            if(!hasProducedFrame){

                paint.setColor(0xffc6d2e0);paint.setTextAlign(Paint.Align.CENTER);paint.setTextSize(20*getResources().getDisplayMetrics().scaledDensity);

                c.drawText("Starting game display…",c.getWidth()/2f,c.getHeight()/2f,paint);

                paint.setTextSize(14*getResources().getDisplayMetrics().scaledDensity);

                c.drawText("Preparing PlayStation hardware",c.getWidth()/2f,c.getHeight()/2f+48,paint);

            }

        }

    }

}
