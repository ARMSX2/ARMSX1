// SPDX-License-Identifier: GPL-3.0-or-later
package kr.co.iefriends.pcsx2;

import android.content.Context;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;
import android.os.Handler;
import android.os.Looper;
import org.json.JSONObject;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;
import javax.net.ssl.HttpsURLConnection;

/** TLS-only, bounded RA transport. Never logs passwords, tokens, requests or responses. */
public final class SwanAchievementsTransport {
    private static Context context(){return NativeApp.mContext==null?null:NativeApp.mContext.get();}
    public static int optionValue(String key,int fallback){Context ctx=context();return ctx==null?fallback:ctx.getSharedPreferences("swan-ra",Context.MODE_PRIVATE).getInt("option."+key,fallback);}
    public static void option(String key,int value){Context ctx=context();if(ctx!=null)ctx.getSharedPreferences("swan-ra",Context.MODE_PRIVATE).edit().putInt("option."+key,value).apply();}
    public static void notifyUnlock(String title,int points,int seconds){Context ctx=context();if(ctx==null)return;
        new Handler(Looper.getMainLooper()).post(()->{
            if(optionValue("enabled",1)==0)return;
            if(optionValue("notifications",1)!=0)com.armsx2.ui.GameOsd.INSTANCE.achievementNote("Achievement unlocked: "+title+" ("+points+" points)",seconds*1000L,optionValue("notificationPosition",1));
            new Thread(com.armsx2.AchievementsProgress::snapshotCurrentGame,"swan-ach-progress").start();
            if(optionValue("soundEffects",1)!=0){String sound=ctx.getSharedPreferences("swan-ra",Context.MODE_PRIVATE).getString("sound","");
                if(!sound.isEmpty())NativeApp.playSound(sound);
                else if(NativeApp.sSoundVolume>0){try {android.media.ToneGenerator tone=new android.media.ToneGenerator(android.media.AudioManager.STREAM_MUSIC,Math.round(40*Math.max(0,Math.min(1,NativeApp.sSoundVolume))));
                    tone.startTone(android.media.ToneGenerator.TONE_PROP_ACK,180);new Handler(Looper.getMainLooper()).postDelayed(tone::release,250);}catch(RuntimeException ignored){}}}
        });}
    public static void unlockSound(String path){Context ctx=context();if(ctx==null)return;
        try {java.io.File file=new java.io.File(path);if(!path.isEmpty()&&(!file.isFile()||!file.getCanonicalPath().startsWith(ctx.getFilesDir().getCanonicalPath()+"/")))return;
            ctx.getSharedPreferences("swan-ra",Context.MODE_PRIVATE).edit().putString("sound",path).apply();}catch(java.io.IOException ignored){} }
    public static void event(int kind,int id,String text,boolean visible){new Handler(Looper.getMainLooper()).post(()->{
        if(kind!=3&&optionValue("enabled",1)==0)return;
        if(kind==0){if(optionValue("leaderboardNotifications",1)!=0)com.armsx2.ui.GameOsd.INSTANCE.achievementNote(text,optionValue("leaderboardsDuration",10)*1000L,optionValue("notificationPosition",1));}
        else if(kind==3)com.armsx2.ui.GameOsd.INSTANCE.clearAchievementIndicators();
        else com.armsx2.ui.GameOsd.INSTANCE.achievementIndicator((kind==1?"challenge.":"leaderboard.")+id,visible?text:"",optionValue("overlayPosition",8));
    });}
    public static String[] request(String address,String post,String contentType) {
        HttpsURLConnection connection=null;
        try {
            if(!com.armsx2.achievements.RaSubmissionPolicy.allowed(optionValue("enabled",1)!=0,address,post))
                return new String[]{"-1",""};
            URL url=new URL(address);
            if(!url.getProtocol().equals("https")||!url.getHost().equals("retroachievements.org")||url.getPort()!=-1||url.getUserInfo()!=null)
                return new String[]{"-1",""};
            connection=(HttpsURLConnection)url.openConnection();connection.setInstanceFollowRedirects(false);
            connection.setConnectTimeout(10000);connection.setReadTimeout(10000);
            connection.setRequestProperty("User-Agent","EmuCorePSX-SwanStation/1.0");
            if(!post.isEmpty()) {byte[] bytes=post.getBytes(StandardCharsets.UTF_8);
                connection.setRequestMethod("POST");connection.setDoOutput(true);
                connection.setRequestProperty("Content-Type",contentType);
                connection.setFixedLengthStreamingMode(bytes.length);
                try(java.io.OutputStream stream=connection.getOutputStream()){stream.write(bytes);}
            }
            int status=connection.getResponseCode();
            InputStream response=status>=400?connection.getErrorStream():connection.getInputStream();
            ByteArrayOutputStream body=new ByteArrayOutputStream();
            if(response!=null)try(InputStream stream=response){byte[] block=new byte[8192];int count;
                while((count=stream.read(block))!=-1){if(body.size()+count>4*1024*1024)throw new java.io.IOException("Response too large");body.write(block,0,count);}}
            return new String[]{Integer.toString(status),body.toString("UTF-8")};
        } catch(Exception ignored){return new String[]{"-2",""};}
        finally {if(connection!=null)connection.disconnect();}
    }
    private static SecretKey key() throws Exception {
        KeyStore store=KeyStore.getInstance("AndroidKeyStore");store.load(null);
        String alias="EmuCorePSX.RA.Token.v1";
        if(!store.containsAlias(alias)) {KeyGenerator generator=KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES,"AndroidKeyStore");
            generator.init(new KeyGenParameterSpec.Builder(alias,KeyProperties.PURPOSE_ENCRYPT|KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build());generator.generateKey();}
        return (SecretKey)store.getKey(alias,null);
    }
    public static void store(String username,String token) {
        Context ctx=context();if(ctx==null)return;
        android.content.SharedPreferences preferences=ctx.getSharedPreferences("swan-ra",Context.MODE_PRIVATE);
        if(token.isEmpty()){preferences.edit().remove("token").commit();return;}
        try {Cipher cipher=Cipher.getInstance("AES/GCM/NoPadding");cipher.init(Cipher.ENCRYPT_MODE,key());
            byte[] encrypted=cipher.doFinal(new JSONObject().put("user",username).put("token",token).toString().getBytes(StandardCharsets.UTF_8));
            preferences.edit().putString("token",Base64.encodeToString(cipher.getIV(),Base64.NO_WRAP)+":"+Base64.encodeToString(encrypted,Base64.NO_WRAP)).commit();
        } catch(Exception ignored){preferences.edit().remove("token").commit();}
    }
    public static String[] restore() {
        Context ctx=context();if(ctx==null)return new String[]{"",""};
        try {String[] parts=ctx.getSharedPreferences("swan-ra",Context.MODE_PRIVATE).getString("token","").split(":");if(parts.length!=2)return new String[]{"",""};
            Cipher cipher=Cipher.getInstance("AES/GCM/NoPadding");cipher.init(Cipher.DECRYPT_MODE,key(),new GCMParameterSpec(128,Base64.decode(parts[0],Base64.NO_WRAP)));
            JSONObject value=new JSONObject(new String(cipher.doFinal(Base64.decode(parts[1],Base64.NO_WRAP)),StandardCharsets.UTF_8));
            return new String[]{value.optString("user"),value.optString("token")};
        } catch(Exception ignored){return new String[]{"",""};}
    }
}
