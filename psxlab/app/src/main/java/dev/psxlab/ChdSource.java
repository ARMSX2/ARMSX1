package dev.psxlab;
import dev.psxlab.core.ByteSource;
import dev.psxlab.core.EngineLog;
import android.os.ParcelFileDescriptor;
import java.io.IOException;
public final class ChdSource implements ByteSource {
    private final EngineLog journal;
    static { System.loadLibrary("psxdisc"); }
    private long handle;
    public ChdSource(ParcelFileDescriptor descriptor) throws IOException {this(descriptor,null);}
    public ChdSource(ParcelFileDescriptor descriptor,EngineLog journal) throws IOException {this.journal=journal;handle=open(descriptor.getFd());}
    private static native long open(int fd) throws IOException;
    private static native void readNative(long handle,long offset,byte[] out) throws IOException;
    private static native void closeNative(long handle);
    private static native long lengthNative(long handle);
    @Override public synchronized long length()throws IOException{if(handle==0)throw new IOException("Disc is closed");return lengthNative(handle);}
    @Override public synchronized byte[] read(long offset,int length) throws IOException {
        if(handle==0)throw new IOException("Disc is closed");
        if(length<0 || length>4*1024*1024)throw new IOException("Disc read too large");
        try(EngineLog.Span span=journal==null?null:journal.watch("cd.chd.read",EngineLog.DEBUG,5_000_000L)){byte[] b=new byte[length];readNative(handle,offset,b);return b;}
    }
    @Override public synchronized void close(){if(handle!=0){closeNative(handle);handle=0;}}
}
