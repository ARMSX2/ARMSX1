package dev.psxlab;

import android.content.*;
import android.graphics.*;
import android.net.Uri;
import android.os.*;
import android.util.LruCache;
import dev.psxlab.core.*;
import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.Consumer;

/** Library-only artwork fetcher. It owns no emulation resources and stops on game launch. */
final class CoverArt implements AutoCloseable {
    private final Context context;
    private final Handler main=new Handler(Looper.getMainLooper());
    private final File folder;
    private final SharedPreferences prefs;
    private final ThreadPoolExecutor workers=new ThreadPoolExecutor(2,2,30,TimeUnit.SECONDS,new LinkedBlockingQueue<>(64),r->new Thread(()->{android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_BACKGROUND);r.run();},"PSX-Covers"));
    private final LruCache<String,Bitmap> memory=new LruCache<>(16*1024*1024){protected int sizeOf(String k,Bitmap b){return b.getAllocationByteCount();}};
    private final Map<String,Request> pending=new HashMap<>();
    private final Set<HttpURLConnection> connections=ConcurrentHashMap.newKeySet();
    private volatile boolean active=true;
    private int epoch;
    private CoverMatcher matcher;
    private static final class Request{final ArrayList<Consumer<Bitmap>> callbacks=new ArrayList<>();Future<?> task;}
    CoverArt(Context c){context=c.getApplicationContext();folder=new File(c.getFilesDir(),"boxart");folder.mkdirs();prefs=c.getSharedPreferences("BoxArt",0);workers.allowCoreThreadTimeOut(true);}
    static String key(Uri uri){try{byte[] digest=MessageDigest.getInstance("SHA-256").digest(uri.toString().getBytes(StandardCharsets.UTF_8));StringBuilder out=new StringBuilder();for(byte b:digest)out.append(String.format(Locale.ROOT,"%02x",b&255));return out.toString();}catch(Exception e){throw new IllegalStateException(e);}}
    void resume(){active=true;}
    synchronized void pause(){active=false;epoch++;for(Request r:pending.values())if(r.task!=null)r.task.cancel(true);pending.clear();workers.getQueue().clear();for(HttpURLConnection c:connections)c.disconnect();}
    synchronized void load(String name,Uri uri,Consumer<Bitmap> ready){
        String key=key(uri);Bitmap cached=memory.get(key);if(cached!=null){ready.accept(cached);return;}
        if(!active)return;
        Request old=pending.get(key);if(old!=null){old.callbacks.add(ready);return;}
        Request request=new Request();request.callbacks.add(ready);pending.put(key,request);int token=epoch;
        try{request.task=workers.submit(()->{
            Bitmap bitmap=null;
            try{
                File custom=new File(new File(folder,"custom"),key+".img"),file=custom.isFile()?custom:new File(folder,key+".img");
                if(file.isFile()){bitmap=decode(read(new FileInputStream(file),8*1024*1024));file.setLastModified(System.currentTimeMillis());}
                if(bitmap==null && context.getSharedPreferences("MainActivity",0).getBoolean("coverDownloads",true) && prefs.getLong("miss_"+key,0)<System.currentTimeMillis()-86400000L){
                    byte[] bytes=lookup(name,uri,key);
                    if(bytes!=null){bitmap=decode(bytes);if(bitmap!=null){synchronized(CoverArt.this){check();if(token!=epoch)throw new InterruptedIOException("Artwork request expired");store(file,bytes);}trim();}}
                    if(bitmap==null){check();prefs.edit().putLong("miss_"+key,System.currentTimeMillis()).apply();}
                }
            }catch(Exception ignored){}finally{
                Bitmap result=bitmap;
                main.post(()->{synchronized(CoverArt.this){if(token!=epoch || !active || pending.get(key)!=request)return;pending.remove(key);}if(result!=null)memory.put(key,result);for(Consumer<Bitmap> callback:request.callbacks)callback.accept(result);});
            }
        });}catch(RejectedExecutionException e){pending.remove(key);}
    }
    private synchronized CoverMatcher catalog()throws IOException{
        if(matcher==null){ArrayList<String> names=new ArrayList<>();try(BufferedReader r=new BufferedReader(new InputStreamReader(context.getAssets().open("ps1-boxart.txt"),StandardCharsets.UTF_8))){String s;while((s=r.readLine())!=null)names.add(s);}matcher=new CoverMatcher(names);}return matcher;
    }
    private byte[] lookup(String name,Uri uri,String key)throws Exception{
        check();String serial=CoverMatcher.serial(name);boolean onlineFailure=false;
        if(serial!=null)try{byte[] bytes=fetch(serialUrl(serial));if(bytes!=null)return bytes;}catch(IOException e){onlineFailure=true;}
        for(String candidate:catalog().find(name)){
            check();try{byte[] bytes=fetch("https://raw.githubusercontent.com/libretro-thumbnails/Sony_-_PlayStation/master/Named_Boxarts/"+URLEncoder.encode(candidate,"UTF-8").replace("+","%20"));if(bytes!=null)return bytes;}catch(IOException e){onlineFailure=true;}
        }
        check();
        if(serial==null){serial=prefs.getString("serial_"+key,null);if(serial==null && !name.toLowerCase(Locale.ROOT).endsWith(".exe")){
            try(ParcelFileDescriptor fd=context.getContentResolver().openFileDescriptor(uri,"r")){
                if(fd!=null)try(ByteSource source=name.toLowerCase(Locale.ROOT).endsWith(".chd")?new ChdSource(fd):new FileSource(new ParcelFileDescriptor.AutoCloseInputStream(ParcelFileDescriptor.dup(fd.getFileDescriptor())))){
                    IsoDisc disc=new IsoDisc(source);disc.bootExecutable();serial=CoverMatcher.serial(disc.bootPath);
                    if(serial!=null)prefs.edit().putString("serial_"+key,serial).apply();
                }
            }catch(IOException | RuntimeException ignored){}
        }
        if(serial!=null){check();try{byte[] bytes=fetch(serialUrl(serial));if(bytes!=null)return bytes;}catch(IOException e){onlineFailure=true;}}
        }
        if(onlineFailure)throw new IOException("Artwork source unavailable");return null;
    }
    private static String serialUrl(String serial){return "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/default/"+serial+".jpg";}
    private byte[] fetch(String url)throws IOException{
        check();HttpURLConnection c=(HttpURLConnection)new URL(url).openConnection();connections.add(c);
        try{c.setConnectTimeout(5000);c.setReadTimeout(5000);c.setRequestProperty("User-Agent","PSXLab-Covers/0.11.5");c.setInstanceFollowRedirects(false);int status=c.getResponseCode();if(status==404)return null;if(status!=200)throw new IOException("Cover response "+status);if(c.getContentLengthLong()>4*1024*1024)throw new IOException("Cover exceeds size limit");byte[] bytes=read(c.getInputStream(),4*1024*1024);return decode(bytes)==null?null:bytes;}finally{connections.remove(c);c.disconnect();}
    }
    private void check()throws InterruptedIOException{if(!active || Thread.currentThread().isInterrupted())throw new InterruptedIOException("Artwork paused");}
    private byte[] read(InputStream source,int maximum)throws IOException{try(InputStream in=source;ByteArrayOutputStream out=new ByteArrayOutputStream()){byte[] chunk=new byte[8192];int n;while((n=in.read(chunk))!=-1){check();if(out.size()+n>maximum)throw new IOException("Image exceeds size limit");out.write(chunk,0,n);}return out.toByteArray();}}
    private static Bitmap decode(byte[] bytes){BitmapFactory.Options o=new BitmapFactory.Options();o.inJustDecodeBounds=true;BitmapFactory.decodeByteArray(bytes,0,bytes.length,o);if(o.outWidth<=0 || o.outHeight<=0 || o.outWidth>16384 || o.outHeight>16384)return null;o.inSampleSize=1;while(Math.max(o.outWidth,o.outHeight)/o.inSampleSize>512)o.inSampleSize*=2;o.inJustDecodeBounds=false;return BitmapFactory.decodeByteArray(bytes,0,bytes.length,o);}
    private static void store(File target,byte[] bytes)throws IOException{File temp=new File(target.getPath()+".tmp");try(FileOutputStream out=new FileOutputStream(temp)){out.write(bytes);}if(!temp.renameTo(target)){temp.delete();throw new IOException("Could not cache cover");}}
    private void trim(){File[] files=folder.listFiles((d,n)->n.endsWith(".img"));if(files==null)return;long total=0;for(File f:files)total+=f.length();if(total<96L*1024*1024)return;Arrays.sort(files,Comparator.comparingLong(File::lastModified));for(File f:files){long n=f.length();if(f.delete())total-=n;if(total<80L*1024*1024)break;}}
    synchronized void retry(Uri uri){String key=key(uri);prefs.edit().remove("miss_"+key).apply();memory.remove(key);new File(folder,key+".img").delete();new File(new File(folder,"custom"),key+".img").delete();}
    void importImage(Uri game,Uri image,Consumer<Boolean> done){
        pause();resume();workers.execute(()->{boolean success=false;try{byte[] bytes=read(context.getContentResolver().openInputStream(image),8*1024*1024);Bitmap bitmap=decode(bytes);if(bitmap!=null){String key=key(game);File custom=new File(folder,"custom");if(!custom.isDirectory()&&!custom.mkdirs())throw new IOException("Cannot create cover folder");store(new File(custom,key+".img"),bytes);memory.put(key,bitmap);prefs.edit().remove("miss_"+key).apply();success=true;}}catch(Exception ignored){}boolean result=success;main.post(()->done.accept(result));});
    }
    @Override public void close(){pause();workers.shutdownNow();memory.evictAll();}
}
