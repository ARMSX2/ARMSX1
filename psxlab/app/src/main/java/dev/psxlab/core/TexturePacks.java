package dev.psxlab.core;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

/** Optional host texture service. Called on the GPU worker; PNG writes have a bounded worker. */
final class TexturePacks implements AutoCloseable {
    static final int CACHE_BYTES=16*1024*1024,MAX_PIXELS=4*1024*1024,MAX_FILES=8192;
    private final boolean dump,replace;
    private final File base;
    private final EngineLog log;
    private final Map<String,File> files=new HashMap<>();
    private final LinkedHashMap<String,Image> cache=new LinkedHashMap<>(16,.75f,true);
    private final Set<String> dumped=ConcurrentHashMap.newKeySet();
    private final Set<String> rejected=new HashSet<>();
    private final ThreadPoolExecutor writer;
    private short[] upload;
    private int received;
    private volatile int cacheBytes,indexed;
    private volatile long lookups,writes,hits,errors,skipped;
    private volatile String detail="Waiting for VRAM uploads";
    private volatile boolean closed;
    record Image(int width,int height,int[] argb) {}
    TexturePacks(boolean dump,boolean replace,String directory,EngineLog log)throws IOException {
        this.dump=dump;this.replace=replace;this.base=new File(directory).getCanonicalFile();this.log=log;
        if(directory.isBlank())throw new IOException("Texture folder is empty");
        if(dump){File folder=new File(base,"dump");if(!folder.isDirectory()&&!folder.mkdirs())throw new IOException("Cannot create "+folder);}
        writer=dump?new ThreadPoolExecutor(1,1,0,TimeUnit.SECONDS,new ArrayBlockingQueue<>(4),r->new Thread(r,"PSX-TextureIO"),new ThreadPoolExecutor.AbortPolicy()):null;
        if(replace){
            File folder=new File(base,"replacements");
            if(folder.isDirectory())try(var paths=Files.walk(folder.toPath(),8)){
                Iterator<Path> it=paths.iterator();int visited=0;
                while(it.hasNext()&&visited++<MAX_FILES){Path p=it.next();String name=p.getFileName().toString().toLowerCase(Locale.ROOT);
                    if(Files.isRegularFile(p,LinkOption.NOFOLLOW_LINKS)&&name.matches("vram-write-[0-9a-f]{32}\\.png"))files.putIfAbsent(name.substring(11,43),p.toFile());}
                if(it.hasNext())detail="Texture folder scan reached its 8192-entry limit";
            }
            if(files.isEmpty())detail="No vram-write-<32 hex digits>.png files found in "+folder;
            else detail="Indexed "+files.size()+" replacement images";
        }
        indexed=files.size();
        event("texture.configure",Map.of("dump",dump,"replace",replace,"directory",base.toString(),"indexed",files.size()));
    }
    // A complete transfer is at most 1 MiB. Disabled settings never allocate this service.
    Image upload(int[] words,int start,int width,int total,int mask) {
        if(closed||(!dump&&files.isEmpty()))return null;
        if(start==0){upload=new short[total];received=0;}
        if(upload==null||received!=start||upload.length!=total){upload=null;received=0;return null;}
        for(int i=0;i<words.length*2&&received<total;i++,received++)upload[received]=(short)(words[i/2]>>>((i&1)*16));
        if(received!=total)return null;
        short[] raw=upload;upload=null;received=0;
        String hash=hashNative(raw).toLowerCase(Locale.ROOT);int height=total/width;
        if(dump)dump(hash,raw,width,height);
        if(!replace)return null;
        lookups++;File file=files.get(hash);if(file==null)return null;
        // Applying a second masked transfer would overwrite protected pixels. Leave it exact.
        if((mask&2)!=0){skipped++;detail="Matched upload uses protected VRAM; replacement skipped";return null;}
        String key=hash+":"+width+":"+height;Image image=cache.get(key);
        if(image==null&&!rejected.contains(key))try{
            long started=System.nanoTime();image=decode(file,width,height);
            int size=image.argb.length*4;
            while(cacheBytes+size>CACHE_BYTES&&!cache.isEmpty()){var entry=cache.entrySet().iterator();Image old=entry.next().getValue();cacheBytes-=old.argb.length*4;entry.remove();}
            cache.put(key,image);cacheBytes+=size;
            detail="Loaded "+file.getName()+" ("+image.width+"×"+image.height+")";
            event("texture.pack.loaded",Map.of("file",file.toString(),"width",image.width,"height",image.height,"decode_ns",System.nanoTime()-started,"cache_bytes",cacheBytes,"source","TexturePacks.decode"));
        }catch(IOException|RuntimeException e){rejected.add(key);error("Cannot load "+file+": "+e.getMessage());}
        if(image!=null)hits++;return image;
    }
    static Image decode(File file,int width,int height)throws IOException {
        if(file.length()>32L*1024*1024)throw new IOException("PNG exceeds 32 MiB");
        BitmapFactory.Options options=new BitmapFactory.Options();options.inJustDecodeBounds=true;BitmapFactory.decodeFile(file.toString(),options);
        int w=options.outWidth,h=options.outHeight;
        if(w<width||h<height||w%width!=0||h%height!=0||w/width!=h/height||w/width>8||(long)w*h>MAX_PIXELS)throw new IOException("Expected a uniform 1×–8× image, at most 4 megapixels, for "+width+"×"+height);
        options.inJustDecodeBounds=false;options.inPreferredConfig=Bitmap.Config.ARGB_8888;options.inPremultiplied=false;options.inScaled=false;
        Bitmap bitmap=BitmapFactory.decodeFile(file.toString(),options);if(bitmap==null)throw new IOException("Invalid PNG");
        try{int[] pixels=new int[w*h];bitmap.getPixels(pixels,0,w,0,0,w,h);return new Image(w,h,pixels);}finally{bitmap.recycle();}
    }
    private void dump(String hash,short[] raw,int width,int height){
        if(dumped.contains(hash))return;
        if(dumped.size()>=4096){skipped++;detail="Dump limit reached (4096 images per session)";return;}
        File target=new File(new File(base,"dump"),"vram-write-"+hash.toUpperCase(Locale.ROOT)+".png");
        if(target.isFile()){dumped.add(hash);return;}
        dumped.add(hash);
        try{writer.execute(()->{
            if(closed)return;
            File temporary=new File(target+".tmp");Bitmap bitmap=null;
            try{
                int[] pixels=new int[raw.length];for(int i=0;i<raw.length;i++){int p=raw[i]&65535,r=p&31,g=(p>>>5)&31,b=(p>>>10)&31;pixels[i]=((p==0?0:(p&32768)!=0?128:255)<<24)|((r<<3|r>>>2)<<16)|((g<<3|g>>>2)<<8)|(b<<3|b>>>2);}
                bitmap=Bitmap.createBitmap(width,height,Bitmap.Config.ARGB_8888);bitmap.setPremultiplied(false);bitmap.setPixels(pixels,0,width,0,0,width,height);
                try(OutputStream out=new BufferedOutputStream(new FileOutputStream(temporary))){if(!bitmap.compress(Bitmap.CompressFormat.PNG,100,out))throw new IOException("PNG encoding failed");}
                Files.move(temporary.toPath(),target.toPath(),StandardCopyOption.REPLACE_EXISTING);writes++;detail="Dumped "+target.getName();
                event("texture.dump.written",Map.of("file",target.toString(),"width",width,"height",height));
            }catch(IOException|RuntimeException e){dumped.remove(hash);error("Cannot dump "+target+": "+e.getMessage());}
            finally{if(bitmap!=null)bitmap.recycle();try{Files.deleteIfExists(temporary.toPath());}catch(IOException ignored){}}
        });}catch(RejectedExecutionException e){dumped.remove(hash);skipped++;detail="Texture writer busy; upload skipped";}
    }
    private void event(String name,Map<String,?> data){if(log!=null)log.event(EngineLog.INFO,name,new HashMap<>(data));}
    private void error(String message){errors++;detail=message;event("texture.error",Map.of("message",message));}
    long[] metrics(){return new long[]{dump?1:0,replace?1:0,lookups,writes,hits,cacheBytes,errors,skipped,indexed};}
    String status(){return detail+" · "+base;}
    @Override public void close(){
        closed=true;upload=null;cache.clear();cacheBytes=0;files.clear();rejected.clear();
        if(writer!=null){writer.shutdownNow();boolean interrupted=false;for(;;)try{if(!writer.awaitTermination(5,TimeUnit.SECONDS))error("Texture writer is still finishing its current PNG");else break;}catch(InterruptedException e){interrupted=true;}if(interrupted)Thread.currentThread().interrupt();}
        dumped.clear();
    }
    static native String hashNative(short[] pixels);
}
