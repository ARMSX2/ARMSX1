// SPDX-License-Identifier: GPL-3.0-or-later
package com.nanodata.armsx;

/** Compatibility entry point. The normal dev Compose activity owns emulation; no old-core SDL entry point. */
public class EmulatorActivity extends android.app.Activity {
    public static final String EXTRA_NATIVE_ARGS = "com.nanodata.armsx.EXTRA_NATIVE_ARGS";
    @Override public void onCreate(android.os.Bundle state) {
        super.onCreate(state);
        String path = getIntent().getDataString();
        String[] args = getIntent().getStringArrayExtra("com.nanodata.armsx.EXTRA_NATIVE_ARGS");
        if (path == null && args != null) {
            for (String arg : args) if (arg != null && !arg.startsWith("--")) path = arg;
        }
        android.content.Intent next = new android.content.Intent(this, com.armsx2.Main.class);
        if (path != null && !path.isEmpty()) {
            next.setAction(android.content.Intent.ACTION_VIEW);
            next.setData(android.net.Uri.parse(path.startsWith("/") ? "file://" + path : path));
        } else android.widget.Toast.makeText(this, "Use Boot BIOS in the app menu.", android.widget.Toast.LENGTH_LONG).show();
        startActivity(next);
        finish();
    }
}
