package com.nanodata.armsx;

import android.content.Intent;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.util.Log;

import androidx.annotation.Nullable;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;

public class EmulatorActivity extends SDLActivity {
    static final String EXTRA_NATIVE_ARGS = "com.nanodata.armsx.EXTRA_NATIVE_ARGS";
    private static final String TAG = "EmulatorActivity";

    private static native void nativeEnqueueLaunchArgument(String argument);

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);

        String launchArgument = launchArgumentFromIntent(intent);
        if (launchArgument != null && !launchArgument.isEmpty()) {
            Log.i(TAG, "Runtime protocol launch: " + launchArgument);
            nativeEnqueueLaunchArgument(launchArgument);
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[]{"SDL2", "armsx"};
    }

    @Override
    protected String getMainFunction() {
        return "external_main";
    }

    @Override
    protected String[] getArguments() {
        List<String> args = new ArrayList<>();
        args.add("armsx");

        String biosPath = copyBundledBios();
        if (biosPath != null) {
            args.add("--bios");
            args.add(biosPath);
        }

        Intent intent = getIntent();
        String[] extras = intent != null ? intent.getStringArrayExtra(EXTRA_NATIVE_ARGS) : null;
        if (extras != null) {
            for (String extra : extras) {
                if (extra != null && !extra.isEmpty()) {
                    args.add(extra);
                }
            }
        }

        String launchArgument = launchArgumentFromIntent(intent);
        if (launchArgument != null && !launchArgument.isEmpty()) {
            args.add(launchArgument);
        }

        return args.toArray(new String[0]);
    }

    private @Nullable String launchArgumentFromIntent(@Nullable Intent intent) {
        if (intent == null) {
            return null;
        }

        String dataString = intent.getDataString();
        if (dataString != null && !dataString.isEmpty()) {
            return dataString;
        }

        return null;
    }

    private @Nullable String copyBundledBios() {
        AssetManager assets = getAssets();
        try (InputStream input = assets.open("bios.bin")) {
            File output = new File(getFilesDir(), "bios.bin");
            if (output.exists() && !output.delete()) {
                Log.w(TAG, "Unable to delete stale bios.bin");
            }
            try (FileOutputStream fos = new FileOutputStream(output)) {
                byte[] buffer = new byte[16 * 1024];
                int read;
                while ((read = input.read(buffer)) != -1) {
                    fos.write(buffer, 0, read);
                }
                fos.flush();
            }
            return output.getAbsolutePath();
        } catch (IOException missing) {
            Log.i(TAG, "Bundled bios.bin not present, continuing without it");
            return null;
        }
    }
}
