package dev.psxlab;

import android.os.Bundle;
import android.widget.*;

/** Standalone controller editor retained for diagnostic entry points. */
public final class ControllerActivity extends ControllerOptionsActivity {
    @Override public void onCreate(Bundle state){super.onCreate(state);rebuildControllerOptions();}
    @Override protected void rebuildControllerOptions(){
        LinearLayout content=Ui.page(this,"Controller options");
        renderControllerOptions(content,(ScrollView)content.getParent());
    }
}
