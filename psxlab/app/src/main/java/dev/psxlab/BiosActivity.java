package dev.psxlab;
import android.app.*;
import android.os.*;
import android.content.*;
import android.widget.*;
import android.provider.OpenableColumns;
import android.database.Cursor;
import java.io.*;
import java.util.concurrent.*;

public final class BiosActivity extends MenuActivity {
    private final ExecutorService worker=Executors.newSingleThreadExecutor();
    private TextView status;private Button choose,boot;private boolean destroyed;
    @Override public void onCreate(Bundle state){
        super.onCreate(state);LinearLayout layout=Ui.page(this,"PlayStation BIOS");((ScrollView)layout.getParent()).setFocusable(false);
        layout.addView(Ui.heading(this,"Your console firmware",20));Ui.gap(layout,8);layout.addView(Ui.text(this,"Import a raw 512 KiB PlayStation BIOS dump. It is saved inside PSX Lab for future game launches.",14,Ui.MUTED));Ui.gap(layout,20);
        status=Ui.text(this,"",14,Ui.TEXT);status.setTextIsSelectable(true);layout.addView(status);Ui.gap(layout,20);
        choose=Ui.button(this,"Choose BIOS file",true);choose.setOnClickListener(v->pick());layout.addView(choose,new LinearLayout.LayoutParams(-1,Ui.dp(this,48)));Ui.gap(layout,16);
        boot=Ui.button(this,"Run BIOS diagnostic",false);boot.setOnClickListener(v->startActivity(new Intent(this,BootActivity.class).putExtra("bios",true).putExtra("name","BIOS diagnostic")));layout.addView(boot,new LinearLayout.LayoutParams(-1,Ui.dp(this,48)));Ui.gap(layout,12);layout.addView(Ui.text(this,"A successful import checks file size and identity. Hardware and game compatibility are still in development.",12,Ui.MUTED));refresh();
    }
    private void refresh(){boolean exists=new File(getFilesDir(),"bios.bin").isFile();boot.setEnabled(exists);status.setText(exists?"Imported: "+getPreferences(0).getString("name","BIOS")+"\n512 KiB\nSHA-256: "+getPreferences(0).getString("sha","unknown"):"No BIOS selected.");}
    private void pick(){try{startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*").addCategory(Intent.CATEGORY_OPENABLE),43);}catch(ActivityNotFoundException e){status.setText("Android file picker is not available.");}}
    @Override public void onActivityResult(int request,int result,Intent intent){
        super.onActivityResult(request,result,intent);if(request!=43 || result!=RESULT_OK || intent==null || intent.getData()==null)return;
        choose.setEnabled(false);boot.setEnabled(false);status.setText("Checking and importing BIOS…");
        android.net.Uri uri=intent.getData();worker.execute(()->{
            try{
                String name="BIOS";try(Cursor c=getContentResolver().query(uri,new String[]{OpenableColumns.DISPLAY_NAME},null,null,null)){if(c!=null && c.moveToFirst())name=c.getString(0);}
                try(InputStream in=getContentResolver().openInputStream(uri)){
                    if(in==null)throw new IOException("Cannot open BIOS file");
                    BiosStore.importBios(this,in,name);
                }
                runOnUiThread(()->{if(!destroyed){choose.setEnabled(true);refresh();}});
            }catch(Exception e){runOnUiThread(()->{if(!destroyed){choose.setEnabled(true);boot.setEnabled(new File(getFilesDir(),"bios.bin").isFile());status.setText("BIOS import failed: "+e.getMessage());}});}
        });
    }
    @Override protected void onDestroy(){destroyed=true;worker.shutdown();super.onDestroy();}
}
