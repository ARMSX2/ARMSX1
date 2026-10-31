// SPDX-License-Identifier: GPL-3.0-or-later
package kr.co.iefriends.pcsx2;

import android.content.Context;
import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import org.junit.Test;
import org.junit.runner.RunWith;
import java.lang.ref.WeakReference;
import static org.junit.Assert.*;

@RunWith(AndroidJUnit4.class)
public class SwanAchievementsTransportTest {
    @Test public void refusesUnsafeDestinationsWithoutSendingCredentials() {
        for(String url:new String[]{"http://retroachievements.org/dorequest.php",
                "https://example.com/", "https://retroachievements.org.example.com/",
                "https://retroachievements.org:444/", "https://user@retroachievements.org/"}) {
            assertArrayEquals(new String[]{"-1",""}, SwanAchievementsTransport.request(url,"",""));
        }
    }
    @Test public void optionsPersistWithoutChangingAccount() {
        Context context=InstrumentationRegistry.getInstrumentation().getTargetContext();
        NativeApp.mContext=new WeakReference<>(context);
        String key="test-only-option";
        android.content.SharedPreferences preferences=context.getSharedPreferences("swan-ra",Context.MODE_PRIVATE);
        try {
            assertEquals(17,SwanAchievementsTransport.optionValue(key,17));
            SwanAchievementsTransport.option(key,23);
            assertEquals(23,SwanAchievementsTransport.optionValue(key,17));
        } finally { preferences.edit().remove("option."+key).commit(); }
    }

    @Test public void positionedIndicatorsUpdateHideAndClear() {
        android.app.Instrumentation instrumentation=InstrumentationRegistry.getInstrumentation();
        NativeApp.mContext=new WeakReference<>(instrumentation.getTargetContext());
        android.content.SharedPreferences preferences=instrumentation.getTargetContext().getSharedPreferences("swan-ra",Context.MODE_PRIVATE);
        String key="option.overlayPosition";boolean had=preferences.contains(key);int previous=preferences.getInt(key,8);
        com.armsx2.ui.GameOsd osd=com.armsx2.ui.GameOsd.INSTANCE;
        try {
            SwanAchievementsTransport.option("overlayPosition",4);
            SwanAchievementsTransport.event(1,987654,"Test challenge",true);
            SwanAchievementsTransport.event(2,987654,"Test tracker",true);
            instrumentation.waitForIdleSync();
            instrumentation.runOnMainSync(()->{
                assertEquals(2,osd.getAchievementIndicators().size());
                assertEquals(4,osd.getAchievementIndicators().get(0).getPosition());
            });
            SwanAchievementsTransport.event(1,987654,"Updated challenge",true);
            SwanAchievementsTransport.event(2,987654,"",false);
            instrumentation.waitForIdleSync();
            instrumentation.runOnMainSync(()->{
                assertEquals(1,osd.getAchievementIndicators().size());
                assertEquals("Updated challenge",osd.getAchievementIndicators().get(0).getText());
                osd.achievementNote("Position test only",1000,9);
                assertEquals(9,osd.getNotes().get(osd.getNotes().size()-1).getPosition());
                osd.getNotes().removeIf(note->note.getText().equals("Position test only"));
            });
            SwanAchievementsTransport.event(3,0,"",false);instrumentation.waitForIdleSync();
            instrumentation.runOnMainSync(()->assertTrue(osd.getAchievementIndicators().isEmpty()));
        } finally {
            instrumentation.runOnMainSync(osd::clearAchievementIndicators);
            if(had)preferences.edit().putInt(key,previous).commit();else preferences.edit().remove(key).commit();
        }
    }

    @Test public void customSoundRejectsPathsOutsidePrivateFiles() throws Exception {
        Context context=InstrumentationRegistry.getInstrumentation().getTargetContext();
        NativeApp.mContext=new WeakReference<>(context);
        android.content.SharedPreferences preferences=context.getSharedPreferences("swan-ra",Context.MODE_PRIVATE);
        boolean had=preferences.contains("sound");String previous=preferences.getString("sound","");
        java.io.File privateSound=java.io.File.createTempFile("swan-sound-",".wav",context.getFilesDir());
        java.io.File outside=java.io.File.createTempFile("swan-sound-",".wav",context.getCacheDir());
        try {
            SwanAchievementsTransport.unlockSound(privateSound.getAbsolutePath());
            assertEquals(privateSound.getAbsolutePath(),preferences.getString("sound",""));
            SwanAchievementsTransport.unlockSound(outside.getAbsolutePath());
            assertEquals(privateSound.getAbsolutePath(),preferences.getString("sound",""));
            SwanAchievementsTransport.unlockSound("");assertEquals("",preferences.getString("sound",""));
        } finally {
            if(had)preferences.edit().putString("sound",previous).commit();else preferences.edit().remove("sound").commit();
            privateSound.delete();outside.delete();
        }
    }
}
